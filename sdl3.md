# Port de Carviary a SDL3

Crear una copia de `src_cpp/` en `src_sdl3/` con los subsistemas Win32 reemplazados por SDL3, manteniendo todo el rendering OpenGL y la logica de juego intactos.

## Ficheros a reemplazar

| Fichero Win32 | Fichero SDL3 | Subsistema |
|---|---|---|
| `sys_win.cpp` | `sys_sdl.cpp` | Entry point, main loop, timers, memoria |
| `gl_vidnt.cpp` | `gl_vid_sdl.cpp` | Ventana, contexto GL, modos de video, vsync |
| `in_win.cpp` | `in_sdl.cpp` | Raton, teclado, joystick/gamepad |
| `snd_win.cpp` | `snd_sdl.cpp` | Audio output (DirectSound -> SDL audio streams) |
| `cd_win.cpp` | `cd_sdl.cpp` | CD audio (stub o musica OGG via SDL_mixer) |
| `net_wins.cpp` | `net_bsd.cpp` | Sockets TCP/IP (Winsock -> BSD sockets) |
| `net_wipx.cpp` | _(eliminar)_ | IPX obsoleto |
| `conproc.cpp` | _(eliminar)_ | IPC dedicado Win32, usar stdin/stdout |
| `winquake.rc` | _(eliminar)_ | Recursos Win32 (icono, splash embebido) |

## Ficheros con cambios menores

| Fichero | Cambio necesario |
|---|---|
| `keys.cpp` | `OpenClipboard`/`GetClipboardData` -> `SDL_GetClipboardText` |
| `menu_multi.cpp` | `FindFirstFile`/`FindNextFile` -> `opendir`/`readdir` o `std::filesystem` |
| `host.cpp` | `QueryPerformanceCounter` (4 lineas) -> `SDL_GetPerformanceCounter` |
| `snd_dma.cpp` | ~15 lineas de DirectSound buffer -> interfaz SNDDMA abstracta |
| `snd_mix.cpp` | Quitar typedef `DWORD`, usar `unsigned long` directo |
| `gl_proc.cpp` | Quitar `#ifdef _WIN32` / `#include <windows.h>`, solo necesita `<GL/gl.h>` |
| `menu.cpp`, `menu_*.cpp` | Quitar `#include "winquake.h"` (no usan nada Win32) |

## Header principal

Reemplazar `headers/winquake.h` por `headers/sdlquake.h`:
- Incluir `<SDL3/SDL.h>` en vez de `<windows.h>`, `<ddraw.h>`, `<dsound.h>`
- Exportar `SDL_Window *sdl_window`, `SDL_GLContext sdl_glcontext` en vez de `HWND mainwindow`, `HGLRC baseRC`
- Eliminar todos los extern de DirectSound/DirectInput

## Ficheros portables (no tocar)

Todo el rendering OpenGL y la logica de juego ya son portables:
- **Rendering GL:** `gl_crosshair`, `gl_dof`, `gl_draw`, `gl_flares`, `gl_font`, `gl_hc_texes`, `gl_md2`, `gl_mdl`, `gl_mirror`, `gl_model`, `gl_part`, `gl_refrag`, `gl_rlight`, `gl_rmain`, `gl_rmisc`, `gl_rscript`, `gl_rsurf`, `gl_screen`, `gl_sky`, `gl_sprite`, `gl_texman`, `gl_warp`
- **Game logic:** `chase`, `cl_demo`, `cl_input`, `cl_main`, `cl_parse`, `cl_tent`, `cmd`, `common`, `console`, `crc`, `cvar`, `host_cmd`, `mathlib`, `pr_cmds`, `pr_edict`, `pr_exec`, `pr_native`, `sbar`, `view`, `wad`, `world`, `zone`
- **Server:** `sv_main`, `sv_move`, `sv_phys`, `sv_user`
- **Network (portable):** `net_dgrm`, `net_loop`, `net_main`

## Mapeado de APIs

| Win32 | SDL3 |
|---|---|
| `WinMain` | `SDL_main` / `main` estandar |
| `CreateWindowEx` | `SDL_CreateWindow(SDL_WINDOW_OPENGL)` |
| `wglCreateContext` | `SDL_GL_CreateContext` |
| `wglMakeCurrent` | `SDL_GL_MakeCurrent` |
| `SwapBuffers` | `SDL_GL_SwapWindow` |
| `wglGetProcAddress` | `SDL_GL_GetProcAddress` |
| `wglSwapIntervalEXT` | `SDL_GL_SetSwapInterval` |
| `ChangeDisplaySettings` | `SDL_SetWindowFullscreen` |
| `EnumDisplaySettings` | `SDL_GetDisplayMode` |
| `QueryPerformanceCounter` | `SDL_GetPerformanceCounter` |
| `QueryPerformanceFrequency` | `SDL_GetPerformanceFrequency` |
| `Sleep` | `SDL_Delay` |
| `PeekMessage`/`DispatchMessage` | `SDL_PollEvent` |
| DirectInput mouse | `SDL_SetRelativeMouseMode` + `SDL_EVENT_MOUSE_MOTION` |
| `joyGetPosEx` | `SDL_OpenGamepad` / `SDL_GetGamepadAxis` |
| DirectSound | `SDL_OpenAudioDevice` + audio stream callback |
| `mciSendCommand` (CD) | Stub o `SDL_mixer` para musica |
| Winsock `socket`/`sendto` | BSD sockets directos (ya portables en Linux/Mac) |
| `OpenClipboard` | `SDL_GetClipboardText` |
| `FindFirstFile` | `opendir`/`readdir` |

## CMakeLists.txt para SDL3

```cmake
cmake_minimum_required(VERSION 3.10)
project(carviary CXX)

find_package(SDL3 REQUIRED)

file(GLOB SOURCES ${CMAKE_SOURCE_DIR}/*.cpp)
# Excluir los ficheros Win32 que ya no existen en src_sdl3
list(FILTER SOURCES EXCLUDE REGEX "sys_win|gl_vidnt|in_win|snd_win|cd_win|net_wins|net_wipx|conproc")

add_executable(carviary ${SOURCES})

target_include_directories(carviary PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/headers)
target_link_libraries(carviary PRIVATE SDL3::SDL3 OpenGL::GL)

set_target_properties(carviary PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../../"
    OUTPUT_NAME "carviary"
)
```

## Orden de trabajo sugerido

1. Copiar `src_cpp/` a `src_sdl3/`, eliminar ficheros Win32 listados arriba
2. Crear `headers/sdlquake.h` reemplazando `winquake.h`
3. `sys_sdl.cpp` - entry point + main loop + timers (el engine arranca)
4. `gl_vid_sdl.cpp` - ventana + contexto GL + swap (se ve algo en pantalla)
5. `in_sdl.cpp` - input raton/teclado (se puede jugar)
6. `snd_sdl.cpp` - audio (se oye)
7. `cd_sdl.cpp` - stub de musica
8. `net_bsd.cpp` - sockets BSD (multijugador)
9. Ajustes menores en ficheros mixtos (keys, menus, host, snd_dma)
10. Probar en Windows
