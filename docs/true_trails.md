# True Trails

## Resumen

Sistema de estelas sólidas (no particulares) para proyectiles. En lugar de los puffs actuales que emite `R_RocketTrail` y compañía, las true trails renderizan una **tira continua de quads** que sigue al objeto y queda orientada al jugador (billboarded al eje de la estela).

## Comportamiento visual

- Los quads forman una cinta contigua entre una posición anterior y la actual del proyectil.
- La tira se orienta al viewer (billboarding tipo *beam*): el eje largo sigue la trayectoria, el eje corto mira a la cámara.
- Las estelas **desaparecen rápidamente** (fade-out corto, del orden de 0.2–0.4 s) para no saturar la escena ni dejar rastros largos.
- El ancho y duración dependen del tipo de proyectil (ver tabla).

## Tipos de estela

| Proyectil | Color | Ancho | Duración |
|-----------|-------|-------|----------|
| Spike (clavo normal) | Blanco | Pequeño | Corta |
| Wiz spike | Verde | Pequeño | Corta |
| Objetos con `EF_ROCKET` / flag de misil | (definir) | Más ancho | Más larga |

## Integración con el código existente

- Punto de llamada para rockets/grenades: [cl_main.cpp:639-648](src_sdl3/cl_main.cpp#L639-L648), donde hoy se invoca `R_RocketTrail`.
- Los spikes y wiz spikes no tienen flag de modelo equivalente: se generan como entidades con `MOD_SPIKE` / equivalente — habrá que localizar el sitio donde se actualiza su `oldorg → origin` por frame y añadir allí la llamada a la nueva función (p. ej. `R_SpikeTrail`, `R_WizSpikeTrail`).
- `R_RocketTrail` vive en [gl_part.cpp:1341](src_sdl3/gl_part.cpp#L1341) y usa el sistema de partículas. La nueva API debe ser independiente — *no* reutilizar `addParticle()`, ya que los quads necesitan topología de tira y vida propia.

## Propuesta de API

```cpp
// nuevo módulo, p. ej. gl_truetrail.cpp
void R_TrueTrail_Spike     (vec3_t start, vec3_t end, entity_t *ent);
void R_TrueTrail_WizSpike  (vec3_t start, vec3_t end, entity_t *ent);
void R_TrueTrail_Missile   (vec3_t start, vec3_t end, entity_t *ent); // EF_ROCKET
void R_TrueTrail_Draw      (void); // llamado desde R_RenderView tras el mundo
void R_TrueTrail_Clear     (void); // en nivel nuevo
```

Estructura interna sugerida: buffer circular de segmentos `{ vec3 a, vec3 b, float spawn_time, float life, rgba color, float width }` — cada frame se descarta lo caducado y se emiten quads para lo vivo.

## Cuestiones abiertas

- ¿Color exacto del trail de misil? 
>humo gris blanco
- ¿Las true trails **sustituyen** a los puffs de partículas o **coexisten**? 
> sustituyen (de momento)
- Blending: additivo (tipo beam láser) vs. alpha normal con textura de degradado. El additivo encaja con el look del motor; alpha normal permite trails más "materiales" (humo).
> no lo se, prueba lo que te parezca mas logico
- Comportamiento bajo agua: ¿la estela se corta o cambia a burbujas como hace hoy `R_RocketTrail`?
> cuando se inicie un efecto de particulas o estelas bajo el agua, las particulas deben ser o parecer circulos (burbujas) durante toda su vida.  No solo este efecto, todos los efectos deben testear primero si es bajo el agua, y si es eso, utilizar particulas que recuerdan a burbujas.
