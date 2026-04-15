# Modo Offline - Bypass de red para Single Player

Dentro de `src_sdl3/`, optimizar el single player local saltandose la pila de red.

Desarrollo incremental de facil a dificil. Cada paso se testea antes de continuar. Si el motor se vuelve inestable, paramos y revertimos al ultimo paso estable.

## Problema actual

En single player, cada frame el engine:
1. **Servidor**: serializa entidades, sonidos, particulas a bytes en un buffer
2. **Loopback**: copia el buffer entero al socket del peer (memcpy)
3. **Loopback**: copia otra vez al `net_message` global (memcpy)
4. **Cliente**: deserializa byte a byte con `MSG_ReadByte/Short/Float/Coord/Angle`

Son 2-3 memcpys de hasta 64KB + serializacion/deserializacion completa por frame, para datos que ya estan en memoria del mismo proceso.

## Condicion de activacion

```c
qboolean offline_mode = (sv.active && svs.maxclients == 1 && 
                         !cls.demorecording && !cls.demoplayback);
```

---

## Paso 0: Infraestructura (trivial)

**Que**: Añadir `extern qboolean offline_mode` global, calcularlo al inicio de cada frame en `_Host_Frame()`.

**Dificultad**: Ninguna. Es una variable y un if.

**Riesgo**: Cero. No cambia ningun comportamiento, solo añade la variable.

**Test**: Compilar, jugar normal. Nada debe cambiar.

**Ficheros**: `host.cpp`, un nuevo header o al inicio de `host.cpp`

---

## Paso 1: Bypass de sonidos (facil, alto impacto)

**Que**: En `SV_StartSound()`, si `offline_mode`, llamar `S_StartSound()` directamente y saltar la serializacion a `sv.datagram`.

**Dificultad**: Baja. Un if al inicio de una funcion. Los parametros ya estan disponibles.

**Riesgo**: Bajo. Si falla, los sonidos no se oyen o suenan mal. Facil de detectar.

**Test**: Cargar un mapa, disparar, abrir puertas. Los sonidos deben sonar identicos.

**Ficheros**: `sv_main.cpp` (SV_StartSound)

```c
if (offline_mode)
{
    vec3_t origin;
    for (int i = 0; i < 3; i++)
        origin[i] = entity->v.origin[i] + 0.5*(entity->v.mins[i]+entity->v.maxs[i]);
    S_StartSound(ent, channel, cl.sound_precache[sound_num], origin, volume/255.0, attenuation);
    return;
}
```

---

## Paso 2: Bypass de particulas (facil)

**Que**: En `SV_StartParticle()`, si `offline_mode`, llamar `R_RunParticleEffect()` directamente.

**Dificultad**: Baja. Mismo patron que sonidos.

**Riesgo**: Bajo. Si falla, particulas no se ven o se ven mal.

**Test**: Disparar a paredes (impactos), explosiones. Particulas deben verse igual.

**Ficheros**: `sv_main.cpp` (SV_StartParticle)

---

## Paso 3: Bypass de client data / stats (medio-facil)

**Que**: En `SV_WriteClientdataToMessage()`, si `offline_mode`, copiar los stats del jugador directamente a `cl.stats[]` sin serializar.

**Dificultad**: Media-baja. Hay que mapear los campos del servidor (`sv_player->v.health`, etc.) a los stats del cliente (`cl.stats[STAT_HEALTH]`, etc.). Son ~15 campos conocidos.

**Riesgo**: Medio-bajo. Si un stat se mapea mal, el HUD muestra valores incorrectos. Facil de verificar visualmente.

**Test**: Recoger items, recibir daño, cambiar arma. El HUD debe actualizarse correctamente.

**Ficheros**: `sv_main.cpp` (SV_WriteClientdataToMessage), `cl_parse.cpp` (referencia de como el cliente lee los stats)

---

## Paso 4: Bypass de temp entities (medio)

**Que**: Interceptar las escrituras de temp entities (explosiones, beams, sangre) cuando van a `sv.datagram` o `sv.reliable_datagram` en modo offline, y ejecutar el efecto directamente en el cliente.

**Dificultad**: Media. Los temp entities se escriben desde QuakeC via `PF_WriteByte/WriteShort/etc` como secuencias de bytes. Hay que interceptar en `PF_WriteByte` cuando `dest == MSG_BROADCAST` y reconstruir el temp entity para ejecutarlo directamente.

**Riesgo**: Medio. Muchos tipos de temp entities (TE_SPIKE, TE_EXPLOSION, TE_BEAM, etc.) con formatos diferentes. Facil que alguno falle.

**Test**: Combate con varios enemigos. Explosiones de rockets, impactos de clavos, rayos de shambler. Todo debe verse igual.

**Ficheros**: `pr_cmds.cpp` (PF_WriteByte y familia), `cl_tent.cpp` (referencia de como el cliente parsea cada TE_)

---

## Paso 5: Bypass de entity updates (dificil, mayor impacto)

**Que**: Reemplazar `SV_WriteEntitiesToClient()` + loopback + `CL_ParseUpdate()` con un path directo que copie el estado de las entidades del servidor al cliente sin serializar.

**Dificultad**: Alta. El servidor usa `edict_t` y el cliente `entity_t` - estructuras diferentes. Hay que:
1. Hacer el PVS check (misma logica que ahora)
2. Para cada entidad visible, copiar origin/angles/model/frame/skin/effects/alpha/scale directamente
3. Manejar la interpolacion del cliente (lerp de posiciones entre frames)
4. Manejar entidades que entran/salen del PVS

**Riesgo**: Alto. La interpolacion de entidades es delicada. Si se rompe, los monstruos se teletransportan en vez de moverse suavemente, o aparecen/desaparecen de forma erratica.

**Test**: Combate completo. Monstruos moviendose, puertas, plataformas, items flotando. Movimiento debe ser suave, sin saltos.

**Ficheros**: `sv_main.cpp` (nuevo SV_DirectUpdateEntities), `cl_main.cpp` (CL_RelinkEntities), `host.cpp` (nuevo path en el game loop)

---

## Paso 6: Bypass del game loop completo (muy dificil)

**Que**: Reemplazar el flujo completo de `CL_SendCmd -> red -> servidor` y `servidor -> red -> CL_ReadFromServer` con un path directo que:
1. Aplique el input del jugador directamente al servidor sin `MSG_WriteAngle`
2. Ejecute `SV_Physics` 
3. Copie el resultado directamente al cliente sin parsear

**Dificultad**: Muy alta. Es esencialmente reescribir el game loop para single player. Requiere entender todas las interacciones entre cliente y servidor.

**Riesgo**: Muy alto. Aqui es donde probablemente hay que parar si los pasos anteriores ya dan suficiente beneficio.

**Ficheros**: `host.cpp` (_Host_Frame), `cl_input.cpp`, `sv_user.cpp`

---

## Criterios para parar

Parar y quedarse en el ultimo paso estable si:
- El motor crashea de forma no obvia (corrupcion de memoria, crashes aleatorios)
- Los efectos visuales se rompen de manera dificil de diagnosticar
- La ganancia de rendimiento del siguiente paso no justifica la complejidad
- El paso requiere cambios en >5 ficheros con interacciones no triviales

## Notas

- Cuando `cls.demorecording` es true, usar siempre el path normal (la demo necesita los mensajes serializados)
- Cada bypass es independiente y tiene su propio `if (offline_mode)` check
- Si un paso causa problemas, se puede desactivar individualmente sin afectar a los demas
- El mayor beneficio para AD probablemente viene de los pasos 1-4 combinados, sin necesidad de llegar al paso 5
