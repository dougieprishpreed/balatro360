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
| LuaJIT 2.1 | Either LuaJIT 2.0's own Xbox 360 interpreter build (recommended, about 2x faster) or PUC Lua 5.1.5 with a LuaJIT compatibility layer (`bit`, `jit`, LuaJIT's exact `math.random`, `math.log(x, base)`) |
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

**Alpha.** Playable on real hardware and in Xenia with the `Release LuaJIT`
configuration.

* Menus run at a locked 60 fps (about 7 ms of CPU per frame).
* In a round Balatro's own Lua logic takes about 20 ms of CPU per frame on
  the 360, more than the 16.7 ms a 60 fps frame allows, so the runtime runs
  rounds at a locked 30 fps instead of alternating between 60 and 30 (see
  `B360_FPS` below). The time is spent in the game's code (card movement, UI
  layout and drawing), not in rendering.
* The Linux build runs the same game code with scripted controller input,
  for development and automated testing.

## Quick start

1. **Build the XEX** with Visual Studio 2010 and the Xbox 360 XDK: put
   LuaJIT 2.0 in `third_party\luajit`, run `tools\xbox360\build_luajit.bat`,
   then build `build/xbox360/Balatro360.sln` as `Release LuaJIT | Xbox 360`.
   (`Release` builds without LuaJIT, using the slower Lua 5.1.)
   Full instructions: [docs/BUILDING_XDK.md](docs/BUILDING_XDK.md).
2. **Prepare the game folder** from your own copy of the game (Python 3):

   ```
   python tools/prepare_game.py "path\to\Balatro.exe" out\Balatro360 --xex "build\xbox360\out\Release LuaJIT\default.xex"
   ```

3. **Copy** `out\Balatro360` to wherever you launch unsigned XEX files from
   (e.g. a USB drive) and launch `default.xex`. Keep the folder together and
   writable: saves and the log are written next to the XEX.

### Controls

Balatro's own controller support is used unchanged (A select, B back, etc.).

* **Hold BACK + START for one second** to quit to the dashboard from anywhere,
  including error screens.

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

* More CPU time savings, guided by `B360_PROFILE` logs from hardware.
* Shader binary cache on the console to speed up start-up.
* Mipmapped textures on the 360.
* Port the platform layer to the open-source
  [xenosworks/xsdk](https://github.com/xenosworks/xsdk) once it matures, so the
  XEX can be built in CI without the Microsoft XDK.

## Credits and licenses

Third-party code and licenses are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
Balatro and all of its assets belong to their respective owners; this project is
not affiliated with or endorsed by LocalThunk or Playstack.
