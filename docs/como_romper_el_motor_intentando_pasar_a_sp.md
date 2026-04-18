# Experimental: Eliminar el componente multiplayer

## Objetivo

Simplificar la arquitectura eliminando la separacion entre cliente/servidor y unificando el manejo de entidades.

## Cambios propuestos

- **Una sola lista de entidades**: Reemplazar las listas separadas de cliente y servidor por una unica lista unificada.
- **Renderizado basado en visibilidad**: Se recorre la lista unica y se renderiza cada entidad segun si es visible o no.
- **Eliminar codigo de red**: Quitar toda la capa de networking (cliente/servidor) que ya no seria necesaria.

## Beneficios esperados

- Codigo mas simple y facil de mantener.
- Menos indirecciones al acceder a las entidades.
- Eliminacion de sincronizacion redundante entre listas.

## Analisis de viabilidad

### A favor

- Ya existe un flag `offline_mode` en `sv_main.cpp` que bypasea la serializacion de red y copia datos directamente. El engine ya contempla funcionar sin red.
- La lista de visibles `cl_visedicts[]` en `cl_main.cpp` ya es la lista unica que alimenta al renderer.
- Las entidades estaticas (`cl_static_entities[]`) ya no pasan por red, demostrando que el patron "entidad sin networking" ya existe.

### Dificultades

- **Dualidad de estructuras**: `edict_t` (servidor, en `progs.h`) y `entity_t` (cliente, en `render.h`) son tipos completamente distintos. Unificarlas requiere redisenar ambas.
- **Protocolo delta hardcodeado**: Los bits `U_ORIGIN`, `U_ANGLE`, etc. estan incrustados en `SV_WriteEntitiesToClient()` (~230 lineas) y `CL_ParseUpdate()` (~200 lineas). Eliminarlos implica reescribir como las entidades pasan del mundo logico al visual.
- **Interpolacion depende de red**: `CL_RelinkEntities()` en `cl_main.cpp` usa timestamps de red (`msgtime`, `msg_origins[]`) para interpolar movimiento. Sin red necesita un mecanismo alternativo.
- **PVS en dos capas**: El servidor calcula visibilidad con `SV_FatPVS()` para decidir que enviar, y el cliente la recalcula con `R_MarkLeaves()` para renderizar. Hay que decidir donde unificar esta logica.
- **~6000+ lineas de networking** distribuidas en `net_main.cpp`, `net_dgrm.cpp`, `net_bsd.cpp`, `net_loop.cpp`, `net_sdl.cpp`.

### Valoracion

| Aspecto             | Nivel |
|---------------------|-------|
| Beneficio           | Alto  |
| Complejidad         | Alta  |
| Riesgo de regresion | Alto  |
| Esfuerzo            | Grande |

## Estrategia recomendada: enfoque incremental

En lugar de eliminar el multiplayer de golpe, hacerlo por fases minimiza el riesgo:

### Fase 1 - Bypass directo en offline

Expandir `offline_mode` para que copie directamente de `edict_t` a `cl_visedicts[]`, saltandose `CL_ParseUpdate()` y toda la serializacion.

### Fase 2 - Interpolacion sin red

Hacer que `CL_RelinkEntities()` funcione sin timestamps de red cuando esta en modo offline. Usar directamente el tiempo del servidor local.

### Fase 3 - Codigo de red opcional

Una vez validadas las fases anteriores, marcar el codigo de red como opcional con `#ifdef MULTIPLAYER` y excluirlo de la compilacion.

## Tareas pendientes

- [ ] Identificar todas las referencias al componente multiplayer.
- [ ] Unificar las estructuras de datos de entidades.
- [ ] Adaptar el bucle de renderizado a la lista unica.
- [ ] Verificar que no se rompen dependencias con el resto del engine.
- [ ] Fase 1: Expandir `offline_mode` para bypass directo edict_t -> cl_visedicts[].
- [ ] Fase 2: Adaptar `CL_RelinkEntities()` para interpolacion sin timestamps de red.
- [ ] Fase 3: Envolver codigo de red en `#ifdef MULTIPLAYER`.
