# Orden de renderizado de Carviary (por frame)

Referencia: `gl_screen.cpp:SCR_UpdateScreen` -> `gl_rmain.cpp:R_RenderView/R_RenderScene`

## Estado GL base (inicializado en GL_Init)

```
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
glTexEnvf(GL_TEXTURE_ENV_MODE, GL_REPLACE)
glEnable(GL_CULL_FACE)     // front face culling
glEnable(GL_BLEND)
glDisable(GL_ALPHA_TEST)
glEnable(GL_DEPTH_TEST)
glDepthFunc(GL_LEQUAL)
```

---

## Fase 1: 3D Scene (`V_RenderView` -> `R_RenderView`)

### 1.1 R_Clear
- `glClear(GL_DEPTH_BUFFER_BIT)` (+ COLOR_BUFFER si gl_clear=1)
- Si hay mirrors: clear especial via Mirror_Clear

### 1.2 R_RenderScene

| Paso | Funcion | Que dibuja | Modo |
|------|---------|-----------|------|
| 1 | `R_SetupFrame` | Calcular PVS, viewleaf, visedicts | - |
| 2 | `R_PushDlights` | Marcar superficies afectadas por luces dinamicas | - |
| 3 | `R_SetFrustum` | Calcular planos del frustum | - |
| 4 | `R_SetupGL` | Viewport, perspectiva, depth range, glCullFace | Depth ON, Blend ON |
| 5 | `R_MarkLeaves` | Marcar hojas BSP visibles desde la camara | - |
| 6 | `R_DrawWorld` | BSP del mundo: caras opacas, lightmaps, fullbrights + static entities | `GL_REPLACE` para texturas, `GL_DST_COLOR * GL_ONE_MINUS_SRC_ALPHA` para lightmaps, multitexture si disponible |
| 7 | `S_ExtraUpdate` | Actualizar sonido (evitar stuttering en mapas lentos) | - |
| 8 | `R_SetupTransEntities` | Clasificar entidades transparentes vs opacas | - |
| 9a | Si NO bajo agua: | | |
| | `R_SortTransEntities(true)` | Entidades transparentes (lejanas primero) | `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA` |
| | `R_DrawParticles(true)` | Particulas detras del agua | `GL_SRC_ALPHA, GL_ONE` (additive) |
| | `R_DrawWaterSurfaces` | Superficies de agua/slime/lava | `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA` con `r_wateralpha` |
| | `R_SortTransEntities(false)` | Entidades opacas | `GL_REPLACE` |
| | `R_DrawParticles(false)` | Particulas delante del agua | `GL_SRC_ALPHA, GL_ONE` |
| 9b | Si bajo agua: | Orden invertido (agua primero, luego trans) | mismos modos |
| 10 | Fog setup | `glFogi(GL_FOG_MODE, GL_LINEAR)` si gl_fogenable | Fog ON |

### 1.3 R_Mirror (si hay superficies mirror)
- Reflip de la escena, re-render con stencil/clip
- `r_mirroralpha` controla opacidad

### 1.4 R_DrawViewModel
- El arma del jugador (v_model)
- `glDepthRange(0, 0.3)` para que no clipee con el mundo
- Blend alpha para transparencia del arma

### 1.5 R_PolyBlend
- Fullscreen color overlay (daño rojo, items, bajo agua)
- `glDisable(GL_DEPTH_TEST)`, `glDisable(GL_TEXTURE_2D)`
- `glColor4fv(v_blend)` con alpha blend
- Brightness/gamma: `DoGamma()` + `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`

---

## Fase 2: Post-proceso

### 2.1 DOF_Apply (Depth of Field)
- Lee framebuffer + depth con `glReadPixels`
- Blur en CPU a 1/4 resolucion
- Composite basado en profundidad
- Escribe resultado con `glDrawPixels`
- Restaura GL_BLEND, GL_DEPTH_TEST

---

## Fase 3: 2D Overlay (`GL_Set2D`)

Cambio a proyeccion ortografica:
```
glOrtho(0, vid.width, vid.height, 0, -99999, 99999)
glDisable(GL_DEPTH_TEST)
glColor4f(1,1,1,1)
```

Todo lo 2D usa `GL_BLEND` con `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA` y `GL_REPLACE` texture env.

### Ramas condicionales (solo una se ejecuta por frame):

**Si cargando mapa** (`con_forcedup && sv.active`):
| Orden | Funcion | Que dibuja |
|-------|---------|-----------|
| 1 | `Draw_MapShots` | Screenshot del mapa anterior como fondo |
| 2 | `SCR_DrawLoading` | Texto "Loading" (desactivado) |

**Si dialog** (`scr_drawdialog`):
| Orden | Funcion | Que dibuja |
|-------|---------|-----------|
| 1 | `Sbar_Draw` | Status bar |
| 2 | `Draw_FadeScreen` | Overlay negro 75% alpha |
| 3 | `SCR_DrawNotifyString` | Texto de dialogo |

**Si intermission** (`cl.intermission == 1`):
| Orden | Funcion | Que dibuja |
|-------|---------|-----------|
| 1 | `Sbar_IntermissionOverlay` | Pantalla de fin de nivel (stats) |

**Si finale** (`cl.intermission == 2`):
| Orden | Funcion | Que dibuja |
|-------|---------|-----------|
| 1 | `Sbar_FinaleOverlay` | Imagen de finale |
| 2 | `SCR_CheckDrawCenterString` | Texto central con efecto maquina de escribir |

**Gameplay normal** (la mayoria del tiempo):
| Orden | Funcion | Que dibuja |
|-------|---------|-----------|
| 1 | `Draw_Crosshair` | Crosshair (si crosshair > 0) - `GL_MODULATE` con alpha |
| 2 | `SCR_DrawNet` | Icono de red (si lag) |
| 3 | `SCR_DrawTurtle` | Icono de tortuga (si fps bajo) |
| 4 | `SCR_DrawPause` | Texto "PAUSED" |
| 5 | `SCR_CheckDrawCenterString` | Texto central (mensajes del servidor) |
| 6 | `Sbar_Draw` | Status bar: salud, armadura, municion, arma, cara | `sbar_alpha` controla transparencia |
| 7 | `SHOWLMP_drawall` | LMPs dinamicos (Tomaz extension) |
| 8 | `SCR_DrawConsole` | Consola (si abierta) - con `con_alpha` transparency. Fondo: `conback` texture |
| 9 | `M_Draw` | Menu (si abierto) - sobre todo lo demas |

### Siempre (despues de las ramas):
| Orden | Funcion | Que dibuja |
|-------|---------|-----------|
| 10 | `SCR_DrawFPS` | Contador FPS (si show_fps=1) |
| 11 | `V_UpdatePalette` | Actualizar palette shifts (bonus flash, daño) |

---

## Fase 4: Present

### GL_EndRendering
- `SDL_GL_SwapWindow(sdl_window)` - presenta el back buffer

---

## Resumen del pipeline por frame

```
glClear(DEPTH)
  |
  v
[3D] Mundo BSP (opaco, lightmapped)
  |
  v
[3D] Entidades (alias models, sprites) - opacas y transparentes
  |
  v
[3D] Agua (alpha blended)
  |
  v
[3D] Particulas (additive blend)
  |
  v
[3D] Fog overlay
  |
  v
[3D] Mirror re-render (si hay)
  |
  v
[3D] Arma del jugador (depth range reducido)
  |
  v
[3D] PolyBlend (daño rojo, underwater, gamma)
  |
  v
[POST] DOF blur (glReadPixels -> CPU -> glDrawPixels)
  |
  v
[2D] GL_Set2D (ortho, no depth test)
  |
  v
[2D] Crosshair, net icon, turtle, pause
  |
  v
[2D] Status bar (HUD)
  |
  v
[2D] Consola / Menu
  |
  v
[2D] FPS counter
  |
  v
SwapBuffers
```
