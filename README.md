# Carviary

A Quake engine based on [TomazQuake](https://web.archive.org/web/2005/http://tomazquake.planetquake.gamespy.com/), ported from C to C++ and compiled with modern MSVC via CMake.

Carviary builds on TomazQuake's feature set (scripted shaders, mirrors, flares, crosshair system, MD2 support, particle engine, etc.) while modernizing the build system and incrementally improving the codebase with fixes backported from [Ironwail](https://github.com/andrei-drexler/ironwail).

## Features

- OpenGL renderer with TomazQuake extensions (shader scripts, mirrors, flares, skyboxes)
- MD2 and MDL model support
- Particle system and dynamic lighting
- Built-in crosshair and HUD customization
- Win32 native (DirectSound, WinSock, DirectInput)

## Requirements

- **Windows** (Win32 target)
- **Visual Studio** with MSVC C++ compiler
- **CMake** 3.10 or newer

## Building

```bash
cmake -S src_cpp -B build -A Win32
cmake --build build --config Release
```

Or via npm (requires Node.js):

```bash
npm run compile
```

The compiled `carviary.exe` is output to the Quake game directory (`d:/quake/` by default — change `RUNTIME_OUTPUT_DIRECTORY` in `src_cpp/CMakeLists.txt` to match your setup).

## Running

Place `carviary.exe` in your Quake directory alongside the `id1/` folder containing the game data (PAK files), then run it:

```bash
cd /path/to/quake
./carviary.exe
```

Or via npm:

```bash
npm start
```

## Project Structure

```
src_cpp/          Source code (.cpp files + headers/)
src_cpp/headers/  Header files
scripts/          Dev tools (profiling, debugging)
data/             Generated analysis output (not tracked)
build/            CMake build output (not tracked)
```

## License

GPL-2.0 — see [gnu.txt](gnu.txt). Based on the Quake engine source code released by id Software.
