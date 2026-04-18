# Profiling granular del renderizador

## Problema

En esta maquina los FPS estan capados a 60 (vsync del compositor de Windows o
del driver NVIDIA). Medir solo FPS **no vale**: el motor puede gastar 2 ms por
frame o 14 ms por frame y verse igual en pantalla — 60 FPS en ambos casos. No
podemos ver regresiones ni mejoras reales solo con el contador de FPS.

Ademas FPS es una metrica agregada: si un cambio acelera el mundo pero ralentiza
los modelos, el FPS medio puede no moverse y no verlo.

## Idea: ab-style profiler para frames

Tomar muestras **por seccion del pipeline** durante N frames, en instantes
**deterministas** dentro de una sesion de demo playback (siempre los mismos
network frames del demo), y volcar el resultado a un JSON que podamos diffear
entre builds.

Inspiracion: `ab` (Apache Benchmark) lanza muchas peticiones y te da un resumen
de latencia por percentiles. Aqui cada "peticion" es una fase del frame.

## Metrica

No hay una metrica primaria. Los bloqueos y las oportunidades de optimizacion
pueden estar en sitios inesperados — si solo miramos una dimension nos
perdemos el big picture. Por eso cada fase registra **a la vez**:

- **Tiempo GPU**: `glGenQueries` + `GL_TIME_ELAPSED` (standard en GL 3.3).
  Tiempo real que la GPU pasa ejecutando los draws de esa fase, en
  microsegundos, independiente de vsync y del CPU.
- **Tiempo CPU**: `Sys_FloatTime()` alrededor de la fase. Tiempo que pasa el
  hilo de render preparando y enviando draws (setup de matrices, walks por el
  BSP, binding de texturas, llamadas a GL).
- **Draw calls**: contador simple incrementado en cada `glDrawElements` /
  `glDrawArrays` dentro de la fase.
- **Triangulos emitidos**: `GL_PRIMITIVES_GENERATED` query (GL 3.3). Mide
  volumen real de geometria procesada, incluyendo lo que genera el pipeline
  (no solo lo que tu envias).
- **Fragmentos visibles**: `GL_SAMPLES_PASSED` occlusion query. Fragmentos
  que pasan el depth test — mide overdraw efectivo y carga de fill rate.

Los cuatro contadores cuentan la misma historia desde angulos distintos. Un
cambio puede bajar triangulos a la mitad sin mover el tiempo GPU (eras
CPU-bound o fill-bound, no vertex-bound); o bajar el tiempo CPU sin tocar el
GPU (mismos draws, menos overhead por draw). Ver las cuatro columnas juntas
es lo que permite diagnosticar *donde* esta el cuello — mirar solo una es
adivinar.

Nota: las queries GL son asincronas. Hay que retirar resultados 2-3 frames
despues de encolarlas. Eso lo gestiona un ring buffer de queries (el mismo
ring sirve para los tres tipos de query: time elapsed, primitives generated,
samples passed).

## Secciones a medir

Cada entrada es un par `(nombre, tiempo_gpu, tiempo_cpu)`.

| Fase | Sitio |
|---|---|
| `clear` | R_Clear |
| `world_opaque` | R_DrawWorld -> R_RecursiveWorldNode -> R_DrawBrushMTex |
| `world_trans` | brush entities con R_DrawBrushMTexTrans |
| `sky` | R_DrawSkyBox + EmitSkyPolys |
| `water` | R_DrawWaterSurfaces |
| `caustics` | acumulado de EmitCausticsPolys dentro del mundo |
| `alias_models` | R_DrawAliasModel para todas las entidades |
| `sprites` | R_DrawSpriteModel |
| `particles` | R_DrawParticles |
| `flares` | R_DrawGlows |
| `viewmodel` | R_DrawViewModel |
| `polyblend` | R_PolyBlend |
| `hud_2d` | todo lo que corre tras GL_Set2D (sbar + console + menu + crosshair + fps) |
| `swap` | SDL_GL_SwapWindow |
| `frame_total` | frame completo |

~15 fases, ajustable.

## Protocolo de muestreo

```
cmd:  profile_demo <nombre>  (arma el profiler y lanza el demo, en ese orden)
cvar: profile_out            (ruta al JSON de salida; se fija antes)
```

1. El usuario ejecuta `profile_demo profbench`. Ese **unico comando** hace
   dos cosas en orden: primero arma el profiler (resetea contadores, carga
   la lista de network frames a muestrear), despues llama internamente a la
   logica de `playdemo profbench`. Un solo comando elimina la ventana fragil
   en la que el demo ya esta corriendo pero el profiler aun no escucha — con
   dos comandos separados es facil perder los primeros frames muestreados.
2. El profiler usa el **indice de network frame del demo** (no el frame de
   render) como reloj. Asi las muestras son reproducibles entre builds y
   entre maquinas independientemente del framerate.
3. La lista de frames a muestrear es **fija y deterministica**: un array
   hardcoded (o derivado de una seed fija con un PRNG simple) de ~100 indices
   de network frame repartidos por el demo, e.g.
   `[42, 118, 203, 287, 361, ...]`. Siempre los mismos.
4. Cuando el demo alcanza uno de esos network frames, el profiler dispara las
   timer queries para ese frame de render. Al agotar la lista, vuelca JSON al
   path de `profile_out` y se desactiva.

Por que **determinista** y no aleatorio: queremos poder diffear builds. Si
cada sesion muestrea en instantes distintos, el ruido del contenido (que
tocaba renderizar en ese frame concreto) se mezcla con el ruido del cambio de
codigo. Muestreando siempre los mismos network frames del mismo demo, el
contenido es identico entre builds y cualquier delta en los percentiles viene
del renderer, no del azar.

El demo de referencia debe cubrir variedad de contenido (mundo abierto,
pasillos, muchos enemigos, agua, cielo) para que la muestra represente el
pipeline completo — el papel que cumplia antes la aleatoriedad temporal lo
cumple ahora la diversidad del demo.

## Formato JSON

```json
{
  "build": "carviary 1.48 (a3f21c)",
  "gl_version": "3.3.0 NVIDIA 595.97",
  "map": "e1m1",
  "resolution": [1920, 1080],
  "samples": 100,
  "sections": {
    "world_opaque": {
      "gpu_us":        { "mean": 1234, "p50": 1180, "p95": 1900, "p99": 2400, "min": 950, "max": 3100 },
      "cpu_us":        { "mean":  420, "p50":  400, "p95":  680, "p99":  900, "min": 310, "max": 1100 },
      "draw_calls":    { "mean":  312, "p50":  305, "p95":  380, "p99":  410 },
      "tris":          { "mean": 42000, "p50": 41500, "p95": 58000, "p99": 64000 },
      "frags_visible": { "mean": 1820000, "p50": 1780000, "p95": 2400000, "p99": 2700000 }
    },
    "sky":          { ... },
    "alias_models": { ... },
    ...
    "frame_total":  { "gpu_us": { "mean": 4800, ... }, "cpu_us": {...} }
  }
}
```

Tiempos en microsegundos; draw calls / tris / frags son contadores absolutos.
Una fase con p95 mucho mayor que p50 indica variabilidad alta (picos); una
fase con p50 alto es un coste constante. Comparar gpu_us contra cpu_us por
fase dice si el cuello esta en la GPU o en el driver/CPU; comparar tiempos
contra contadores de volumen dice si el coste es por-draw (overhead) o por
geometria/fill.

## Uso tipico

```
# baseline
profile_out profiles/baseline.json
profile_demo profbench    # arma profiler + lanza el demo, un solo comando
# el demo corre hasta el final (reproduccion automatica, no hay que jugar);
# al alcanzar el ultimo network frame muestreado el profiler vuelca el JSON.

# hago un cambio al renderer, recompilo

# repetir exactamente igual
profile_out profiles/change.json
profile_demo profbench

# comparar los dos JSON con un script chico (diff por percentiles)
```

El demo dura lo que dure (minuto y medio, dos minutos, lo que sea) pero es
tiempo pasivo — el motor reproduce solo, tu no tocas nada. Esa es la clave
de que sea reproducible: cero input humano entre baseline y change, y cero
ventana de arranque en la que se puedan perder frames.

Asi podemos decir "este cambio mejoro world_opaque p50 en 200 us" en vez de
"creo que va igual".

## Implementacion aproximada

**Ficheros nuevos**:
- `gl_profiler.h` / `gl_profiler.cpp` — ~300 lineas
  - `Prof_BeginSection(name)` / `Prof_EndSection(name)` que arrancan/paran
    **tres queries GL por fase** (`GL_TIME_ELAPSED`, `GL_PRIMITIVES_GENERATED`,
    `GL_SAMPLES_PASSED`) ademas de tomar `Sys_FloatTime()` para CPU.
  - `Prof_CountDraw()` — hook a llamar desde los wrappers de `glDrawElements`
    / `glDrawArrays` para contar draw calls de la fase activa.
  - ring de 4-8 sets de queries por seccion (para cubrir la latencia de
    readback de 2-3 frames).
  - acumulador de 100 muestras por seccion con las cuatro columnas
    (gpu_us, cpu_us, draw_calls, tris, frags_visible).
  - `Prof_Start(out_path)`, `Prof_Frame()` (decide si medir este frame segun
    la lista de network frames), `Prof_Finish()` (vuelca JSON).

**Modificaciones minimas** (~20 sitios, un par de lineas cada uno):
- `R_RenderScene`, `R_DrawWorld`, `R_DrawAliasModel`, `R_PolyBlend`, `SCR_UpdateScreen`, etc.
- Cada uno envuelto en `Prof_BeginSection("x")` / `Prof_EndSection("x")`.
- Los macros pueden ser no-op en un build sin profiling.
- Los draws del motor pasan por un par de funciones centralizadas; el
  contador de draw calls se pincha ahi una sola vez.

**Comandos/cvars**:
- `profile_demo <nombre>` — comando unico: arma el profiler y lanza
  `playdemo <nombre>` internamente, en ese orden. Evita la ventana fragil
  entre `playdemo` y el arranque del profiler.
- `profile_out <path>` — fija el path de salida (antes de `profile_demo`).
- `profile_running` (cvar read-only) — indica si esta en curso.

## Decisiones de diseno

- **No medir cada frame**. Mantener impacto bajo: 100 muestras deterministas
  entre miles de frames del demo dan estadistica solida sin perturbar el
  frame rate, y al ser los mismos frames siempre, el diff entre builds es
  limpio.
- **No incluir swapbuffer en total** si vsync = on, porque se come todo el
  headroom. Medir `frame_total` **sin** el swap para ver trabajo real.
- **Medir tiempo y volumen a la vez**. GPU time, CPU time, draw calls,
  triangulos y fragmentos visibles — las cinco columnas juntas. El cuello
  puede estar en cualquiera y optimizar a ciegas (mirando solo tiempo, o solo
  contadores) lleva a "mejoras" que no mueven la aguja.
- **Un JSON por sesion, no CSV ni logging continuo**. Lo hace facil de diffear,
  versionar, compartir.

## Extensiones posibles (no ahora)

- Snapshot de cvars al iniciar (para saber que config genero el JSON).
- Comparador automatico: `profile_diff baseline.json change.json` que imprime
  las deltas mas grandes.
- Texture memory / VBO memory usada por fase (estimacion).
- Estado del pipeline: numero de cambios de shader, numero de bindings de
  textura por fase (a veces el cuello es cambiar estado, no dibujar).

## Alternativas consideradas

- **RenderDoc / NVIDIA Nsight**: mas potente pero externo, no scripteable
  dentro del motor, no reproducible en otras maquinas sin el software.
- **Solo CPU timers**: no ven el coste real de la GPU.
- **Medir cada frame y loguear todo**: volumen enorme, perturbacion alta,
  dificil de diffear.
- **Capturar 1000 frames y comparar medias**: sin percentiles pierdes los
  picos, que son donde estan los stalls interesantes.
