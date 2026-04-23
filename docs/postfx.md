# PostFX pipeline

## Flujo por frame

1. `PostFX_BeginScene()` — bindea el scene FBO (2 attachments + depth texture) y limpia el mask.
2. Todo el dibujo 3D (world, alias, particles, agua, humo, llamas, magic, etc.) escribe al scene FBO.
3. `PostFX_EndScene()`:
   - Blur ping-pong del `fb_mask` (separable horizontal/vertical, N iteraciones = `r_fb_glow_radius`).
   - Composite al default framebuffer: `scene + glow * strength * visible`.
4. HUD/consola se pintan al default FB *después* de `EndScene`.

## Attachments del scene FBO

| slot | textura              | contenido                         |
|------|----------------------|-----------------------------------|
| 0    | `scene_color_tex`    | imagen final de la escena         |
| 1    | `scene_fbmask_tex`   | máscara fullbright para bloom     |
| —    | `scene_depth_tex`    | depth del scene (se muestrea post)|

Ambos color attachments están activos durante TODO el frame 3D. `glDrawBuffers` no se toca entre pases.

## Contrato de los fragment shaders de escena

Todo FS que se ejecute dentro de `Begin/EndScene` **debe** declarar los dos outputs:

```glsl
layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec4 frag_fbmask;
```

Si se omite `location=1`, el attachment 1 queda indefinido en los píxels que el FS toca → glow fantasma o tags que no se borran cuando un ocluyente pasa por encima. Es un bug silencioso — no da error de compilación ni runtime.

### Qué escribir al `fb_mask`

#### Superficie opaca no emisiva (paredes, agua, humo, etc.)

```glsl
frag_fbmask = vec4(0.0);
```

El cero borra cualquier tag previo en ese píxel. Es lo que hace que puertas/brushmodels ocluyan correctamente el glow de paneles fullbright detrás.

#### Superficie emisiva (fullbright, flames, embers, gunshot glow, magic core)

```glsl
frag_fbmask = vec4(premultiplied_rgb, gl_FragCoord.z);
```

- **RGB** = contribución de color al glow, premultiplicada por alpha si aplica (`c.rgb * c.a`).
- **Alpha** = depth lineal del emisor en clip space. **Crítico**: el composite rechaza el glow si hay un ocluyente delante. Si emites `alpha=0`, el pase de rejection trata el tap como "sin emisor" → tu glow nunca se verá.

### Shaders con dual-source blending (e.g. `gl_magic.cpp`)

Coexisten los tres outputs:

```glsl
layout(location = 0, index = 0) out vec4 frag_color;
layout(location = 0, index = 1) out vec4 frag_factor;
layout(location = 1)            out vec4 frag_fbmask;
```

## Semántica del blur

Separable gaussian 9-tap con **propagación min() del alpha**: el alpha del pixel resultante es el `min` de los alphas de los taps cuyo alpha era > 0. Esto significa que `fb_mask.a` tras el blur es el depth del **emisor más cercano a la cámara** que ha contribuido a ese pixel.

Si ninguna muestra tenía emisor (todos alpha = 0), el pixel final tiene `alpha = 0` → composite lo interpreta como "no hay glow aquí".

## Semántica del composite

```glsl
float emitter_depth = glow.a;
float scene_depth   = texture(u_depth, uv).r;
float visible = (emitter_depth > 0.0) ?
                step(emitter_depth, scene_depth + 0.0005) : 0.0;
frag_color = vec4(scene + glow.rgb * u_strength * visible, 1.0);
```

Si `scene_depth < emitter_depth` → hay un píxel de la escena más cerca que el emisor que alcanzó aquí → **rejection**, no se suma glow. Evita el bleed a través de ocluyentes.

Tolerancia 0.0005 absorbe ruido de precisión. Si un emisor queda exactamente pegado a una superficie no emisiva y en algún ángulo el glow desaparece, bajar esa tolerancia (o convertir a depth lineal) ayudaría.

## Cvars

| cvar                   | default | efecto                                              |
|------------------------|---------|-----------------------------------------------------|
| `r_fb_glow`            | 1       | enciende/apaga el pipeline de glow                  |
| `r_fb_glow_radius`     | 12      | iteraciones de blur (separable). Cap 32.            |
| `r_fb_glow_strength`   | 1.0     | factor multiplicativo del glow al componer          |
| `r_fb_debug`           | 0       | 1 → muestra el mask (blurreado) en vez de la escena |

## Cómo añadir un shader de escena nuevo

1. Declarar los dos outputs en el FS.
2. Decidir si el pixel es emisivo (→ escribe color + depth al mask) o no (→ escribe `vec4(0)`).
3. Asegurar que el pase se ejecuta entre `PostFX_BeginScene()` y `PostFX_EndScene()`.

## Cómo añadir otro post-process

Punto de entrada: `PostFX_EndScene` en `gl_postfx.cpp`. El orden actual es:

1. Blur ping-pong sobre `fb_mask`.
2. Composite.

Añadir un paso extra (p.ej. distortion) es un shader nuevo que lee `scene_color_tex`/`scene_depth_tex` y escribe a un FBO auxiliar o directamente al default. La arquitectura no se rompe; solo añades un paso entre el blur y el composite, o después del composite.

## Coste GPU aproximado

A 1920×1080 con `r_fb_glow_radius 12`:

- 24 passes full-screen de 9-tap (horizontal + vertical) ≈ 24M fetches × 9 ≈ 216M texels por frame.
- Negligible en GPUs modernas (< 1 ms). En 4K multiplica x4 → vigilar si se añaden más post-process.
