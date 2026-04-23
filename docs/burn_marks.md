# Marcas quemadas en el lightmap

Feature: explosiones (y potencialmente otros eventos: lava splash, rayos,
impactos muy grandes) dejan **marcas oscuras permanentes en la superficie
donde ocurren**, que perduran lo que dure el mapa. Implementadas como
modificación directa de los lightmaps BSP.

## Por qué el lightmap

El engine ya tiene:
- Un atlas de lightmaps en GPU ([gl_rsurf.cpp](../src_sdl3/gl_rsurf.cpp)).
- El mecanismo `lightmap_modified[]` + `glTexSubImage2D` para subir cambios.
- Un shader de paredes que multiplica diffuse × lightmap.

Modificar lumels → la pared se oscurece de verdad bajo la luz, sin necesidad
de otro pass de render, sin drawcalls extra, y se integra con las sombras de
dlights automáticamente. Las marcas no se "dibujan encima" — **son** la
iluminación de esa zona.

## Estado actual

- `R_BuildLightMap` ([gl_rsurf.cpp:264](../src_sdl3/gl_rsurf.cpp#L264)):
  construye los lumels RGB de una cara a partir de `surf->samples` (estáticos
  del BSP) más dlights. Se llama una vez al cargar el mapa.
- `lightmap_modified[]`: flag por atlas-page. Cuando se pone a true,
  `glTexSubImage2D` sube la región dirty en el siguiente frame.
- `GL_CreateSurfaceLightmap`: asigna el slot `(light_s, light_t,
  lightmaptexturenum)` a cada cara.
- Sky y water saltan el build (sky por diseño, water por legacy).

Lo que NO existe todavía: una forma de modificar lumels **persistentemente**
después del build inicial. `R_BuildLightMap` se puede re-llamar por cara pero
siempre recalcula desde `samples` + dlights — no tiene memoria de cambios.

## Plan

### 1. Añadir capa de modificación persistente

Nuevo array paralelo `byte *surf->burn_overlay` por cara (o en un array
`burn_overlay[num_surfaces][smax*tmax]` aparte para no tocar `msurface_t`).
Contiene factor de oscurecimiento por lumel, rango `[0..255]` donde 0 = no
afectado y 255 = completamente negro.

Alternativa más simple: un segundo atlas paralelo `burn_atlas` del mismo
tamaño que `lightmaps[]`. `R_BuildLightMap` multiplica el lumel final por
`(255 - burn) / 255` antes de escribir.

Voy con la alternativa — menos memoria (un solo byte por lumel, no RGB), más
fácil de gestionar.

### 2. API pública

```cpp
// Apply a burn mark centered at world position `pos` with radius r.
// Intensity 0..1. Scans all lightmapped surfaces near `pos`, projects
// onto each, and stamps the burn_atlas falling off with distance.
void Burn_Stamp (const vec3_t pos, float radius, float intensity);
```

Llamado desde:
- `Explosion_Spawn` (gl_explosion.cpp) → `Burn_Stamp(origin, 48.0f, 0.8f)`.
- Posiblemente `Gunshot_Register` con radio mucho menor y baja intensidad si
  queremos quemaduras por balas.

### 3. Estampado por cara

Para cada cara candidata:

1. Filtrar: saltar `SURF_DRAWSKY | SURF_DRAWTURB`, y caras fuera de un AABB
   de `pos ± radius`.
2. Proyectar `pos` al plano de la cara. Si la distancia perpendicular es
   mayor que `radius`, saltar.
3. Convertir la posición proyectada a coordenadas lumel usando los mismos
   `texinfo->vecs` que usa `R_BuildLightMap`:
   ```cpp
   ds = DotProduct(proj, vecs[0]) + vecs[0][3] - texturemins[0];
   dt = DotProduct(proj, vecs[1]) + vecs[1][3] - texturemins[1];
   int lumel_s = ds >> 4;
   int lumel_t = dt >> 4;
   ```
4. Escribir en `burn_atlas` alrededor de `(lumel_s, lumel_t)` con falloff
   (ej. Gaussiano o cuadrático) sobre un radio en lumels =
   `radius / 16`.
5. Marcar `lightmap_modified[lightmaptexturenum] = true` y actualizar el
   `lightmap_rectchange` para que glTexSubImage2D suba la región.

### 4. Modificar R_BuildLightMap

Tras el cálculo normal (samples + dlights), multiplicar cada componente por
`(255 - burn) / 255`. Añade 3 multiplicaciones por lumel — coste mínimo.

### 5. Rebuild on demand

Cuando `Burn_Stamp` añade quemaduras a una cara, forzar un rebuild de esa
cara: `R_BuildLightMap(surf, base, BLOCK_WIDTH*lightmap_bytes)`. Esto
reescribe el atlas en CPU; la subida a GPU se hace por el mecanismo existente
`lightmap_modified`.

### 6. Persistencia entre mapas

`R_ClearBurnMarks` llamado desde `GL_BuildLightmaps` al inicio. Las marcas
viven solo dentro de una sesión del mapa actual.

## Complicaciones conocidas

- **Caras muy subdivididas**: una explosión cerca de un corner lit puede
  afectar a varias caras. El scan por AABB + plane distance resuelve esto.
- **Resolución baja**: lumels miden 16 unidades de mundo. Una quemadura de
  radio 48 cubre solo 3-4 lumels por eje, el borde es pixelado. `GL_LINEAR`
  en el atlas suaviza pero sigue siendo tosco. Aceptable dado el look retro
  del engine.
- **Dlights interactúan mal**: un dlight pasando por una zona quemada la
  rebaja mucho (multiplicación en cadena). Puede quedar visualmente bien
  ("ahí hubo fuego y está apagado") o mal según el caso. Si se ve mal, hacer
  el burn aditivo en lugar de multiplicativo: `final -= burn_darken` con
  clamp a 0, aplicado solo al lumel estático pre-dlight.
- **Fuga entre caras adyacentes**: dos caras con edge compartido pueden
  mostrar transición visible si una está quemada y la otra no. Se puede
  mitigar propagando las quemaduras por edges compartidos al estampar
  (igual que hicimos con el shore mask del agua).
- **Memoria**: un atlas 128×128 RGBA = 64KB. Con `MAX_LIGHTMAPS=512` = 32MB.
  Un segundo atlas byte (mono) del mismo tamaño añade 8MB. Aceptable.

## Orden de implementación sugerido

1. **Spike**: asignar `burn_atlas`, llamar stamp hardcoded al inicio de una
   cara cualquiera, ver que aparece oscurecido. Validar pipeline.
2. **Proyección correcta**: implementar `Burn_Stamp` con el scan de caras.
   Probar con un comando de consola (`burn`) que quema donde mira el
   jugador.
3. **Integración**: llamar desde `Explosion_Spawn`. Tunear radio e
   intensidad.
4. **Edge propagation** si se ven costuras feas entre caras.
5. **Clear on map change**: `R_ClearBurnMarks` en `R_NewMap`.

## Archivos a tocar

| Archivo | Cambio |
|---------|--------|
| `gl_rsurf.cpp` | `burn_atlas`, `R_BuildLightMap` modificado, `Burn_Stamp` |
| `gl_rsurf.cpp` | `R_ClearBurnMarks` llamado desde `R_NewMap` |
| `gl_explosion.cpp` | llamar `Burn_Stamp` en `Explosion_Spawn` |
| `headers/render.h` | prototipo de `Burn_Stamp` |

## Alternativas que descarté

- **Decal textures sobre paredes**: dibujar un quad con textura de quemadura
  encima de la pared. Pros: fácil, bonito, se ve con textura real. Contras:
  otro pass de draw, Z-fighting, no interactúa con la iluminación.
- **Modificar `samples` del BSP en RAM**: sí funcionaría pero perderíamos el
  valor original para las dlights, y no hay forma limpia de separar
  contribución estática vs dinámica sin copia.
- **Shader con sampler extra**: samplear un "burn texture" además del
  lightmap en el FS de paredes. Implica tocar vertex format y el shader
  opaque — alto riesgo (ver lo que pasó con el shader de agua).
