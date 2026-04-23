# Lightmaps en el agua

Plan para aplicar lightmaps a las superficies de agua (`SURF_DRAWTURB`), que
actualmente se renderizan sin iluminación y por tanto ignoran la luz ambiental
del mapa.

## Estado actual

El agua vive en un pipeline paralelo al mundo sólido:

- **Shader paredes** ([gl_render.cpp:452-469](../src_sdl3/gl_render.cpp#L452-L469)):
  samplea diffuse `u_tex` + lightmap `u_lightmap`, multiplica
  `diffuse.rgb * lm.rgb`. Recibe `v_lmtc` del vertex shader.
- **Shader agua** ([gl_render.cpp:558-574](../src_sdl3/gl_render.cpp#L558-L574)):
  solo `u_tex`, UVs distorsionadas con senos. **No conoce lightmaps.**
- **Geometría**: `warp_vtx_t` tiene solo `pos + uv diffuse` (sin uv lightmap).
- **Build de lightmaps**
  ([gl_rsurf.cpp:1771](../src_sdl3/gl_rsurf.cpp#L1771) y
  [gl_rsurf.cpp:1816](../src_sdl3/gl_rsurf.cpp#L1816)):
  explícitamente saltan `SURF_DRAWTURB`. Las surfaces de agua nunca reciben
  `light_s`/`light_t`/`lightmaptexturenum` — quedan sin inicializar.

Dos bloqueos, en orden de fuerza:

1. Las surfaces de agua no tienen lightmap asignado en el atlas.
2. El shader de agua no samplea lightmap aunque lo tuviera.

## Decisión de diseño: warp vs lightmap

Samplear el lightmap con UVs **sin warp** (las originales, no las distorsionadas
por los senos del agua). El warp es un efecto superficial tipo ondulación
óptica; la iluminación del fondo debería ser estable. Samplear con warp
también introduce el riesgo de leer fuera del rect del surface dentro del
atlas y meter luz de otras paredes.

## Plan (7 pasos)

### 1. Asignar lightmap a agua en el build

En [gl_rsurf.cpp:1771](../src_sdl3/gl_rsurf.cpp#L1771) y
[gl_rsurf.cpp:1816](../src_sdl3/gl_rsurf.cpp#L1816), quitar el skip de
`SURF_DRAWTURB`. Verificar que `R_BuildLightMap` no peta con surfaces de
agua (puede que `samples` sea NULL si `qbsp` no les metió lighting — hay que
defender contra eso).

### 2. Extender `warp_vtx_t`

En [gl_warp.cpp:59-62](../src_sdl3/gl_warp.cpp#L59-L62): añadir
`float lm_s, lm_t`. Revisar todo
`WARP_MAX_VERTS * sizeof(warp_vtx_t)` para que la capacidad del VBO siga
correcta.

### 3. Rellenar coords lightmap en `Water_AppendSurface`

En [gl_warp.cpp:259](../src_sdl3/gl_warp.cpp#L259): portar la fórmula de
[gl_rsurf.cpp:1702-1715](../src_sdl3/gl_rsurf.cpp#L1702-L1715)
(`DotProduct` con `texinfo->vecs`, offset `light_s*16+8`, normalizar por
`BLOCK_WIDTH*16`).

### 4. Configurar attrib location=2 en el VAO del agua

En [gl_warp.cpp:84-85](../src_sdl3/gl_warp.cpp#L84-L85):

```cpp
DynamicVBO_SetAttrib(&warp_vbo, 2, 2, GL_FLOAT, GL_FALSE,
                     sizeof(warp_vtx_t), offsetof(warp_vtx_t, lm_s));
```

### 5. Actualizar GLSL del agua

En [gl_render.cpp:526-574](../src_sdl3/gl_render.cpp#L526-L574):

- VS: añadir `layout(location = 2) in vec2 a_lmtc;` y varying `v_lmtc`.
- FS: añadir `uniform sampler2D u_lightmap;` y
  `vec4 lm = texture(u_lightmap, v_lmtc); c.rgb *= lm.rgb;`.
- **No warpear** `v_lmtc`.

### 6. Bucketizar surfaces por (textura, lightmap page)

En [R_DrawWaterSurfaces](../src_sdl3/gl_rsurf.cpp#L731): actualmente agrupa
solo por textura. Ahora cada surface con `lightmaptexturenum` distinto
necesita su propio draw call (o al menos su propio bind de TU1). Patrón
idéntico al que ya hace `R_EmitTextureChains` para paredes.

### 7. Bind TU1 + uniform sampler

En el loop de draw:

```cpp
glActiveTexture(GL_TEXTURE1);
glBindTexture(GL_TEXTURE_2D, lightmap_textures[lmnum]);
glUniform1i(R_WorldWaterShader_u_lightmap, 1);
```

antes de entrar al loop.

## Constraint confirmado: los BSP antiguos no tienen lighting en agua

Los mapas de Quake originales **no contienen `samples` para caras
`SURF_DRAWTURB`**. `qbsp` histórico saltaba esas caras porque el renderer
nunca las usaba. Esto invalida el plan "directo" anterior: aunque metamos
los pasos 1–7, el lightmap resultante sería luz plana (o basura) y el agua
seguiría viéndose casi igual que ahora.

El plan debe entonces **fabricar el lighting del agua**, no leerlo del BSP.

## Opciones de fuente de iluminación

Tres alternativas en orden de coste/calidad:

### A) Heredar el lightmap de la cara opuesta (fondo / techo del volumen)

Para cada cara de agua, trazar un ray corto desde su centro perpendicular al
plano hasta chocar con una superficie sólida (el fondo de la piscina si el
agua mira hacia arriba; el techo si mira hacia abajo). Copiar el bloque de
lightmap del hit al atlas, en el slot reservado para la cara de agua. Es un
bake one-shot en `GL_BuildLightmaps`, no toca el hot path de render.

- **Pros**: aprovecha infraestructura existente (atlas + shader multiply);
  una vez bakeado, el coste de render es igual al de una pared.
- **Contras**: el fondo puede no existir (agua mirando al cielo), y si la
  cara de agua es mayor que la sólida bajo ella, hay que extrapolar bordes.
  Casos raros se resuelven con fallback a luz plana.

### B) Sample por frame en el shader desde una textura 3D de luz volumétrica

Más moderno pero requiere construir esa textura (o aproximarla desde
entities de luz del BSP: `light`, `light_torch`, etc.). Fuera del scope
típico de un engine Quake; lo menciono sólo para completitud.

### C) Iluminación por vertex-lit sin atlas

Al construir la geometría del agua, para cada vértice hacer `R_LightPoint`
(ya existe en el engine — es lo que ilumina a los monstruos). Meter el
color en el vertex como atributo. El FS multiplica diffuse por el color
interpolado. No hay atlas, no hay page binding, no hay `SURF_DRAWTURB`
skip que quitar.

- **Pros**: mucho más simple que (A). Respeta ambient sin necesidad de
  bake. Los dlights actualizan `R_LightPoint` automáticamente por frame.
- **Contras**: resolución de iluminación = densidad de vértices del agua
  (puede ser baja si la subdivisión no es fina). Sin sombras nítidas;
  iluminación suave tipo Gouraud.

## Recomendación

**Empezar por (A) como primer experimento.** El usuario ha pedido
explícitamente probar primero la herencia del lightmap del fondo por
raycast, aun siendo consciente de que **es muy probable que salga mal** y
haya que revertirlo.

Motivos por los que puede salir mal (para anticipar):

- Superficies de agua sin fondo sólido (agua que mira al cielo abierto, o
  volúmenes de agua grandes donde el raycast no encuentra nada razonable).
- Discordancia de tamaño: la cara de agua puede ser mucho mayor que la
  pared/suelo bajo ella, obligando a extrapolar bordes del atlas — con
  artefactos de sangrado (luz de otras caras filtrándose).
- Discordancia de orientación: plano de agua y plano del fondo no
  coplanares → el lightmap del fondo "proyectado" hacia arriba queda
  estirado o rotado respecto al UV del agua.
- Superficies de agua que cubren varias piezas geométricas abajo: no hay
  una única cara-fuente que sirva para todo el bloque de lightmap.
- Costo de bake en `GL_BuildLightmaps`: un raycast por cara de agua puede
  ser aceptable, pero un raycast por **lumel** (16×16 por bloque típico)
  es más caro y probablemente necesario si la cara de agua no casa 1:1
  con la de abajo.

**Plan de fallback** si (A) no funciona o queda feo: revertir los
cambios y probar (C) vertex-lit. El md mantiene ambos planes documentados
para no perder el trabajo de investigación.

## Plan revisado para la opción C (vertex lit)

1. **Extender `warp_vtx_t`** con `unsigned char r, g, b, _pad` (o
   `float rgb[3]` — byte es más eficiente en bandwidth).
2. **En `Water_AppendSurface`**, por cada vértice del polígono:
   `R_LightPoint(v->xyz, out_rgb)` y copiar al vertex. Cuidado con
   performance — ese call hace un trace BSP; medirlo. Si es caro, cachear
   por surface (todos los vértices de la misma cara comparten luz
   aproximada).
3. **Attrib location=2 en el VAO del agua** apuntando a los bytes de
   color.
4. **VS agua**: leer `a_color` y pasarlo como `v_color`.
5. **FS agua**: `c.rgb *= v_color; frag_color = c;`.
6. **No tocar** `GL_BuildLightmaps`, `GL_CreateSurfaceLightmap`, ni el
   atlas. El agua queda fuera de ese sistema.
7. **Dynamic lights**: `R_LightPoint` ya considera dlights si se le pasa
   el tiempo correcto. Verificar que el eval se hace por frame (no one
   shot en build).

Los riesgos antiguos (`samples == NULL`, dlights) desaparecen con este
enfoque porque no dependemos del atlas.

## Tabla de archivos a tocar

| Tarea | Archivo | Líneas |
|-------|---------|--------|
| Permitir lightmap en agua (`GL_CreateSurfaceLightmap`) | `src_sdl3/gl_rsurf.cpp` | 1771 |
| Permitir lightmap en agua (`GL_BuildLightmaps`) | `src_sdl3/gl_rsurf.cpp` | 1816 |
| Extender `warp_vtx_t` | `src_sdl3/gl_warp.cpp` | 59-62 |
| Rellenar coords lightmap | `src_sdl3/gl_warp.cpp` | 259 (en `Water_AppendSurface`) |
| Attrib location=2 en VAO agua | `src_sdl3/gl_warp.cpp` | 84-85 |
| GLSL agua (VS + FS) | `src_sdl3/gl_render.cpp` | 526-574 |
| Uniform location `u_lightmap` | `src_sdl3/gl_render.cpp` | 581-584 |
| Declarar uniform en header | `src_sdl3/headers/gl_render.h` | ~200 |
| Bucketizar + bind TU1 en draw | `src_sdl3/gl_rsurf.cpp` | 731-828 (`R_DrawWaterSurfaces`) |
