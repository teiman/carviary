# Experimento: grass shader aplicable por textura

## Idea

Nuevo comando de consola `grow_grass`. Dispara un traceline desde la vista
del jugador. La primera superficie de mundo que toca: lee el nombre de la
textura de esa superficie y marca **esa textura** para que se dibuje con un
shader "grass" (hierba emergiendo de la superficie) en lugar del shader
normal de mundo opaco.

Punto interesante: la marca es **por textura**, no por superficie concreta.
Disparar contra un trozo de suelo marca como "grass" todas las superficies
del mapa que comparten esa textura. Esto permite convertir suelos enteros a
hierba con un solo comando, y es trivial de encadenar (`grow_grass` en varios
materiales).

## Uso previsto

1. Estás dentro de un mapa.
2. Miras un tipo de suelo (dirt, grass, etc.) y escribes `grow_grass` en
   consola.
3. Todo ese material en el mapa pasa a renderizarse con el shader grass.
4. `grow_grass_clear` vuelve al render normal.

## Flujo tecnico

### Resolver la textura bajo la mira

- Origen del trace: `r_refdef.vieworg`.
- Direccion: `vpn` (vector de vista).
- Distancia: 8192 unidades (suficiente para ver el otro extremo de cualquier
  room).
- Funcion existente reutilizable: `CL_TraceLine` / `SV_Move` segun que este
  disponible en cliente. Si lo mas limpio es `TraceLine` de `cl_` (lo usa el
  codigo de impact sparks), reusar eso.
- Recuperar la `msurface_t` impactada. La trace devuelve un punto de
  impacto + plano; mapear a surface mirando el nodo BSP bajo ese punto
  (`Mod_PointInLeaf` + walk hasta encontrar el surface cuyo plano coincide).
  Alternativa mas simple: iterar las surfaces del leaf/nodo del hit buscando
  la que contiene el impacto -- hay helpers en el codigo para puntos en
  surface (`R_LightPoint` hace algo similar).
- De ahi: `texture_t *t = surf->texinfo->texture;` -> nombre en `t->name`.

### Marcar la textura

- Anadir un campo `byte grass;` a `texture_t` (en `gl_model.h`). 0 por
  defecto, 1 si esta marcada.
- `grow_grass` setea a 1. `grow_grass_clear` recorre `cl.worldmodel->textures`
  y los pone a 0.
- Persistencia: por ahora ninguna. La marca vive mientras el mapa este
  cargado. Si se quiere guardar, un cvar string con lista de nombres
  separados por coma ("texA,texB,texC") seria el fix minimo.

### Render path

`R_EmitTextureChains` ya itera por textura. Justo antes de elegir shader
entre `R_WorldOpaqueShader` / `R_WorldFenceShader`, mirar `t->grass`. Si
esta activa, usar `R_WorldGrassShader` (nuevo).

Coste: una lectura de byte por textura visible (no por superficie, no por
vertex). El loop sobre `numtextures` del worldmodel ya lo pagamos para el
batching normal, asi que anadir un tercer caso de shader no cambia nada.

Nota importante para la iteracion 2 del shader (blades de hierba con
geometry shader o instancing): **ese NO se mete aqui**. La capa base del
suelo sigue saliendo en `R_EmitTextureChains` como un draw mas de world.
Los blades son geometria adicional que no vive en `world_static_vbo`, asi
que van en un **segundo pass aparte** despues de `R_EmitTextureChains`: un
loop que recorra las texturas con `t->grass` marcada y emita los blades
con su propio VBO / drawcall por grupo. Eso mantiene limpia la ruta del
suelo y separa "tintar la textura" de "plantar geometria".

### Shader grass (primera iteracion minima)

Mantenerlo **muy simple** para validar el pipeline antes de complicar:

- Vertex shader: pasa posicion, tc, lm_tc a fragment.
- Fragment shader: `texture(u_tex, v_tc) * texture(u_lm, v_lm_tc)` **tintada
  con verde** (`rgb.g += 0.3`, clamp). Sin geometria nueva.
- Con esto verificamos que la seleccion por textura funciona correctamente.

### Shader grass (segunda iteracion, lo visual)

Una vez que la seleccion por textura esta probada, el shader grass "de
verdad" puede hacer varias cosas. Opciones de menor a mayor coste:

1. **Tint + textura overlay**: multiplicar por una textura de hierba
   (`textures/grass_overlay.png`) en world space. Barato. No se ve 3D pero
   da la sensacion de estar tapizado.
2. **Grass billboards via geometry shader**: cada triangulo del surface
   emite N quads verticales pequenos con textura de alpha blade. Requiere
   geometry shader y una textura de sprite de hoja. Coste medio, buen
   resultado visual.
3. **Instanced grass blades**: generar un grid de hojas sobre el surface
   con `glDrawArraysInstanced`. Cada hoja es un quad billboard. Este es el
   que usa la mayoria de engines modernos. Requiere precalcular puntos por
   surface al cargar el mapa y guardarlos por textura.

Empezar por (1) o (2). Dejar (3) para cuando el resto este solido.

### Iluminacion y viento

Con la opcion (2) o (3) conviene anadir:

- `u_time` uniform para balanceo de hierba (sin en el vertex shader).
- Lightmap aplicado a las blades igual que al suelo, para que no se vean
  "encima" de la luz.

## Casos borde

- **Sky / water**: si el trace impacta una surface con `SURF_DRAWSKY` o
  `SURF_DRAWTURB`, no se marca. El shader grass es solo para mundo opaco.
  Mensaje en consola: "grow_grass: hit sky/water, ignored".
- **Sin impacto**: si el trace no toca nada en 8192 unidades, mensaje
  "grow_grass: no surface in sight".
- **Ya marcada**: `grow_grass` sobre una textura ya marcada no hace nada
  (idempotente). O toggle, a decidir.
- **Textura animada**: `texture_t->anim_next` encadena animaciones. Marcar
  la base animada marca tambien las siguientes del ciclo, o no? Decision
  pragmatica: marcar solo la que tocaste. Si da problemas visuales cuando
  la textura cicla, revisar.

## Comandos

- `grow_grass` -- marca la textura bajo la mira.
- `grow_grass_clear` -- desmarca todas.
- `grow_grass_list` -- imprime nombres de texturas marcadas actualmente.

## Trabajo aproximado

- Campo `grass` en `texture_t` + loader que lo pone a 0: 5 min.
- Comando `grow_grass` + traceline + resolucion de surface -> texture: 1-2h
  (el traceline es la parte fragil).
- Seleccion de shader en `R_EmitTextureChains`: 20 min.
- Shader grass iteracion 1 (tint verde): 30 min.
- Shader grass iteracion 2 (geometry shader con blades): medio dia.

## No hacer en este experimento

- No persistir entre mapas.
- No colisiones: la hierba es decorativa, no tiene fisica.
- No iluminacion dinamica propia de la hierba.
- No hacer que la hierba reaccione al jugador (aplastada). Eso es un
  experimento separado si mola.
