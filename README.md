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
the parts of the LÖVE 11 API the game uses. It builds with the open
[xSDK](https://github.com/xenosworks/xsdk) toolchain and renders through
[vulkan4xenon](https://github.com/xenosworks/vulkan4xenon), or, in the older
build, with Microsoft's Xbox 360 XDK and Direct3D 9:

| LÖVE piece | Balatro360 replacement |
|---|---|
| LuaJIT 2.1 | Either LuaJIT 2.0's own Xbox 360 interpreter build (recommended, about 2x faster) or PUC Lua 5.1.5 with a LuaJIT compatibility layer (`bit`, `jit`, LuaJIT's exact `math.random`, `math.log(x, base)`) |
| OpenGL + GLSL shaders | Vulkan on vulkan4xenon: the game's shaders are translated to Vulkan GLSL at load time and compiled on the console (glslang, then xost), with the results cached. The XDK build uses Direct3D 9 and HLSL |
| OpenAL + libvorbis | A software mixer feeding the console's audio (xSDK's feeder, or XAudio2); Ogg Vorbis via stb_vorbis |
| SDL game controllers | XAM's controller input (XInput with the XDK), exposed through `love.joystick` with the gamepad API |
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

**Alpha.** The XDK build (`Release LuaJIT`) is playable on real hardware and
in Xenia. The xSDK build on the `xsdk` branch, which is to replace it, builds
and runs the game on the PC through the same Vulkan renderer; it has not run
on a console yet. Xenia doesn't run xSDK titles.

* Menus run at a locked 60 fps (about 7 ms of CPU per frame).
* In a round Balatro's own Lua logic takes about 20 ms of CPU per frame on
  the 360, more than the 16.7 ms a 60 fps frame allows, so the runtime runs
  rounds at a locked 30 fps instead of alternating between 60 and 30 (see
  `B360_FPS` below). The time is spent in the game's code (card movement, UI
  layout and drawing), not in rendering.
* The Linux build runs the same game code with scripted controller input,
  for development and automated testing.

## Quick start

1. **Clone** this repository (`git clone https://github.com/dougieprishpreed/balatro360.git`).
2. **Build the XEX** on Linux or in WSL with the open xSDK:
   `tools/xsdk/bootstrap.sh` once, then `tools/xsdk/build.sh`
   ([docs/BUILDING_XSDK.md](docs/BUILDING_XSDK.md)). The older Microsoft
   XDK build is in [docs/BUILDING_XDK.md](docs/BUILDING_XDK.md).
3. **Prepare the game folder** from your own copy of the game (Python 3):

   ```
   python tools/prepare_game.py "path\to\Balatro.exe" out\Balatro360 --xex "build\xbox360\out\Release LuaJIT\default.xex"
   ```

4. **Copy** `out\Balatro360` to wherever you launch unsigned XEX files from
   (e.g. a USB drive) and launch `default.xex`. Keep the folder together and
   writable: saves and the log are written next to the XEX.

### Controls

Balatro's own controller support is used unchanged (A select, B back, etc.).

* **Hold BACK + START for one second** to quit to the dashboard from anywhere,
  including error screens.
* **Hold BACK and press RB** for a screenshot (xSDK build): the next frame
  is saved as `shots\shot_0001.png` and on.

### Configuration

Create `game\b360_config.lua` on the console to change settings, for
example:

```lua
return {
    -- Balatro's own feature flags (F_*)
    F_RUMBLE = 1.0,         -- controller rumble strength (0 to disable)
    F_HIDE_BG = false,      -- keep drawing the board while paused

    -- Balatro360 options (B360_*)
    B360_FPS = "auto",      -- 60, 30 or "auto" (the default): 60 while the game
                            -- keeps up, otherwise a steady 30
    B360_SAFE_AREA = 0.92,  -- shrink the picture if your TV crops the edges
    B360_SHOW_FPS = true,   -- overlay: FPS, frame time, CPU time per frame
                            -- and the current target ([60] or [30])
    B360_LOG_PERF = true,   -- every 10 s in b360.log: frame times split into
                            -- update / draw / present, draw calls
    B360_PROFILE = true,    -- every 30 s in b360.log: the Lua functions the
                            -- game spends its time in (slightly slower)
    B360_GPU_PROFILE = true, -- xSDK build, every 10 s in b360.log: the GPU
                            -- time of each shader and render pass in one frame
}
```

`b360.log` also notes every switch between 60 and 30 fps (`[pace]` lines).

### Files on the console

| Path | Purpose |
|---|---|
| `default.xex` | the runtime |
| `game\` | game files extracted from your Balatro.exe |
| `save\` | profiles and settings (same format as the PC version, so PC saves can be copied over) |
| `b360.log` | runtime log; please include it in bug reports |
| `cache\` | xSDK build: the shaders compiled on the first start (delete it after updating the game) |
| `default.log` | xSDK build: the SDK's own log (start-up, exit, shader compiles) |
| `shots\` | xSDK build: screenshots (BACK + RB) |

## Repository layout

```
src/runtime/        LÖVE API implementation (platform independent C++)
src/gfx/            renderer interface, GLSL->HLSL/Vulkan GLSL translator, backends:
    vulkan/         Vulkan 1.0: vulkan4xenon on the 360 (xSDK), any driver on Linux
    d3d9/           Xbox 360 Direct3D 9 (XDK)
    gl/             Linux OpenGL (OSMesa, offscreen)
    null/           no rendering (big-endian test builds)
src/platform/       OS layer: xsdk/ (360, xSDK), xbox360/ (360, XDK) and posix/
src/lua/            boot scripts embedded into the executable
third_party/        Lua 5.1.5, LuaBitOp, stb, miniz (all permissively licensed)
tools/              prepare_game.py, project generators; xsdk/: the xSDK build
build/xsdk/         CMake project for the xSDK build
build/xbox360/      Visual Studio 2010 solution for the XDK
docs/               build, architecture and testing notes
```

## Roadmap

* More CPU time savings, guided by `B360_PROFILE` logs from hardware.
* Make the xSDK build the main one, built by GitHub Actions, and retire the
  XDK build.

Done in the xSDK build: the port to xSDK and vulkan4xenon, a shader binary
cache on the console (`cache\`), and mipmapped textures.

## Credits and licenses

Third-party code and licenses are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
Balatro and all of its assets belong to their respective owners; this project is
not affiliated with or endorsed by LocalThunk or Playstack.
