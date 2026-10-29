# Balatro360

An unofficial, fan-made port of [Balatro](https://www.playbalatro.com/) to the
Xbox 360, for consoles that can run unsigned code (RGH or JTAG, or
ABadAvatar from USB).

**This repository contains no game data.** Balatro is © LocalThunk / Playstack.
To use this project you must own the game and supply your own legally obtained
copy of the PC version. The converter reads *your* `Balatro.exe` and copies
its files into the console folder on your machine; nothing from the game is
ever committed here or redistributed.

## Install

Download the latest `Balatro360-v*.zip` from
[Releases](https://github.com/dougieprishpreed/balatro360/releases), then:

1. **Extract** the whole zip to a folder on your PC.
2. **Run `Convert Balatro.bat`** and choose `Balatro.exe` from your Balatro
   installation (Steam: right-click Balatro, Manage, Browse local files). It
   copies the game's files into the `Balatro360\game` folder. It needs
   nothing installed: it uses the PowerShell that comes with Windows 10 and
   11. On a Mac or Linux, or by hand, see the zip's `README.txt`.
3. **Copy** the `Balatro360` folder to a USB drive or the console's hard disk
   and launch `default.xex` from it (Aurora, Freestyle Dash, XeXMenu, ...).
   Keep the folder together and writable: saves and the log are written next
   to the XEX.

The first start takes longer while the console compiles the game's shaders;
they're kept in `cache\` for later starts. After updating Balatro, run the
converter again and delete `cache\` on the console.

## Status

**v1.0.0: playable on real hardware.** Menus run at 60 fps; busy scenes and
most of a run drop to a steady 30 (the switch is automatic, see `B360_FPS`).
In a run Balatro's own Lua logic takes about 13 ms and its drawing about 8 ms
of CPU per frame on the 360, more than a 60 fps frame allows, and the CRT
screen effect alone takes about 10 ms of GPU time.

Seeded runs match the PC version: the random number generator is a bit-exact
reimplementation of LuaJIT's, verified against LuaJIT on x86-64 and on
big-endian PowerPC.

## How it works

Balatro is written in Lua on top of the [LÖVE](https://love2d.org) framework.
Rather than porting LÖVE itself (SDL, OpenGL, OpenAL, LuaJIT, none of which
exist on the 360), Balatro360 is a small, purpose-built runtime that implements
the parts of the LÖVE 11 API the game uses. It builds with the open
[xSDK](https://github.com/xenosworks/xsdk) toolchain and renders through
[vulkan4xenon](https://github.com/xenosworks/vulkan4xenon):

| LÖVE piece | Balatro360 replacement |
|---|---|
| LuaJIT 2.1 | LuaJIT 2.0's own Xbox 360 interpreter build, with a LuaJIT compatibility layer (`bit`, `jit`, LuaJIT's exact `math.random`); PUC Lua 5.1.5 as a fallback build |
| OpenGL + GLSL shaders | Vulkan on vulkan4xenon, drawn on a render thread of its own: the game's shaders are translated to Vulkan GLSL at load time and compiled on the console (glslang, then xost), with the results cached |
| OpenAL + libvorbis | A software mixer feeding the console's audio through xSDK; Ogg Vorbis via stb_vorbis |
| SDL game controllers | XAM's controller input, exposed through `love.joystick` with the gamepad API |
| PhysFS | `game:\game\` (read-only data) overlaid by `game:\save\` |
| FreeType | stb_truetype glyph atlases |

The same runtime also builds for Linux with an offscreen OpenGL or Vulkan
backend. That build is how the port is developed and tested without a
console: it runs the real game, scripted controller input drives it, and it
writes screenshots (see [docs/HOST_TESTING.md](docs/HOST_TESTING.md)). Xenia
doesn't run xSDK titles.

The first versions were built with Microsoft's Xbox 360 XDK and Direct3D 9;
that build is no longer maintained ([docs/BUILDING_XDK.md](docs/BUILDING_XDK.md),
and the `xdk` branch has the last of it).

## Building

GitHub Actions builds `default.xex` and the release zip on every push to
`main` ([.github/workflows/xsdk-build.yml](.github/workflows/xsdk-build.yml);
artifacts `balatro360-xex` and `balatro360-release`).

To build it yourself on Linux or in WSL: `tools/xsdk/bootstrap.sh` once, then
`tools/xsdk/build.sh` ([docs/BUILDING_XSDK.md](docs/BUILDING_XSDK.md)). Then
prepare a console folder from your own game with Python 3:

```
python tools/prepare_game.py "path\to\Balatro.exe" out\Balatro360 --xex build-xsdk/output/default.xex
```

`python tools/package_release.py <artifact> --version 1.0.0` makes the
release zip from the `balatro360-xex` artifact.

## Controls

Balatro's own controller support is used unchanged (A select, B back, etc.).

* **Hold BACK + START for one second** to quit to the dashboard from anywhere,
  including error screens.

## Configuration

Create `game\b360_config.lua` in the console folder to change settings, for
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
    B360_HALF_RES_BACKGROUND = false, -- draw the swirling backgrounds at full
                            -- resolution (they're blocks of two pixels anyway)

    -- For testing (all off unless set)
    B360_SHOW_FPS = true,   -- overlay: FPS, frame time, CPU time per frame
                            -- and the current target ([60] or [30])
    B360_LOG_PERF = true,   -- every 10 s in b360.log: frame times split into
                            -- update / draw / present, draw calls
    B360_PROFILE = true,    -- every 30 s in b360.log: the Lua functions the
                            -- game spends its time in (slightly slower)
    B360_GPU_PROFILE = true, -- every 10 s in b360.log: the GPU time of each
                            -- shader and render pass in one frame
    B360_SCREENSHOTS = true, -- hold BACK and press RB: the next frame is saved
                            -- as shots\shot_0001.png and on
    B360_INTERPOLATE = true, -- at 30 fps, show a frame halfway between each two
                            -- (experimental: 60 a second, a frame late, and
                            -- the console's GPU can't always keep up)
    B360_RENDER_THREAD = false, -- draw on the game's thread
}
```

`b360.log` also notes every switch between 60 and 30 fps (`[pace]` lines).

## Files on the console

| Path | Purpose |
|---|---|
| `default.xex` | the runtime |
| `game\` | game files from your Balatro.exe (the converter or `tools/prepare_game.py`) |
| `save\` | profiles and settings (same format as the PC version, so PC saves can be copied over) |
| `cache\` | the shaders compiled on the first start (delete it after updating the game) |
| `b360.log` | runtime log; please include it in bug reports |
| `default.log` | the SDK's own log (start-up, exit, shader compiles) |

## Repository layout

```
src/runtime/        LÖVE API implementation (platform independent C++)
src/gfx/            renderer interface, GLSL->Vulkan GLSL/HLSL translator, the
                    render thread (threaded_device), backends:
    vulkan/         Vulkan 1.0: vulkan4xenon on the 360, any driver on Linux
    gl/             Linux OpenGL (OSMesa, offscreen)
    null/           no rendering (big-endian test builds)
    d3d9/           Xbox 360 Direct3D 9 (the old XDK build)
src/platform/       OS layer: xsdk/ (360), posix/ (Linux), xbox360/ (old XDK build)
src/lua/            boot scripts embedded into the executable
third_party/        Lua 5.1.5, LuaBitOp, stb, miniz (all permissively licensed)
tools/              prepare_game.py, package_release.py; xsdk/: the xSDK build;
                    release/: the converter and the release README
build/xsdk/         CMake project for the xSDK build
build/xbox360/      Visual Studio 2010 solution for the old XDK build
docs/               build, architecture and testing notes
```

## Roadmap

* More CPU time savings in Balatro's per-frame Lua work, for 60 fps in runs.
* More GPU headroom: xost, the console's shader compiler, spends a fifth to a
  third of the CRT effect's instructions on register copies.

## Credits and licenses

Third-party code and licenses are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
Balatro and all of its assets belong to their respective owners; this project is
not affiliated with or endorsed by LocalThunk or Playstack.
