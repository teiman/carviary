# Slide

Mediante varios trucos y trampas que parece que solo van a funcionar en
singleplayer, vamos a crear un sistema de "slide" (deslizamiento).

## Idea general

Cuando el jugador ejecuta `slide` estando en movimiento, durante un breve
periodo de tiempo se aplican dos efectos simultáneos:

- **Velocidad**: el jugador se mueve más rápido durante el slide. Sumar a
  `velocity` directamente no sirve, porque el servidor recorta cada frame la
  velocidad *deseada* (`wishspeed`) contra `sv_maxspeed` (default 320) en dos
  puntos:
    - [sv_user.cpp:333-337](src_sdl3/sv_user.cpp#L333-L337) en `SV_AirMove`
      (movimiento normal en tierra y aire).
    - [sv_user.cpp:243-247](src_sdl3/sv_user.cpp#L243-L247) en `SV_WaterMove`.

  La forma limpia: encapsular esa lectura en una función que devuelva el
  **valor efectivo** del cap, p.ej. `SV_GetEffectiveMaxSpeed()`. Por defecto
  devuelve `sv_maxspeed.value`; mientras el slide está activo, le suma el
  delta dictado por la curva del slide (`slide_speed_bonus * curve(t)`).
  Las dos lecturas de `sv_maxspeed.value` arriba se sustituyen por la
  llamada — el clamp sigue ocurriendo en el mismo sitio, pero contra un
  techo dinámico.

  Ventaja: la cvar global no se toca, no se replica, no se persiste en
  savegames, y modular el delta en el tiempo (la "curva suave" que pide la
  sección de Sensación) es trivial porque la función se evalúa cada frame.

  Nota: `cl_forwardspeed` ([cl_input.cpp:223](src_sdl3/cl_input.cpp#L223)) **no
  es el cap real**, sólo la magnitud que el cliente mete en `cmd.forwardmove`.
  El cap autoritativo es server-side.
- **Cámara**: se añade un offset a la altura de la vista que la baja, como si
  el jugador se hubiera agachado.

Ambos deltas son **temporales** y **client-side cuando sea posible** — el lado
servidor se entera lo mínimo imprescindible.

## Duración

- La duración base del slide es de **2 segundos**.
- Si durante el slide el jugador está descendiendo por una superficie
  inclinada (rampa, escaleras), la duración se amplía, simulando que se está
  deslizando cuesta abajo. La amplitud del bonus depende de la pendiente.

## Sensación

Los cambios no son brutales, sino graduales:

- La bajada de cámara entra y sale con una curva suave (ease-in / ease-out),
  no con un escalón.
- La velocidad extra también se atenúa hacia el final del slide en lugar de
  cortarse de golpe.

El resultado debe ser que la animación de agacharse y volver a ponerse
derecho sea creíble y divertida.

## Cómo testearlo

Bindear `shift` al comando `slide`, que inicia el efecto:

```
bind SHIFT "slide"
```

## Pendiente / a decidir

- Curva exacta de la cámara al bajar y al subir (¿lerp lineal, smoothstep,
  cubic ease-out?).
- Curva de la velocidad extra a lo largo de los 2 segundos.
- Cuánto se amplía la duración por grado de pendiente al descender.
- ¿Hay cooldown entre slides, o se puede encadenar libremente?
- ¿El slide se cancela si el jugador salta, dispara, o cambia de dirección?
- ¿Funciona también en multijugador con predicción de cliente, o lo dejamos
  estrictamente single-player como dice arriba?
