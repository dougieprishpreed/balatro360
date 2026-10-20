# Building Balatro360 with the Xbox 360 XDK

This is the only step that has to happen on your Windows machine; everything
else (runtime development, testing against the real game, shader translation)
is done with the Linux host build.

## What you need

* Windows with **Visual Studio 2010** (Professional or Ultimate; Express can't
  load platform add-ins). Service Pack 1 is recommended.
* The **Xbox 360 XDK** with its Visual Studio 2010 integration, installed
  *after* Visual Studio (the 2.0.2xxxx-series XDKs). After installing, the
  `XEDK` environment variable points at it and Visual Studio shows an
  **Xbox 360** platform.
* **Python 3** (any recent version) for `tools/prepare_game.py`.
* Your own legally obtained copy of the PC version of Balatro (`Balatro.exe`).
* A console that can run unsigned XEX files. On a stock console that is
  usually an exploit run from USB such as ABadAvatar; RGH/JTAG consoles and
  dev kits work too.

## 1. Get the source

Clone the repository (or download a zip of the branch), for example to
`F:\downloads\temp\python\hermes\balatro360`.

```
git clone https://github.com/dougieprishpreed/balatro360.git F:\downloads\temp\python\hermes\balatro360
cd F:\downloads\temp\python\hermes\balatro360
git checkout claude/exciting-feynman-7xcs5u
```

## 2. Build the XEX

1. Open `build\xbox360\Balatro360.sln` in Visual Studio 2010.
2. In the toolbar pick the configuration **Release** and platform **Xbox 360**.
   (Debug also works; it is much slower but easier to debug on a dev kit.
   The **LuaJIT** configurations are described below.)
3. **Build → Build Solution** (F7).

The result is `build\xbox360\out\Release\default.xex`.

From a command prompt you can do the same with:

```
msbuild build\xbox360\Balatro360.sln /p:Configuration=Release /p:Platform="Xbox 360" /m > build_log.txt
```

Either the *Visual Studio Command Prompt (2010)* or the XDK's *Xbox 360
Command Prompt* works for this: the project's Xbox 360 platform settings
find the XDK compiler through the `XEDK` environment variable either way.
(The LuaJIT step below is different and needs the Visual Studio prompt.)

### Faster Lua: the LuaJIT configurations (recommended once the plain build works)

By default the runtime uses the reference Lua 5.1 interpreter, which is
portable but slow. LuaJIT ships an official Xbox 360 build script (its
interpreter, written in PowerPC assembly; no JIT on the console). On the
Linux test build, LuaJIT's interpreter runs Balatro about **2x faster** than
Lua 5.1, which matters a lot on the 360's CPU.

1. Clone LuaJIT 2.1 into `third_party\luajit` (this folder is git-ignored):

   ```
   git clone -b v2.1 https://github.com/LuaJIT/LuaJIT.git third_party\luajit
   ```

2. Open **Visual Studio Command Prompt (2010)** (the 32-bit one; the XDK must
   be installed so `XEDK` is set) and run LuaJIT's Xbox 360 build script.
   Use this prompt rather than the Xbox 360 Command Prompt: the script first
   builds small helper tools for your PC with the plain `cl` on `PATH`, and
   only then calls the XDK's cross compiler itself through `%XEDK%`. If `cl`
   on `PATH` is the XDK's PowerPC compiler, those helper tools can't run.

   ```
   cd third_party\luajit\src
   xedkbuild
   ```

   It should end with `=== Successfully built LuaJIT for Xbox 360 ===` and
   leave `luajit20.lib` in `third_party\luajit\src`.

3. In Visual Studio pick the **Release LuaJIT** configuration and build. The
   XEX lands in `build\xbox360\out\Release LuaJIT\default.xex`.

(`Debug LuaJIT` needs a debug LuaJIT: run `xedkbuild debug` instead, or just
use `Release LuaJIT`.)

## 3. Build the console folder

```
python tools\prepare_game.py "F:\Program Files (x86)\Balatro.v1.0.1n\Balatro.exe" out\Balatro360 --xex build\xbox360\out\Release\default.xex
```

This extracts the game's files from *your* `Balatro.exe` into
`out\Balatro360\game\` and copies the XEX next to it:

```
out\Balatro360\
    default.xex
    game\
        main.lua, conf.lua, engine\, functions\, localization\, resources\, ...
```

You only need to re-run this when the game updates. After a rebuild of the
runtime just copy the new `default.xex`.

## 4. Copy to the console and run

Copy the whole `Balatro360` folder wherever your setup launches unsigned XEX
files from (for example a USB drive used with ABadAvatar, or the console's
hard drive via FTP), then launch `default.xex` with your launcher. The folder
must stay together: the runtime reads `game\` and writes `save\` and
`b360.log` next to `default.xex`, so that location must be writable (a USB
drive or hard drive is; a disc is not). On a dev kit you can also deploy from
Visual Studio.

On first launch the runtime creates `save\` next to `default.xex` and writes a
log to `b360.log`. Startup takes a few seconds longer than later loads because
all shaders are compiled on the console.

Hold **BACK + START** for one second to quit to the dashboard.

## 5. Reporting results back

The port hasn't been built against the real XDK yet, so the first builds will
probably need some fixes. The quickest loop is:

1. If the build fails, save the full build output (the `msbuild ... >
   build_log.txt` command above, or copy everything from Visual Studio's
   **Output** window).
2. If it builds but misbehaves on the console, grab `b360.log` from the game
   folder (FTP) and, if possible, a photo of the screen.
3. Commit them to the repository under `reports/` (for example
   `reports/build_log.txt`, `reports/b360.log`) and push, or paste them into
   the chat. Those files are plain text and contain no game data.

## Troubleshooting

**"The project's platform 'Xbox 360' is not installed"**: the XDK's Visual
Studio integration isn't installed, or was installed before Visual Studio.
Re-run the XDK installer.

**Unresolved externals / missing `.lib` files**: the linker inputs are the ones
the XDK samples use (`xapilib`, `d3d9`, `d3dx9`, `xgraphics`, `xboxkrnl`,
`xnet`, `xaudio2`, `x3daudio`, `xmcore`, `vcomp`, plus `d`-suffixed debug
variants and `xbdm` for Debug). Library names vary slightly between XDK
versions. Open any XDK sample's project settings (**Linker → Input**) and use
the same list; it lives in `tools/gen_vcxproj.py` (`LIBS_DEBUG` /
`LIBS_RELEASE`). Edit it there and run `python tools/gen_vcxproj.py`, or edit
`Balatro360.vcxproj` directly.

**`D3DRS_SEPARATEALPHABLENDENABLE` is undeclared**: add
`B360_HAS_SEPARATEALPHABLENDENABLE=0` to **C/C++ → Preprocessor →
Preprocessor Definitions**.

**"cannot instantiate abstract class" for `VoiceCallback`**: the XAudio2
callback signatures differ in your XDK. Compare the `IXAudio2VoiceCallback`
declaration in `%XEDK%\include\xbox\xaudio2.h` with
`src/platform/xbox360/platform_x360.cpp` and adjust (usually
`OnVoiceProcessingPassStart` with or without a `UINT32` parameter).

**Black screen, but `b360.log` shows the game running**: please send the log.
It records shader compile errors (`[d3d] shader compile failed`) with the
generated HLSL error messages.

**"Could not open file main.lua"**: the `game\` folder is missing or is not
next to `default.xex`. The runtime reads from `game:\game\`, where `game:` is
the folder the XEX was launched from.

## Regenerating project files

`build/xbox360/*.vcxproj*` and `src/runtime/embedded_lua.h` are generated and
committed so you don't need Python to build:

```
python tools\gen_vcxproj.py      # after adding/removing source files
python tools\embed_lua.py        # after editing src\lua\*.lua
```
