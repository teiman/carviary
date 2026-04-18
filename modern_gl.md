# Port a OpenGL 3.3 Core

Estado actual: OpenGL 1.x fixed-function con SDL3 como capa de plataforma.
Objetivo: subir a GL 3.3 core profile, en fases, sin romper el build.

Estrategia: durante la migracion el contexto es **GL 3.3 Compatibility** (GLAD compat). Cada fase convierte un grupo de ficheros a estilo moderno (VBO + shader). Cuando ya no quedan llamadas 1.x, se regenera GLAD como **Core** y se cambia el profile — y el compilador grita si se ha colado algo.

## Inventario de deuda GL 1.x

| Categoria | Calls | Ficheros principales |
|---|---|---|
| glBegin/glEnd | 49 | gl_rsurf (10), gl_draw (8), gl_sky (8) |
| Matrix stack (Push/Pop/Rotate/Translate/Scale) | 68 | gl_rmain (34), gl_dof (12) |
| glTexEnvf MODULATE/REPLACE | 49 | gl_rsurf (16), gl_font (6) |
| glVertex/TexCoord/Color (immediate mode) | 226 | gl_draw (63), gl_rsurf (22), gl_font (21) |
| glEnable/Disable(GL_TEXTURE_2D) | 27 | gl_rsurf (8), gl_rmain (4) |
| glBindTexture | 42 | gl_rsurf (16), gl_sky (10) |
| glBlendFunc cambios | 19 | gl_rsurf (5), gl_warp (4) |
| Depth/Cull state | 26 | gl_rmain (8), gl_mirror (5) |
| Fog (glFog*) | 4 | gl_rmain (4) |

**Primitivas eliminadas en GL 3.3 core:** GL_QUADS (25 usos), GL_POLYGON (16 usos). Hay que triangular.

---

## Equivalencias GL 1.x -> 3.3 core

| GL 1.x | GL 3.3 core |
|---|---|
| glBegin/glEnd + glVertex | VBO + VAO + glDrawArrays/Elements |
| glPushMatrix/glPopMatrix | Matrices en CPU + uniforms |
| glTexEnvf MODULATE/REPLACE | Shader: `color * texture` o `texture` |
| glColor3f/4f entre vertices | Vertex attribute o uniform |
| GL_QUADS, GL_POLYGON | GL_TRIANGLES con index buffer |
| glAlphaFunc | `if (alpha < threshold) discard;` |
| glFog* | Uniform + calculo en fragment shader |
| glEnable(GL_TEXTURE_2D) | No existe - shader siempre samplea |
| MYgluPerspective/glOrtho | Funciones de matriz CPU |

---

## Shaders (9 programs)

| Shader | Fases que lo introducen/usan | Vertex attrs | Fragment |
|---|---|---|---|
| particle | F1 | pos + color | flat color |
| hud_2d | F2, F3 | pos2d + tc + color | texture * color |
| sprite | F2 | pos + tc | texture * color |
| fullscreen | F3 | pos2d | color / post-process |
| sky | F4 | pos | cubemap sample |
| alias_model | F5 | pos + tc + normal + lerp | texture * lighting |
| world_opaque | F6 | pos + tc + lm_tc | texture * lightmap |
| world_fence | F6 | pos + tc + lm_tc | texture * lightmap + alpha discard |
| world_water | F7 | pos + tc + warp_tc | texture * alpha, warp |

---

# Fases

Cada fase es entregable: al acabar, el juego compila, arranca y se ve bien. Entre fases se puede parar sin deuda abierta.

---

## Fase 0 — Infraestructura ✅

Estado: **completada**.

- [x] GLAD 3.3 Compat en [src_sdl3/third_party/glad/](src_sdl3/third_party/glad/)
- [x] Contexto GL 3.3 Compat + `gladLoadGLLoader` en [gl_vid_sdl.cpp:330-361](src_sdl3/gl_vid_sdl.cpp#L330-L361)
- [x] CMake: `project(... C CXX)` + glad.c compilado
- [x] Helpers: [gl_core.h](src_sdl3/headers/gl_core.h) (`GLShader`, `DynamicVBO`) y [gl_mat4.h](src_sdl3/headers/gl_mat4.h)
- [x] Portabilidad: eliminados `extern PROC` Win32, `BOOL` -> `qboolean`

**Criterio de fin:** el binario arranca y `GL_VERSION` >= 3.3.

---

## Fase 1 — Particulas (shader `particle`) ✅

Estado: **completada y validada visualmente**.

- [x] [gl_part.cpp](src_sdl3/gl_part.cpp): eliminado el andamiaje manual de carga de funciones GL (~60 lineas de typedefs `PFNGL*`)
- [x] Shader migrado de GLSL 110 (`attribute`/`varying`/`gl_FragColor`) a GLSL 330 core (`in`/`out`/`layout(location)`)
- [x] `R_DrawParticles` sustituye `glBegin(GL_TRIANGLES)` por triangle soup en CPU → `DynamicVBO_Upload` → `glDrawArrays`
- [x] MVP leido transitoriamente de las matrices fixed-function (se arregla en F8)

**Criterio de fin cumplido:** particulas visibles identicas a antes.

---

## Fase 2 — Billboards y UI trivial (shaders `sprite` + `hud_2d`) ✅

Estado: **completada y validada visualmente**.

- [x] [gl_render.cpp](src_sdl3/gl_render.cpp) nuevo: registro central de shaders compartidos (`particle`, `hud_2d`, `sprite`), lazy init + helper `R_CurrentMVP`
- [x] [gl_part.cpp](src_sdl3/gl_part.cpp) refactor para usar el `particle` compartido
- [x] [gl_crosshair.cpp](src_sdl3/gl_crosshair.cpp): quad 2D pixel-space con ortho manual, shader `hud_2d`
- [x] [gl_sprite.cpp](src_sdl3/gl_sprite.cpp): billboard 3D, `e->scale` pre-multiplicado en CPU (fuera `glPushMatrix/glScalef`), shader `sprite`
- [x] [gl_flares.cpp](src_sdl3/gl_flares.cpp): `GL_TRIANGLE_FAN` -> triangle soup, reutiliza shader `particle` (sin textura, color por vertice)

**Criterio de fin cumplido:** crosshair, sprites y flares se ven como antes.

---

## Fase 3 — HUD completo (shader `fullscreen`) ✅

Estado: **completada y validada visualmente**.

- [x] [gl_render](src_sdl3/headers/gl_render.h) ampliado: shader `fullscreen`, helpers `R_HudTexQuad`, `R_HudFill` y batch de caracteres (`R_HudBegin/Add/EndCharBatch`)
- [x] [gl_font.cpp](src_sdl3/gl_font.cpp): `Draw_Character`/`Draw_String`/`Draw_Alt_String` con batch, parser `&f...` intacto
- [x] [gl_draw.cpp](src_sdl3/gl_draw.cpp): `Draw_Pic/AlphaPic/Fill/FadeScreen/TileClear/MenuPlayer/MapShots` son one-liners al helper
- [x] [gl_screen.cpp](src_sdl3/gl_screen.cpp): `GL_BrightenScreen` usa `R_HudFill` con blend custom

**Criterio de fin cumplido:** menu, consola, HUD de juego, fades y FPS counter correctos.

---

## Fase 4 — Skybox ✅

Estado: **completada** (skybox validado estructuralmente; falta prueba visual con texturas reales, pendiente de que aparezcan `gfx/env/*`).

- [x] [gl_sky.cpp](src_sdl3/gl_sky.cpp) `R_DrawSkyBox`: 6 caras via tabla + bucle, reutiliza shader `sprite` (sin cubemap nuevo — suficiente)
- [x] `EmitSkyPolys` (sky animado clasico) **se queda en GL 1.x** — depende de `glpoly_t` y cae en F6/F7 con rsurf/warp

**Criterio de fin cumplido:** el juego arranca sin regresiones; el sky animado clasico (Quake vanilla) sigue funcionando en su ruta legacy.

---

## Fase 5 — Modelos animados MDL (shader `alias_model`) ✅

Estado: **completada y validada visualmente**. Aprovechada para simplificar.

- [x] `MD2` eliminado por completo (gl_md2.cpp, `Mod_LoadQ2AliasModel`, `md2_t`, `aliastype` en `model_t`, ALIASTYPE_MDL/MD2)
- [x] Sombras de alias eliminadas (`GL_DrawAliasBlendedShadow`, cvar `r_shadows`, entrada del menu, bloque shadow en `R_DrawAliasModel`)
- [x] [gl_mdl.cpp](src_sdl3/gl_mdl.cpp) `GL_DrawAliasBlendedFrame` con triangle soup + `DynamicVBO` + shader `alias_model`; lerp y lighting se siguen computando en CPU (CPU->VBO->shader)
- [x] Shader nuevo `alias_model` (pos + tc + color per-vertex + sampler) en [gl_render](src_sdl3/gl_render.cpp)

~600 lineas menos de codigo tras la simplificacion.

**Criterio de fin cumplido:** monstruos, viewmodel y pickups se ven correctos, con lighting y animacion.

**Detalle abierto:** los monstruos se perciben levemente "difuminados" respecto al resto del motor. Descartado sampling (probado con sampler object forzando `GL_NEAREST`) y lighting per-vertex (probado con color blanco plano). Causa por determinar; no bloquea.

---

## Fase 6a — BSP mundo opaco (shaders `world_opaque` + `world_fence` + `world_fullbright`) ✅

Estado: **completada y validada visualmente**.

Limpieza previa (pre-F6a):
- [x] Eliminados cvars `gl_wireframe`, `gl_wireonly`, `gl_showpolys`, variable `wireframe`, `R_DrawLinePolys` — wireframe no se usaba
- [x] Eliminados `R_DrawBrushNoMTex` / `R_DrawBrushNoMTexTrans` — multitexture es core en 3.3

Port F6a:
- [x] Shaders nuevos: `world_opaque` (diffuse × lightmap), `world_fence` (igual + alpha discard), `world_fullbright` (aditivo, premultiplica alpha para respetar paleta 255 transparente)
- [x] [gl_rsurf.cpp](src_sdl3/gl_rsurf.cpp) `R_DrawBrushMTex`: fan -> triangle soup, unit 0 = diffuse, unit 1 = lightmap, MVP via `R_CurrentMVP`, cleanup defensivo (unbind unit 1, unit 0 activa) al salir para no contaminar paths legacy
- [x] Caustics (`EmitCausticsPolys`) **desactivadas temporalmente** — vuelven en F7 con `gl_warp`
- [x] `R_DrawBrushMTexTrans` y `R_DrawBrushMTexScript` **siguen en GL 1.x** — F6b y F8 respectivamente

**Criterio de fin cumplido:** mundo, lightmaps, fences y fullbrights correctos.

**Nota:** los `glActiveTexture(GL_TEXTURE0)` defensivos en [gl_render.cpp](src_sdl3/gl_render.cpp) (HUD helpers) y [gl_rmain.cpp](src_sdl3/gl_rmain.cpp) (alias model) se quedan hasta que F6b/F8 porten las funciones legacy que dejan unit 1 activa.

---

## Fase 6b — BSP mundo transparente (`R_DrawBrushMTexTrans`) ✅

Estado: **completada y validada visualmente**.

- [x] `R_DrawBrushMTexTrans` reescrita usando shaders `world_opaque` / `world_fence` con `u_alpha` y blend activado
- [x] Save/restore de estado `GL_BLEND` para no corromper llamadas posteriores
- [x] Cleanup defensivo al salir (unit 1 unbind, unit 0 activa)

**Criterio de fin cumplido:** probado forzando `alpha=0.5` en todas las entidades brush de `e1m1`; puertas y plataformas se ven translúcidas, mundo detrás correcto.

---

## Fase 7 — Agua y efectos (shader `world_water`) ✅

Estado: **completada y validada visualmente** (warp casi identico al original).

- [x] Shader nuevo `world_water`: warp calculado per-fragment con formula adaptada de Ironwail, resuelve el "unos pixeles se mueven mas que otros" que daba la version per-vertex con `gl_subdivide_size=128`
- [x] [gl_warp.cpp](src_sdl3/gl_warp.cpp) `EmitWaterPolys(s, alpha)`: firma cambiada, triangle soup + shader, warp delegado a GPU
- [x] [gl_warp.cpp](src_sdl3/gl_warp.cpp) `EmitCausticsPolys(s)`: shader `sprite` + `glBlendFunc(ZERO, SRC_COLOR)` para el efecto multiplicativo
- [x] [gl_sky.cpp](src_sdl3/gl_sky.cpp) `EmitSkyPolys(s)`: sky clasico animado con dos pases (solido + alpha overlay), reusa shader `sprite`
- [x] `R_DrawBrushMTex` reactiva `EmitCausticsPolys` cuando `SURF_UNDERWATER` y `gl_caustics`
- [x] `R_DrawWaterSurfaces` simplificada (el alpha se pasa por parametro en lugar de via `glColor`)

**Formula del warp**:
```glsl
float r_s = (os + 8.0 * sin(ot * 0.03125 + u_time)) * 0.015625;
float r_t = (ot + 8.0 * sin(os * 0.03125 + u_time)) * 0.015625;
```

Misma estructura matematica que el legacy (amplitud 8, escala 1/64, fase cruzada os/ot), pero con frecuencia espacial 1/4 de la legacy (0.03125 vs 0.125) — asi cada ciclo de warp cubre ~4x mas area y el tiling se nota menos, acercandose al look de Ironwail.

**Criterio de fin cumplido:** agua, lava, slime, caustics y sky animado correctos.

**Nota:** `EmitEnvMapPolys` (env mapping "shiny") se queda en GL 1.x — solo lo usa `R_DrawBrushMTexScript`, se resolvera en F8.

---

## Fase 8 — Scene setup y casos raros

**Objetivo:** cerrar los ultimos sitios con GL 1.x.

- Ficheros: [gl_rmain.cpp](src_sdl3/gl_rmain.cpp) (matrices + fog, 2 glBegin), [gl_mirror.cpp](src_sdl3/gl_mirror.cpp) (re-render, 1 glBegin), [gl_rscript.cpp](src_sdl3/gl_rscript.cpp) (1 glBegin)
- Sustituye: `glPushMatrix/Pop` → stack manual con `mat4_t`, `glFog*` → uniform, espejos → FBO

**Criterio de fin:** `gl_grep -n "glBegin\|glPush\|glFog" src_sdl3/*.cpp` no devuelve nada.

---

## Fase 9 — Switch a Core profile (partida en sub-fases)

### Fase 9a — Matrix stack CPU ✅

Estado: **completada y validada visualmente**.

- [x] [gl_mat4.h](src_sdl3/headers/gl_mat4.h) / [gl_mat4.cpp](src_sdl3/gl_mat4.cpp): `MatStack` con `Push/Pop/Load/LoadIdentity/MulTranslate/MulScale/MulRotate`, helpers `mat4_rotate_axis` y `mat4_frustum`
- [x] Stacks globales `r_modelview`, `r_projection` inicializados en `GL_Init`
- [x] `R_CurrentMVP` ahora lee del stack CPU, no de `glGetFloatv`
- [x] Todas las llamadas `glPushMatrix/glPopMatrix/glRotatef/glTranslatef/glScalef/glFrustum/glOrtho/glMatrixMode/glLoadMatrixf/glLoadIdentity` del render principal sustituidas: `GL_Set2D`, `R_SetupGL`, `MYgluPerspective`, `R_BlendedRotateForEntity`, `R_RotateForEntity`, `R_DrawAliasModel`, `R_SetupBrushPolys`, `R_DrawWaterSurfaces`
- [x] DOF (post-process opcional, cvar `r_dof=0` por defecto) sigue en GL 1.x — se tratara si se activa

**Criterio de fin cumplido:** todo se ve igual que antes del port de matrices.

### Fase 9b — Fog eliminado ✅

Estado: **completada**.

- [x] Eliminados cvars `gl_fogenable`, `gl_fogstart`, `gl_fogend`, `gl_fogred`, `gl_foggreen`, `gl_fogblue`
- [x] Eliminadas todas las llamadas `glFog*` y `glEnable/glDisable(GL_FOG)` del motor (en `R_RenderScene` y `gl_flares`)
- [x] Eliminadas 6 entradas consecutivas del menu Video (fog toggle + start + end + 3 sliders de color), con renumeracion
- [x] Crosshair queda como entrada final (case 9, y=104)

La feature fog era `off` por defecto y daba un aspecto feo (per-vertex). Si se reintroduce en el futuro, sera en shaders.

### Fase 9c — Switch final a Core profile ✅

Estado: **completada y validada visualmente**. El motor corre en **OpenGL 3.3 Core**.

Cambios:
- [x] Limpiado `GL_Init` de estado 1.x: fuera `glEnable(GL_TEXTURE_2D)`, `glAlphaFunc`, `glShadeModel`, `glPolygonMode`, `glTexEnvf`, `glTexGeni`
- [x] Eliminado DOF ([gl_dof.cpp](src_sdl3/gl_dof.cpp) + cvars + llamadas): el path usaba `glRasterPos`/`glDrawPixels`, imposible en Core
- [x] Sustituido `glColor3f`/`glTexEnvf` del texto legacy por `Draw_SetCharColor` (console/menu/checkbox)
- [x] Borrados `glColor4f(1,1,1,1)` residuales en crosshair/flares/mdl/part/sprite
- [x] GLAD regenerado como **Core**
- [x] `SDL_GL_CONTEXT_PROFILE_CORE` activado
- [x] **Internal format** de texturas arreglado: `gl_solid_format=3` → `GL_RGB8`, `gl_alpha_format=4` → `GL_RGBA8`, lightmap con `GL_RGBA8` explicito (Core rechaza los numericos legacy)
- [x] **IDs de textura** migrados a `glGenTextures`: `playertextures[16]`, `lightmap_textures[MAX_LIGHTMAPS]`, `translate_texture`, `solidskytexture`, `alphaskytexture`, todas las texturas de `GL_LoadTexture` (Core no acepta IDs inventados por `texture_extension_number++`)
- [x] Eliminado todo el sistema legacy de multitextura (`gl_mtexable`, `qglMTexCoord2fSGIS_ARB`, `qglSelectTextureSGIS_ARB`, `TEXTURE0/1_SGIS_ARB`)
- [x] Eliminado `texture_extension_number` global

**Criterio de fin cumplido:** arranca con `GL_VERSION: 3.3.0 Core`, mundo/modelos/HUD/agua/cielo/partículas/flares/fence/fullbrights todos visuales correctos.

---

## Estimacion

| Fase | Dias | Notas |
|---|---|---|
| 0 | 1 (hecho) | Infra |
| 1 | 0.5 | Particulas |
| 2 | 1 | Crosshair + sprite + flares |
| 3 | 1.5 | HUD completo |
| 4 | 1 | Skybox |
| 5 | 2-3 | MDL + MD2 |
| 6 | 3-5 | BSP + lightmaps (el gordo) |
| 7 | 1 | Agua |
| 8 | 2 | Scene setup + mirror + rscript |
| 9 | 0.5 | Switch a Core |

**Total: 2-3 semanas.**

---

## Ventajas al terminar

- Rendering mas rapido (batching real, menos state changes)
- Base para efectos modernos (shadow maps, SSAO, bloom, etc.)
- Compatible con GPUs modernas sin capa de compatibilidad

## Riesgos

- El engine mezcla logica de juego con GL calls en muchos sitios
- Alias models usan GL command lists con fans/strips interleaved
- BSP surfaces tienen chain rendering complejo con lightmap atlas
- Regresiones visuales dificiles de detectar (blending, z-fighting)
