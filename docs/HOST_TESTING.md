# Testing on Linux (host build)

The runtime builds for Linux with an offscreen OpenGL backend (Mesa OSMesa),
so the real game can be run, driven and screenshotted without a console or a
display. This is how most of the port is developed.

## Build

```
sudo apt install build-essential libosmesa6-dev python3
make -j            # out/host/b360 and out/host/b360-shadertool
make GFX=null      # out/host-null/b360 (no rendering)
make GFX=vulkan GLSLANG_SRC=... GLSLANG_BUILD=...
                   # out/host-vulkan/b360: the console's Vulkan renderer,
                   # offscreen (below)
make ppc           # out/ppc/b360: big-endian PowerPC, run with qemu-ppc
make LUA=luajit    # out/host-luajit/b360: system LuaJIT (libluajit-5.1-dev),
                   # JIT disabled like on the 360 (B360_JIT=1 to enable)
make ppc-luajit LJ=/path/to/LuaJIT   # big-endian PowerPC + LuaJIT interpreter,
                   # the closest match to "Release LuaJIT" (see Makefile for
                   # how to cross-build LuaJIT; use -ffp-contract=off in
                   # XCFLAGS, like /fp:strict on the 360)
```

`tools/test_rng.sh [binary] [runner]` checks that `math.random` & co. match
LuaJIT exactly (`tools/test_rng.sh out/ppc/b360 qemu-ppc` for big-endian).

The `ppc` build needs `g++-powerpc-linux-gnu` and `qemu-user`. It runs the
whole game on a big-endian CPU like the 360's, which catches byte-order bugs
in decoding, saving and the Lua VM.

The code is compiled with `-std=c++98` so anything that builds here should
also be accepted by the Visual Studio 2010-era XDK compiler.

### The Vulkan renderer

`GFX=vulkan` builds the xSDK build's renderer (`src/gfx/vulkan`) for the
host. It needs a Vulkan driver (`apt install libvulkan-dev
mesa-vulkan-drivers`; lavapipe draws on the CPU) and glslang-xenon built for
the host: `GLSLANG_SRC` is its checkout and `GLSLANG_BUILD` its CMake build
folder (`tools/xsdk/bootstrap.sh` makes both under `build-xsdk/deps`, the
defaults). `B360_VK_VALIDATE=1` turns on Khronos's validation layer
(`vulkan-validationlayers`). Its screenshots should match the OpenGL
build's, which checks the backend, the Vulkan GLSL the translator writes and
the mipmaps; `tools/xsdk/check_shaders.py` checks that xost, the console's
shader compiler, takes the same shaders.

## Prepare game data

```
python3 tools/prepare_game.py /path/to/Balatro.exe /tmp/b360
```

## Run

```
B360_GAME_DIR=/tmp/b360/game \
B360_SAVE_DIR=/tmp/b360/save \
B360_SCRIPT=tests/menu.txt \
B360_FIXED_DT=0.016666 \
B360_MAX_FRAMES=1900 \
out/host/b360
```

| Variable | Meaning |
|---|---|
| `B360_GAME_DIR` | extracted game folder |
| `B360_SAVE_DIR` | save folder (created if missing) |
| `B360_SCRIPT` | input script (below) |
| `B360_FIXED_DT` | fixed frame time in seconds; makes runs deterministic and independent of host speed |
| `B360_MAX_FRAMES` | quit after this many frames |
| `B360_WAV` | write the mixed audio to a WAV file |
| `B360_FIXED_TIME` | pin `os.time()` (Balatro seeds runs from it) so runs are identical frame for frame |
| `B360_NULL_VSYNC` | `1`: the null renderer behaves like a 60 Hz display (for frame pacing tests) |

A `b360_config.lua` in the save folder works like on the console, e.g.
`return { B360_LOG_PERF = 3, B360_PROFILE = 10 }` (numbers are the report
intervals in seconds). Host runs are unpaced unless `B360_FPS` is set.

`tests/pacing` checks the 60/30 switching against the emulated display:

```
B360_NULL_VSYNC=1 B360_GAME_DIR=tests/pacing B360_SAVE_DIR=/tmp/p out/host-null/b360
```

## Input scripts

One command per line, `<frame> <command> [args]`, `#` starts a comment:

```
1250 tap a                 # press and release (4 frames later) a gamepad button
1300 press dpleft          # hold...
1320 release dpleft        # ...and release
1400 axis leftx 0.8        # set a stick/trigger axis
1500 screenshot shot.png   # write a PNG of the frame being presented
1600 log reached shop      # print a marker in the log
1900 quit
```

Button names are LÖVE's gamepad names (`a b x y back start leftshoulder
rightshoulder leftstick rightstick dpup dpdown dpleft dpright`); anything else
is sent as a keyboard key. `tests/menu.txt` boots to the main menu and starts
a run.

## Shader validation

`b360-shadertool` prints what the translator generates:

```
out/host/b360-shadertool hlsl-ps /tmp/b360/game/resources/shaders/holo.fs
out/host/b360-shadertool hlsl-vs ...   # vertex stage
out/host/b360-shadertool glsl-ps ...   # what the OpenGL backend compiles
out/host/b360-shadertool uniforms ...
```

`tools/check_hlsl.sh` compiles every game shader with vkd3d's HLSL compiler
(`vkd3d-compiler`, 1.14 or newer) for `vs_3_0` and `ps_3_0`.
