# Building Balatro360 with the open xSDK

The xSDK build makes `default.xex` with open tools only: the
[xenosworks/xsdk](https://github.com/xenosworks/xsdk) toolchain (GCC 15,
binutils, newlib) and its GPU layer, with rendering through
[vulkan4xenon](https://github.com/xenosworks/vulkan4xenon), a Vulkan 1.0
subset for Xenos. No Microsoft XDK, Visual Studio or Windows build is
involved, so GitHub Actions builds it too
(`.github/workflows/xsdk-build.yml`, artifact `balatro360-xex`).

Like every xSDK title it runs on consoles that run unsigned code (RGH/JTAG,
or an exploit such as ABadAvatar). **Xenia doesn't run xSDK titles**; test on
the console.

## What you need

* Linux, or WSL 2 on Windows with Debian (13 "trixie" is what the build is
  tested on).
* About 5 GB of disk and, the first time, about 30 minutes to build GCC.
* Python 3 and your own copy of Balatro for `tools/prepare_game.py`, as in
  [BUILDING_XDK.md](BUILDING_XDK.md), section 3.

Packages (Debian):

```
sudo apt install build-essential gcc-multilib bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev \
    curl xz-utils patch python3 cmake ninja-build git ca-certificates
```

`gcc-multilib` is for LuaJIT's code generator, which must be built for a
32-bit host to write the console's 32-bit interpreter.

## 1. The toolchain and the pinned dependencies

From the repository:

```
JOBS=$(nproc) bash tools/xsdk/bootstrap.sh
```

This clones the pinned revisions of xsdk, vulkan4xenon, glslang-xenon and
LuaJIT 2.0 into `build-xsdk/deps`, builds the cross compiler into
`build-xsdk/toolchain` (set `XSDK_TOOLCHAIN` to put it elsewhere), and a
host glslang for checking shaders. At the end it prints the paths the build
takes. The pins are in `bootstrap.sh`; `tools/xsdk/verify_deps.py` stops a
build from checkouts at other revisions (`B360_ANY_DEPS=1` lets one through,
for trying newer heads).

## 2. Build the XEX

```
export XSDK=$PWD/build-xsdk/deps/xsdk VULKAN4XENON=$PWD/build-xsdk/deps/vulkan4xenon
export GLSLANG_XENON=$PWD/build-xsdk/deps/glslang-xenon LUAJIT=$PWD/build-xsdk/deps/luajit
export XSDK_TOOLCHAIN=$PWD/build-xsdk/toolchain
JOBS=$(nproc) bash tools/xsdk/build.sh
```

The result is `build-xsdk/output/default.xex`, with the licences it ships
under in `build-xsdk/output/licenses/`. `B360_XSDK_BUILD=<folder>` puts the
output elsewhere. `B360_LUA=puc` builds with PUC Lua 5.1 instead of LuaJIT
(about half the speed; useful to tell a LuaJIT problem from anything else).

What the build does:

* `tools/xsdk/build_luajit.sh` builds LuaJIT 2.0's PowerPC interpreter the
  way LuaJIT's own Xbox 360 build (`src/xedkbuild.bat`) does, the same VM
  and defines, assembled as ELF: xSDK's GCC uses the console's calling
  convention (`-mcall-xsdk`), the one that VM was written for. Lua errors
  unwind with LuaJIT's own unwinder, as with the XDK, so the runtime still
  never holds a lock or anything with a destructor across a call that can
  raise one.
* `build/xsdk/CMakeLists.txt` compiles the runtime (C++98, `-fsigned-char`,
  `-fno-strict-aliasing`, `-ffp-contract=off` so seeds match the PC),
  glslang-xenon for the console, and links vulkan4xenon and xSDK; xSDK's
  `xsdk_add_xex` packs the ELF.

## 3. The console folder

Prepare the game folder once, as for the XDK build, and put the new XEX in
it:

```
python tools\prepare_game.py "path\to\Balatro.exe" out\Balatro360 --xex <the default.xex you built>
```

Copy `out\Balatro360` to the console (USB or hard disk) and launch
`default.xex`. What the xSDK build adds next to it:

| Path | Purpose |
|---|---|
| `cache\spirv\` | the game's shaders as SPIR-V, so later starts skip glslang (about a second) |
| `cache\xenos\` | the same shaders compiled for Xenos by vulkan4xenon, so later starts skip that too |
| `default.log` | xSDK's own log (start-up, exit, vulkan4xenon's messages, shader compile times) |

`b360.log` is the runtime's log, as before. The first start compiles all
shaders on the console and is slower; delete `cache\` after updating the
game. Hold **BACK + START** for a second to quit to the dashboard.

## Checking on the PC

There's no emulator, so two host checks stand in before a console run:

* **Shaders against the console's compiler.** Every LÖVE shader goes
  through the runtime's translator, glslang and xost (vulkan4xenon's
  compiler, the same code the console runs) with the console's register
  budget. With the host shader tool (`make out/host/b360-shadertool`, see
  [HOST_TESTING.md](HOST_TESTING.md)):

  ```
  python3 tools/xsdk/check_shaders.py --xsdk $XSDK --glslang build-xsdk/deps/glslang-host/StandAlone/glslang \
      --tool out/host/b360-shadertool --empty out/Balatro360/game/resources/shaders/*.fs
  ```

  `make -C $XSDK build/host/libxsc.so` builds the xost library it loads.
* **The Vulkan renderer itself.** `make GFX=vulkan` builds the host runtime
  with the same backend, drawing offscreen on any Vulkan driver (Mesa's
  lavapipe: `apt install mesa-vulkan-drivers libvulkan-dev
  vulkan-validationlayers`); `B360_VK_VALIDATE=1` turns on Khronos's
  validation layer. Its screenshots can be compared with the OpenGL
  build's.

## Notes

* Frame pacing: vulkan4xenon's swapchain shows a frame per vblank; the 30
  fps mode queues frames two vblanks apart.
* Audio goes through xSDK's feeder for the kernel's render driver; the mix
  is stereo in its front channels.
* The Microsoft XDK build ([BUILDING_XDK.md](BUILDING_XDK.md)) still builds
  from the same sources until the xSDK build replaces it.
