# Estrategia de mejora testeada

Proceso para validar visualmente que un cambio en el motor no introduce regresiones graficas.

## Pasos

1. **Antes del cambio:** Reproducir una demo, capturando pantalla cada 100 frames. Guardar las capturas en `antes/`.
2. **Aplicar el cambio** en el codigo del motor.
3. **Despues del cambio:** Reproducir la misma demo, capturando pantalla cada 100 frames. Guardar las capturas en `ahora/`.
4. **Comparar** las capturas de `antes/` vs `ahora/` para detectar diferencias o regresiones visuales.

## Notas

- Skill de Claude Code disponible: `/mejora-testeada` (en `.claude/commands/mejora-testeada.md`).
