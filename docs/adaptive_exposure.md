# Exposición adaptativa vía lightstyle 0

Experimento: simular la adaptación del ojo humano a la luz ambiental
**modulando el lightstyle 0** en runtime.

- Jugador en zona oscura → subir `d_lightstylevalue[0]` → todas las
  superficies con style 'a' (la mayoría del mapa) se iluminan más.
- Jugador en zona luminosa → bajar `d_lightstylevalue[0]` → todo se
  oscurece.

Los cambios se suavizan con un filtro temporal para que no sean bruscos —
como tarda el ojo en acostumbrarse al entrar a un sótano desde la calle.

## Por qué lightstyle 0 y no postFX

- `d_lightstylevalue[]` es un array global de escalas por lightstyle
  que `R_BuildLightMap` usa para modular los `samples` del BSP al
  construir los lumels que suben al atlas.
- Cambiar `d_lightstylevalue[0]` marca automáticamente como
  `lightmap_modified` todos los lumels que usan style 0 (via la lógica
  existente de styles parpadeantes como antorchas).
- **No pasa por el framebuffer**: afecta directamente a la iluminación
  del mundo, así que los monstruos y entidades alias también responden
  (R_LightPoint samplea los mismos lumels).
- No hay banding de 8-bit, no hay feedback loops con el bloom, no hay
  conflictos con `V_PolyBlend`.

## Valor base de lightstyle 0

`d_lightstylevalue[i]` es un entero. El código base usa `264` como
"luz normal" (ver `R_NewMap` en `gl_rmisc.cpp`):

```cpp
for (i = 0; i < 256; i++)
    d_lightstylevalue[i] = 264;
```

Y `R_AnimateLight` lo actualiza cada frame según el string del style.
Style 'a' = all-on = 264. Style 'z' = all-off = 0. Valores intermedios
para parpadeos de antorcha.

**Nuestra modulación**: en lugar de dejar style 0 fijo en 264,
mantenerlo en `264 * exposure_factor` donde `exposure_factor` varía
entre ~0.5 (zona muy luminosa → oscurecer) y ~2.0 (zona muy oscura →
aclarar).

## Cómo medir la luz local

`R_LightPoint(r_refdef.vieworg)` devuelve la iluminación en un punto
vía raycast al suelo + lookup del lightmap. Escribe en la global
`lightcolor[3]` (RGB `[0..255]`).

```cpp
R_LightPoint(r_refdef.vieworg);
float brightness = (lightcolor[0] + lightcolor[1] + lightcolor[2]) / (3.0f * 255.0f);
```

## Cálculo del factor

```cpp
const float REF = 0.45f;              // "luz normal"
float raw_target = REF / fmaxf(brightness, 0.05f);
float target_factor = clamp(raw_target, 0.5f, 2.0f);
```

## Suavizado temporal

```cpp
static float current_factor = 1.0f;
float k = 1.0f - expf(-frametime / TIME_CONSTANT);
current_factor += (target_factor - current_factor) * k;
```

`TIME_CONSTANT ~ 1.0s` razonable. Más grande = más dramático.

## Aplicación

**Importante**: `d_lightstylevalue[0]` **NO lo establecemos, le añadimos un
offset** sobre el valor que el motor le deja cada frame. El motor (vía
`R_AnimateLight`) escribe el valor "natural" del style 'a' cada frame —
típicamente 264 pero puede variar si el mapa animara el style 0 con
secuencias raras. Nuestra contribución es un delta aditivo respecto a ese
valor base.

Flujo correcto:

```cpp
// Llamado DESPUÉS de R_AnimateLight, así tomamos el valor natural ya
// escrito este frame.
void Exposure_Update (float frametime)
{
    int natural = d_lightstylevalue[0];   // lo que el motor puso este frame

    // Medir, computar target_factor, interpolar current_factor...

    // Delta aditivo: la diferencia entre lo que queremos y lo natural.
    int desired = (int)(natural * current_factor);
    int offset  = desired - natural;

    // Clamp del offset para limitar cuánto nos desviamos del valor base.
    if (offset < -200) offset = -200;
    if (offset >  264) offset =  264;

    int final = natural + offset;
    if (final < 0) final = 0;

    d_lightstylevalue[0] = final;
}
```

De esta forma, si el mapa hace parpadear el style 0 (raro pero posible),
el parpadeo se preserva proporcionalmente; si el motor sube/baja ese
valor por cualquier razón, nuestro offset sigue siendo coherente.

Llamar desde `R_RenderView` o `CL_RelinkEntities`, **después** de
`R_AnimateLight` para que el `natural` refleje el valor de este frame.

**Consecuencia automática**: `R_BuildLightMap` detecta que el cached
value del style ha cambiado para cualquier surface que lo use y
regenera su lumel en ese frame. Ver la lógica en `gl_rsurf.cpp`:

```cpp
for (maps = 0; maps < MAXLIGHTMAPS && s->styles[maps] != 255; maps++)
    if (d_lightstylevalue[s->styles[maps]] != s->cached_light[maps])
        goto dynamic;
```

Eso dispara el upload via `lightmap_modified + glTexSubImage2D`.

## Integración

1. **Nuevo fichero `gl_exposure.cpp`** con la lógica.
2. **Llamada a `Exposure_Update(host_frametime)`** desde algún sitio
   por frame — probablemente `R_RenderView` al principio.
3. **Cvars**:
   - `r_auto_exposure` (0/1, default 1).
   - `r_auto_exposure_ref` (default 0.45).
   - `r_auto_exposure_min` (default 0.5).
   - `r_auto_exposure_max` (default 2.0).
   - `r_auto_exposure_rate` (tiempo de adaptación, default 1.0 s).
   - `r_auto_exposure_debug` — imprime cada 0.5s la medición y el
     factor.
4. **Cleanup**: al desactivar el cvar o cambiar de mapa, restaurar
   `d_lightstylevalue[0] = 264`.

## Riesgos

- **Coste de rebuild**: subir/bajar `d_lightstylevalue[0]` cada frame
  por una cantidad pequeña re-construye TODOS los lumels de paredes y
  suelos (casi todos usan style 0). Sube el coste CPU del lightmap
  path. Mitigación: solo actualizar si el cambio vs la última
  aplicación supera un umbral (p.ej. 2 unidades enteras de 264) — así
  evitamos rebuilds por fluctuaciones de interpolación sub-enteras.
- **No afecta a caras con style != 0**: las antorchas parpadeantes
  usan styles 1-6. Esas seguirán con su comportamiento normal, lo cual
  es correcto — no queremos que nuestra "adaptación del ojo" haga
  parpadear las luces.
- **Interacción con `gl_overbrights`**: si el engine permite valores >
  255 en el lightmap (overbright), la escala por encima de 1.0 puede
  saturar los canales 8-bit. Depende del atlas format; en este engine
  los lumels se almacenan en RGBA8 (`GL_RGBA8`), así que el clamp
  ocurre al store. No hay HDR — nuestro rango debe mantenerse razonable.
- **Al morir / spectator**: si el view origin pasa por un leaf sin
  lighting, `R_LightPoint` devuelve 255/255/255 (full bright) y la
  exposición colapsa a 0.5. Meter un fallback: si el leaf es
  `CONTENTS_SOLID` o similar, no actualizar ese frame.

## Casos límite

- **Cambio de mapa**: `R_NewMap` resetea `d_lightstylevalue[0]` a 264.
  Hay que resetear también `current_factor = 1.0f` para que no arrastre
  estado de la sesión anterior.
- **Demo / replay**: la exposición se calcula client-side cada frame, no
  se graba. Misma demo reproduce distinto si el cvar cambia. Aceptable.
- **Multijugador**: puramente client-side, no afecta a lo que ven los
  otros jugadores.

## Debug y tuning

Cvar `r_auto_exposure_debug 1`:

```
[exposure] brightness=0.12 target=2.00 current=1.47 natural=264 offset=+124 -> 388
[exposure] brightness=0.68 target=0.66 current=0.72 natural=264 offset=-74  -> 190
```

El `offset` es la contribución aditiva; el valor final es
`natural + offset`.

## Extensión futura

- **Asimetría**: adaptar más lento a oscuridad que a luz (al entrar en
  un túnel tardas más en ver que al salir). Dos constantes distintas.
- **Sin raycast**: usar el lightmap del leaf en el que está el jugador
  como medida. Más barato que `R_LightPoint` porque no traza ray. Sería
  un single-lookup en `cl.worldmodel->lightdata`.
- **Flash blanco al salir a la luz fuerte**: si `brightness` sube muy
  rápido y el delta pasa un umbral, inyectar un bonus flash blanco vía
  `cl.cshifts[CSHIFT_BONUS]` — ya lo tenemos implementado para
  explosiones, reutilizable.
