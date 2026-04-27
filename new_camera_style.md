# Nuevo estilo de cámara

Experimento con un tipo de cámara desacoplada del crosshair, puramente client-side.

## Principios

- **Servidor ajeno**: el lado servidor no se entera de nada. Todo ocurre en el cliente; el servidor sigue recibiendo una dirección de mira "normal".
- **Crosshair confinado**: el crosshair se mueve dentro de un rectángulo tumbado (más ancho que alto) en pantalla. Al mover el ratón, el crosshair se desplaza dentro de esa área en lugar de arrastrar la cámara directamente.
- **Dirección enviada**: la dirección que se envía al servidor como `viewangles` es la que apunta el crosshair, no la de la cámara.

## Cámara

- La cámara sigue al crosshair con una animación suave (lerp / damping).
- La resistencia aumenta cuanto más cerca está el crosshair del borde del rectángulo: cerca del centro la cámara apenas se mueve; cerca del borde tira con más fuerza para reencuadrar.
- Esto produce un efecto tipo "soft follow" en lugar del enganche rígido clásico de FPS.

## Arma en primera persona (`v_weapon`)

- El `v_weapon` apunta hacia donde mira el crosshair, no hacia donde mira la cámara.
- Como consecuencia, respecto al encuadre de la cámara el arma puede aparecer ligeramente inclinada hacia arriba, abajo, izquierda o derecha según la posición del crosshair dentro del rectángulo.
- Refuerza visualmente la separación entre "a dónde miro" y "a dónde apunto".

## Pendiente / a decidir

- Tamaño y proporción exactos del rectángulo del crosshair.
- Curva de damping de la cámara en función de la distancia al borde.
- Comportamiento al disparar: ¿retroceso afecta al crosshair, a la cámara, o a ambos?
- Cómo se reconcilia con `cl.viewangles` y la predicción de cliente.
