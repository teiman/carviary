# Carviary vs Ironwail Delta

Comparison of Carviary (C++ port of TomazQuake) source files against Ironwail (modern Quake engine).
Diff % = changed lines / (carv lines + iron lines). Higher = more different.

## Matched Files
| Carviary | Ironwail | Carv Lines | Iron Lines | Diff Lines | Diff % | Similarity |
|----------|----------|-----------|-----------|-----------|--------|------------|
| `menu.cpp`               | `menu.c`                 |       464 |      7629 |      7466 |    92% | very different |
| `gl_warp.cpp`            | `gl_warp.c`              |       335 |        25 |       325 |    90% | very different |
| `gl_rmisc.cpp`           | `gl_rmisc.c`             |       296 |      1029 |      1124 |    84% | very different |
| `gl_sky.cpp`             | `gl_sky.c`               |       305 |       835 |       920 |    80% | very different |
| `gl_rmain.cpp`           | `gl_rmain.c`             |      1008 |      2065 |      2423 |    78% | very different |
| `mathlib.cpp`            | `mathlib.c`              |       275 |       933 |       928 |    76% | very different |
| `gl_draw.cpp`            | `gl_draw.c`              |       610 |      1307 |      1469 |    76% | very different |
| `gl_model.cpp`           | `gl_model.c`             |      1771 |      5120 |      5102 |    74% | very different |
| `console.cpp`            | `console.c`              |       738 |      2439 |      2357 |    74% | very different |
| `net_win.cpp`            | `net_win.c`              |       120 |       121 |       171 |    70% | very different |
| `gl_screen.cpp`          | `gl_screen.c`            |       830 |      2172 |      2082 |    69% | diverged       |
| `cvar.cpp`               | `cvar.c`                 |       307 |       743 |       718 |    68% | diverged       |
| `common.cpp`             | `common.c`               |      1763 |      4449 |      4276 |    68% | diverged       |
| `gl_rlight.cpp`          | `gl_rlight.c`            |       436 |       396 |       549 |    65% | diverged       |
| `pr_edict.cpp`           | `pr_edict.c`             |      1129 |      2561 |      2308 |    62% | diverged       |
| `keys.cpp`               | `keys.c`                 |       972 |      1547 |      1575 |    62% | diverged       |
| `sbar.cpp`               | `sbar.c`                 |      1177 |      2153 |      2008 |    60% | diverged       |
| `host_cmd.cpp`           | `host_cmd.c`             |      1674 |      3791 |      3183 |    58% | diverged       |
| `cl_demo.cpp`            | `cl_demo.c`              |       431 |       871 |       766 |    58% | diverged       |
| `snd_mix.cpp`            | `snd_mix.c`              |       388 |       629 |       585 |    57% | diverged       |
| `pr_exec.cpp`            | `pr_exec.c`              |       662 |       694 |       766 |    56% | diverged       |
| `gl_refrag.cpp`          | `gl_refrag.c`            |       234 |       204 |       248 |    56% | diverged       |
| `pr_cmds.cpp`            | `pr_cmds.c`              |      1866 |      3442 |      2842 |    53% | diverged       |
| `chase.cpp`              | `chase.c`                |        95 |       118 |       115 |    53% | diverged       |
| `wad.cpp`                | `wad.c`                  |       325 |       168 |       254 |    51% | diverged       |
| `host.cpp`               | `host.c`                 |       889 |      1530 |      1188 |    49% | diverged       |
| `cmd.cpp`                | `cmd.c`                  |       884 |       993 |       909 |    48% | diverged       |
| `zone.cpp`               | `zone.c`                 |       932 |      1060 |       878 |    44% | diverged       |
| `sv_main.cpp`            | `sv_main.c`              |      1321 |      2082 |      1449 |    42% | diverged       |
| `cl_main.cpp`            | `cl_main.c`              |       779 |      1059 |       786 |    42% | diverged       |
| `cl_parse.cpp`           | `cl_parse.c`             |      1057 |      1390 |      1023 |    41% | diverged       |
| `snd_dma.cpp`            | `snd_dma.c`              |      1017 |      1119 |       872 |    40% | diverged       |
| `net_wins.cpp`           | `net_wins.c`             |       575 |       544 |       445 |    39% | diverged       |
| `cl_tent.cpp`            | `cl_tent.c`              |       416 |       363 |       263 |    33% | diverged       |
| `net_wipx.cpp`           | `net_wipx.c`             |       431 |       448 |       287 |    32% | diverged       |
| `view.cpp`               | `view.c`                 |       976 |      1017 |       593 |    29% | similar        |
| `sv_user.cpp`            | `sv_user.c`              |       622 |       647 |       319 |    25% | similar        |
| `snd_mem.cpp`            | `snd_mem.c`              |       341 |       368 |       173 |    24% | similar        |
| `cl_input.cpp`           | `cl_input.c`             |       452 |       499 |       229 |    24% | similar        |
| `sv_phys.cpp`            | `sv_phys.c`              |      1425 |      1298 |       612 |    22% | similar        |
| `crc.cpp`                | `crc.c`                  |        97 |        85 |        39 |    21% | similar        |
| `world.cpp`              | `world.c`                |       918 |       972 |       392 |    20% | similar        |
| `net_main.cpp`           | `net_main.c`             |       870 |       917 |       342 |    19% | similar        |
| `net_dgrm.cpp`           | `net_dgrm.c`             |      1273 |      1421 |       424 |    15% | similar        |
| `sv_move.cpp`            | `sv_move.c`              |       419 |       419 |        62 |     7% | similar        |
| `net_loop.cpp`           | `net_loop.c`             |       245 |       250 |        29 |     5% | similar        |

## Carviary-only files (no ironwail equivalent)

- `cd_win.cpp`
- `conproc.cpp`
- `gl_crosshair.cpp`
- `gl_flares.cpp`
- `gl_font.cpp`
- `gl_hc_texes.cpp`
- `gl_md2.cpp`
- `gl_mdl.cpp`
- `gl_mirror.cpp`
- `gl_part.cpp`
- `gl_proc.cpp`
- `gl_rscript.cpp`
- `gl_rsurf.cpp`
- `gl_sprite.cpp`
- `gl_texman.cpp`
- `gl_vidnt.cpp`
- `in_win.cpp`
- `menu_help.cpp`
- `menu_main.cpp`
- `menu_multi.cpp`
- `menu_options.cpp`
- `menu_quit.cpp`
- `menu_single.cpp`
- `snd_win.cpp`
- `sys_win.cpp`

## Ironwail-only files (not in carviary)

- `bgmusic.c`
- `cd_null.c`
- `cfgfile.c`
- `gl_fog.c`
- `gl_mesh.c`
- `gl_shaders.c`
- `gl_texmgr.c`
- `gl_vidsdl.c`
- `image.c`
- `in_sdl.c`
- `json.c`
- `lodepng.c`
- `main_sdl.c`
- `miniz.c`
- `net_bsd.c`
- `net_udp.c`
- `pl_linux.c`
- `pl_win.c`
- `quakedef.c`
- `r_alias.c`
- `r_brush.c`
- `r_part.c`
- `r_sprite.c`
- `r_world.c`
- `snd_codec.c`
- `snd_flac.c`
- `snd_mikmod.c`
- `snd_modplug.c`
- `snd_mp3.c`
- `snd_mp3tag.c`
- `snd_mpg123.c`
- `snd_opus.c`
- `snd_sdl.c`
- `snd_umx.c`
- `snd_vorbis.c`
- `snd_wave.c`
- `snd_xmp.c`
- `steam.c`
- `strlcat.c`
- `strlcpy.c`
- `sys_sdl_unix.c`
- `sys_sdl_win.c`

---

## Low Hanging Fruit from Ironwail

### Already Applied (gl_rmain)
- R_CullBox con signbits — culling 4x mas rapido
- glFinish eliminado — ya no stall de GPU cada frame
- Near clip adaptativo — evita ver a traves de paredes con FOV alto
- Check tipo viewmodel — previene crash si el arma no es alias model

### Backport Candidates (sorted by impact)

| # | File | Mejora | Impact | Dificultad |
|---|------|--------|--------|------------|
| 1 | snd_dma | **Sound skip fix**: `rand() % skip` puede exceder longitud del sonido, causando audio corrupto. Ironwail clampea a `sc->length` | Alto | Facil (~8 lineas) |
| 2 | snd_dma | **Ambient volume clamp 255**: sin clamp, ambient muy alto overflow el campo `master_vol` de 8-bit | Alto | Facil (2 lineas) |
| 3 | host | **Host_Error: clear intermission**: sin `cl.intermission = 0`, un error durante intermission deja la pantalla bloqueada | Alto | Facil (1 linea) |
| 4 | cl_main | **CL_LerpPoint nolerp fix**: mover check de `cl_nolerp` despues del calculo de frac para evitar drift de `cl.time` | Alto | Facil (~5 lineas) |
| 5 | cl_parse | **CL_EntityNum negative check**: indice negativo causa out-of-bounds. Ironwail anade validacion | Medio | Facil (3 lineas) |
| 6 | cl_parse | **CL_KeepaliveMessage static buffer**: `byte olddata[8192]` en stack a file-scope static, reduce riesgo de stack overflow | Medio | Facil (1 linea) |
| 7 | zone | **Zone size 1MB a 4MB**: mods modernos necesitan mas zona de memoria | Medio | Facil (1 define) |
| 8 | cl_main | **R_RemoveEfrags no-op**: efrags no se usan para entidades dinamicas en GLQuake, llamada innecesaria | Bajo | Facil (comentar 2 lineas) |
| 9 | host | **Host_FilterTime clamp tighter**: lower bound de 0.001 a 0.0001, previene division por cero | Bajo | Facil (1 linea) |
| 10 | snd_dma | **SND_PickChannel dead code**: sfx=NULL antes de return es redundante, se sobreescribe con memset | Bajo | Facil (borrar 2 lineas) |
| 11 | cl_main | **CL_NextDemo disconnect**: llamar CL_Disconnect cuando no hay demos, evita estado corrupto | Bajo | Facil (1 linea) |

---

### Summary

| Category | Count |
|----------|-------|
| Matched pairs | 46 |
| Carviary-only | 25 |
| Ironwail-only | 42 |
| Identical (<5% diff) | 0 |
| Similar (5-30% diff) | 11 |
| Diverged (30-70% diff) | 25 |
| Very different (>70% diff) | 10 |
