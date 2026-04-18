# Alias model vertex blending en GPU

## Objetivo

Mover la interpolacion de poses y el shading per-vertex de los alias models
(MDL de Quake: enemigos, armas, items) de CPU a GPU. Hoy cada draw de un alias
model interpola en CPU cada vertex entre dos keyframes y calcula un color por
vertex con una tabla precomputada de dot-products. En combate con ~300 draws
por frame esto consume el grueso de `cpu_alias` (~0.5-3 ms p50, picos hasta
varios ms).

Meta: que `R_DrawAliasModel` solo emita uniforms + drawcall. El vertex
shader hace la interpolacion de poses y el shading.

## Estado actual

Documentado en detalle en el audit del codigo. Los puntos clave:

### Estructura en memoria del modelo

`aliashdr_t` (en `gl_model.h`) guarda `posedata` como un bloque contiguo de
`trivertx_t[numposes][numverts]`. `trivertx_t` es `{byte v[3], byte lightnormalindex}`
-- posicion cuantizada + indice a la tabla de 256 normales de `anorms.h`.
La descuantizacion usa `scale` + `scale_origin` del header.

### Render per-frame

`GL_DrawAliasBlendedFrame` (en `gl_mdl.cpp`) recorre el GL command list del
modelo (strips y fans estilo Quake 1 clasico) y por cada vertex:

1. Lerp `pos = verts1->v * (1-blend) + verts2->v * blend`
2. Lerp shading usando tablas `r_avertexnormal_dots_mdl[16][256]` indexadas
   por `lightnormalindex`
3. Escribe `{x,y,z, s,t, r,g,b,a}` a un `alias_soup[]` CPU
4. Re-triangula strips/fans a triangle list
5. Al final: `DynamicVBO_Upload(alias_vbo, soup, ...)` + `glDrawArrays`

### Shader actual

`R_AliasShader` (en `gl_render.cpp`): solo MVP + texture + color per-vertex.
No sabe nada de poses ni de luz.

### Instrumentacion

`PROF_CPU_ALIAS` ya envuelve cada call. Medimos cpu_us y gpu_us por seccion.

## Diseno del nuevo pipeline

### Datos GPU por modelo (static, creados al cargar)

Por cada modelo alias, al terminar `Mod_LoadAliasModel` generamos:

- **VBO static de poses**: `numposes * numverts * 4 bytes` formato
  `{byte v[3], byte nidx}`. Contiguo. La descuantizacion la hace el shader.
- **IBO static de triangulos**: la command list actual (strips + fans) se
  pre-triangula a triangle list. `num_indices = 3 * num_triangles`.
- **Datos auxiliares**: `scale`, `scale_origin` (ya estan en aliashdr_t), y el
  array de `stverts` (texture coords per-vertex), que tambien va como VBO
  static.

Memoria extra por modelo: tipicamente `numposes * numverts * 4` (poses) +
`numverts * 8` (texcoords) + `num_indices * 2` (indices short). Para un modelo
grande como un enemigo de 500 verts x 30 poses son ~60 KB. Trivial.

### Draw call

Cada draw recibe como uniforms:

- `u_mvp` (mat4) -- ya existe
- `u_scale` (vec3), `u_origin` (vec3) -- descuantizacion
- `u_pose_a`, `u_pose_b` (int) -- offsets en el VBO, en vertices
- `u_blend` (float) -- 0..1
- `u_light_dir` (vec3) -- direccion de luz normalizada
- `u_shade_color` (vec3) -- RGB de luz
- `u_ambient` (float) -- minimo de luz
- `u_alpha` (float) -- transparencia entidad
- `u_fullbright` (int) -- 1 si el modelo es fullbright (laser/bolt/flame)
- `u_normals[256]` (vec3 uniform array) -- tabla `anorms.h` subida una vez

Y el shader hace:

```glsl
// vertex shader
int idx = gl_VertexID; // vertex index dentro del mesh triangulado
// Fetch pose A y pose B usando textureBuffer o SSBO indexado por idx
ivec3 pa = fetch_pos(u_pose_a + idx);
ivec3 pb = fetch_pos(u_pose_b + idx);
int   na = fetch_nidx(u_pose_a + idx);
int   nb = fetch_nidx(u_pose_b + idx);

vec3 pos_a = vec3(pa) * u_scale + u_origin;
vec3 pos_b = vec3(pb) * u_scale + u_origin;
vec3 pos   = mix(pos_a, pos_b, u_blend);

vec3 n_a = u_normals[na];
vec3 n_b = u_normals[nb];
vec3 n   = normalize(mix(n_a, n_b, u_blend));

float d   = max(u_ambient, dot(n, -u_light_dir));
vec3  lit = u_fullbright != 0 ? vec3(1.0) : u_shade_color * d;

v_color = vec4(lit, u_alpha);
v_tc    = a_tc;
gl_Position = u_mvp * vec4(pos, 1.0);
```

El fragment shader sigue siendo el actual (`texture * v_color`).

### Opciones para pasar las poses al shader

Dos caminos razonables:

**A) Dos attribute streams del mismo VBO con offsets**: configuro
`glVertexAttribPointer` dos veces con distintos byte offsets por draw. Cambia
state por draw pero es lo que ya hacemos para texturas/lightmaps. Requiere
`glBindVertexBuffer` + `glVertexAttribFormat` (DSA style, GL 4.3+).

**B) SSBO con todo `posedata` del modelo**: bound una vez, shader lo indexa
con `gl_VertexID + u_pose_a`. Requiere GL 4.3+ (tenemos 4.4). Mas simple de
usar, no hay que reconfigurar attributes por draw.

Eleccion: **B (SSBO)**. El shader es mas directo. No hay cambio de attribute
state por draw, solo uniforms.

## Fases

### Fase 1 -- Datos GPU por modelo

Todavia no se toca el render path. Se anade la estructura y se llena.

- Extender `aliashdr_t` con campos `gpu_vbo_texcoords`, `gpu_ssbo_poses`,
  `gpu_ibo`, `gpu_num_indices`. O struct paralela por modelo.
- En `GL_MakeAliasModelDisplayLists`: tras generar la command list, ademas
  pre-triangular a triangle list y subir los 3 buffers (texcoords VBO, poses
  SSBO, IBO) con `GL_STATIC_DRAW`.
- Modelos alias cargados tendran el VBO/SSBO creado al vuelo en el primer
  uso si no esta todavia.
- **No se cambia el render**. Solo verificar que compila, los modelos cargan,
  no crash al cargar un nivel.

Entregable: los modelos tienen sus buffers listos. El render sigue usando
el path viejo.

### Fase 2 -- Nuevo shader alias

Escribir `R_AliasShaderGPU` nuevo con GLSL 4.4 core, en paralelo al actual.

- Uniforms y layout como arriba.
- Tabla `u_normals[256]` se sube una sola vez al linkar el shader (los
  valores de `anorms.h` son constantes).
- Funcion `R_EnsureAliasShaderGPU` para compilar on-demand.
- Cachear los uniform locations.

Entregable: shader nuevo existe, compila, no se usa. El viejo sigue en uso.

### Fase 3 -- Nuevo render path

- Cvar nuevo: `r_alias_gpu` (default 0). Archived.
- Funcion `R_DrawAliasModel_GPU` (entity_t *, int cull). Misma signatura
  que la actual. Si `r_alias_gpu.value != 0`, se llama en lugar de la vieja
  desde los callers.
- Implementacion: calcular pose indices y blend igual que
  `GL_DrawAliasBlendedFrame`, calcular `light_dir/shade_color/ambient` igual
  que hoy (reutilizando `R_LightPoint`), bindear shader + SSBO + VBO + IBO
  del modelo, subir uniforms, `glDrawElements`.

Entregable: con `r_alias_gpu 1` en consola, los enemigos se ven. Visual diff
contra el path viejo:
- Poses correctas (no glitches de animacion).
- Shading parecido (dot product directo vs tabla precomputada puede dar
  pequenas diferencias; si son visibles, replicar formula exacta).
- Colores correctos bajo dlights de colores (magenta, naranja).
- Viewmodel sigue igual.

### Fase 4 -- Viewmodel y casos borde

- `R_DrawViewModel` ya llama a `R_DrawAliasModel` generico. Solo verificar
  que el depth range hack sigue fuera del shader.
- Modelos `fullbright` (laser/bolt/flame) pasan `u_fullbright=1`.
- Entities con `e->alpha < 1`: pasa `u_alpha`.
- Modelos con skin custom o colormap player (el colormap de Quake que tintaba
  el armadura/shirt/pants): verificar que el path de skin sigue funcionando.

Entregable: playthrough de un nivel completo con `r_alias_gpu 1`. Sin
regresiones visuales conocidas.

### Fase 5 -- Apagar el viejo

- Cambiar default de `r_alias_gpu` a 1.
- Dejar pasar unos dias / una iteracion.
- Eliminar:
  - `GL_DrawAliasBlendedFrame` y funciones auxiliares exclusivas.
  - `alias_soup`, `alias_vbo`.
  - Tablas `r_avertexnormal_dots_mdl[16][256]` y `shadedots_mdl/shadedots2_mdl`.
  - Includes de `anorm_dots.h`.
  - El shader `R_AliasShader` viejo (solo MVP + color) queda; el nuevo lo
    sustituye.
- Renombrar `R_AliasShaderGPU` y `R_DrawAliasModel_GPU` al nombre canonico
  (sin el sufijo).
- Eliminar el cvar `r_alias_gpu`.

Entregable: codigo mas pequeno y el pipeline nuevo es el unico.

## Medicion esperada

Antes del cambio (ad_dm1, combate):
- `cpu_alias.cpu_us.p50` ~500 us
- `cpu_alias.cpu_us.p95` ~1500 us
- `cpu_alias.cpu_us.max` varios ms en pelea

Despues:
- `cpu_alias.cpu_us.p50` < 50 us (solo setup de uniforms + drawcall)
- `cpu_alias.cpu_us.p95` < 200 us
- `cpu_alias.gpu_us.p50` sube ligeramente (trabajo que antes era CPU ahora
  es GPU), pero GPU sobra.
- Frame total no cambia si vsync on (ya estamos a techo).
- Margen CPU: +1-3 ms libres por frame en combate.

## Decisiones tomadas / trampas conocidas

- **Usamos SSBO** para las poses, no dos attribute streams. Mas simple de
  cablear. Requiere 4.4 core, lo tenemos.
- **Pre-triangulamos al cargar**. El mesh queda como triangle list + IBO.
  Mas uploads al cargar, pero zero triangulation en cada frame.
- **Tabla de normales en uniform array** en vez de UBO. 256 * vec3 = 3 KB,
  cabe de sobra. Se setea una vez por compile.
- **Mantener formula de shading vanilla**: ambient + dot(N, L), no iluminacion
  PBR. Si se nota diferencia visual vs el viejo, hay que investigar antes
  de apagar el viejo.
- **Precision**: Quake quantiza a byte/vertex. El shader descuantiza a
  float. Mismo resultado.
- **Models extra grandes**: numverts * numposes acotado por `MAXALIASVERTS`
  y `MAXALIASFRAMES`. Si algun modelo AD usa valores altos, verificar que
  el SSBO cabe (no hay limite practico en GL 4.4).

## Orden de trabajo

Fase 1 y 2 se pueden escribir sin tocar el render. Son seguras, no rompen
nada. Luego fase 3 tras cvar, se prueba en caliente. Fase 4 para cerrar casos
borde. Fase 5 cuando haya confianza.

Cada fase se cierra con una captura de `profile_live` y una foto del juego
para comparar con la anterior.
