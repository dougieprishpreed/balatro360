# Balatro360

An unofficial, fan-made port of [Balatro](https://www.playbalatro.com/) to the
Xbox 360, for consoles that can run unsigned code (for example via
ABadAvatar from USB, or RGH/JTAG).

**This repository contains no game data.** Balatro is © LocalThunk / Playstack.
To use this project you must own the game and supply your own legally obtained
copy of the PC version. The tools here read *your* `Balatro.exe` and
build a console folder from it on your machine; nothing from the game is ever
committed here or redistributed.

## How it works

Balatro is written in Lua on top of the [LÖVE](https://love2d.org) framework.
Rather than porting LÖVE itself (SDL, OpenGL, OpenAL, LuaJIT, none of which
exist on the 360), Balatro360 is a small, purpose-built runtime that implements
the parts of the LÖVE 11 API the game uses, on top of the Xbox 360 XDK:

| LÖVE piece | Balatro360 replacement |
|---|---|
| LuaJIT 2.1 | Either LuaJIT's own Xbox 360 interpreter build (recommended, about 2x faster) or PUC Lua 5.1.5 with a LuaJIT compatibility layer (`bit`, `jit`, LuaJIT's exact `math.random`, `math.log(x, base)`) |
| OpenGL + GLSL shaders | Direct3D 9 on Xenos; the game's GLSL shaders are translated to HLSL at load time |
| OpenAL + libvorbis | A software mixer feeding XAudio2; Ogg Vorbis via stb_vorbis |
| SDL game controllers | XInput, exposed through `love.joystick` with the gamepad API |
| PhysFS | `game:\game\` (read-only data) overlaid by `game:\save\` |
| FreeType | stb_truetype glyph atlases |

The same runtime also builds for Linux with an offscreen OpenGL backend. That
build is how the port is developed and tested without a console: it runs the
real game, scripted controller input drives it, and it writes screenshots
(see [docs/HOST_TESTING.md](docs/HOST_TESTING.md)).

Seeded runs should match the PC version: the random number generator is a
bit-exact reimplementation of LuaJIT's, verified against LuaJIT on x86-64 and on
big-endian PowerPC.

## Status

* Runs Balatro 1.0.1o on the Linux host build through the splash screen, main
  menu, deck selection and into a run with controller input. Shaders, fonts,
  audio, threads and saves all work.
* The Xbox 360 backend (D3D9, XAudio2, XInput, file system, threads) is written
  but **has not yet been compiled or run on real hardware**. Expect a few
  rounds of fixing compiler and runtime issues. See
  [docs/BUILDING_XDK.md](docs/BUILDING_XDK.md).
* Performance is the main unknown. On the Linux build a frame of gameplay
  costs about 13 ms of CPU with Lua 5.1 and 6.5 ms with LuaJIT's interpreter
  (on a modern x86 core); the 360's in-order PowerPC cores are several times
  slower per thread, so the LuaJIT configuration will likely be needed for a
  smooth frame rate.

## Quick start

1. **Build the XEX** with Visual Studio 2010 and the Xbox 360 XDK:
   open `build/xbox360/Balatro360.sln`, pick `Release | Xbox 360`, build.
   Full instructions: [docs/BUILDING_XDK.md](docs/BUILDING_XDK.md).
2. **Prepare the game folder** from your own copy of the game (Python 3):

   ```
   python tools/prepare_game.py "path\to\Balatro.exe" out\Balatro360 --xex build\xbox360\out\Release\default.xex
   ```

3. **Copy** `out\Balatro360` to wherever you launch unsigned XEX files from
   (e.g. a USB drive) and launch `default.xex`. Keep the folder together and
   writable: saves and the log are written next to the XEX.

### Controls

Balatro's own controller support is used unchanged (A select, B back, etc.).

* **Hold BACK + START for one second** to quit to the dashboard from anywhere,
  including error screens.

### Configuration

Create `game\b360_config.lua` on the console to override the game's
feature flags, for example:

```lua
return {
    F_RUMBLE = 1.0,      -- controller rumble strength (0 to disable)
    F_HIDE_BG = false,   -- keep drawing the board while paused
}
```

### Files on the console

| Path | Purpose |
|---|---|
| `default.xex` | the runtime |
| `game\` | game files extracted from your Balatro.exe |
| `save\` | profiles and settings (same format as the PC version, so PC saves can be copied over) |
| `b360.log` | runtime log; please include it in bug reports |

## Repository layout

```
src/runtime/        LÖVE API implementation (platform independent C++)
src/gfx/            renderer interface, GLSL->HLSL translator, backends:
    d3d9/           Xbox 360 Direct3D 9
    gl/             Linux OpenGL (OSMesa, offscreen)
    null/           no rendering (big-endian test builds)
src/platform/       OS layer: xbox360/ and posix/
src/lua/            boot scripts embedded into the executable
third_party/        Lua 5.1.5, LuaBitOp, stb, miniz (all permissively licensed)
tools/              prepare_game.py, project generators
build/xbox360/      Visual Studio 2010 solution for the XDK
docs/               build, architecture and testing notes
```

## Roadmap

* First console build and fixes (D3D9/XAudio2 details, performance).
* Shader binary cache on the console to speed up start-up.
* Mipmapped textures on the 360.
* Port the platform layer to the open-source
  [xenosworks/xsdk](https://github.com/xenosworks/xsdk) once it matures, so the
  XEX can be built in CI without the Microsoft XDK.

## Credits and licenses

Third-party code and licenses are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
Balatro and all of its assets belong to their respective owners; this project is
not affiliated with or endorsed by LocalThunk or Playstack.
