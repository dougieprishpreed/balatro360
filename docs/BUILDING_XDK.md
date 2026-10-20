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

Clone the repository with git (Git for Windows):

```
git clone https://github.com/dougieprishpreed/balatro360.git
```

To update later, run `git pull` in the clone and build again; a normal
**Build** is enough (git only touches files that changed). Your own files
stay put: `out\` (the prepared console folder), `third_party\luajit\`,
build output and logs are all ignored by git.

The project records the commit it was built from (and a `+` if you had
local changes) on the first line of `b360.log`
(`source <commit> <date>`), so logs show exactly which version ran.

A **Code → Download ZIP** of `main` also works; copy the zip's
`balatro360-main` folder over your project folder and **Rebuild**.

## 2. Build the XEX

1. Open `build\xbox360\Balatro360.sln` in Visual Studio 2010.
2. In the toolbar pick the configuration **Release** and platform **Xbox 360**.
   (Debug also works; it is much slower but easier to debug on a dev kit.
   The **LuaJIT** configurations are described below.)
3. **Build → Build Solution** (**Rebuild Solution** after switching
   configurations or if something looks stale).

The result is `build\xbox360\out\Release\default.xex`.

From a command prompt you can do the same with:

```
msbuild build\xbox360\Balatro360.sln /t:Build /p:Configuration=Release /p:Platform="Xbox 360" /m > build_log.txt
```

Either the *Visual Studio Command Prompt (2010)* or the XDK's *Xbox 360
Command Prompt* works for this: the project's Xbox 360 platform settings
find the XDK compiler through the `XEDK` environment variable either way.
(The LuaJIT step below is different and needs the Visual Studio prompt.)

### LuaJIT (the configuration to play with)

`Release` uses the reference Lua 5.1 interpreter: portable, but slow. The
**Release LuaJIT** configuration uses LuaJIT's own Xbox 360 port (its
interpreter, written in PowerPC assembly; there is no JIT on the console),
which runs Balatro about **2x faster**. LuaJIT is also what the PC version
runs on, so game behaviour matches PC exactly. One-time setup:

1. Download **LuaJIT 2.0**:
   <https://github.com/LuaJIT/LuaJIT/archive/refs/heads/v2.0.zip>, extract it,
   and rename the extracted `LuaJIT-2.0` folder so that it is
   `third_party\luajit` inside the project (so `third_party\luajit\src\lj_vm.h`
   exists). If you previously put LuaJIT 2.1 there, delete that folder first.
   It must be 2.0: LuaJIT 2.1 removed the PowerPC code its Xbox 360 build
   needs, so 2.1's `xedkbuild.bat` fails. 2.0 is the maintained stable branch
   and runs Balatro the same way. git ignores this folder, so updates never
   touch it.

2. Open **Visual Studio Command Prompt (2010)** (the 32-bit one, with the XDK
   installed so `XEDK` is set), go to the project folder and run:

   ```
   tools\xbox360\build_luajit.bat
   ```

   It should end with `=== Successfully built LuaJIT for Xbox 360 ===` and
   leave `luajit20.lib` in `third_party\luajit\src`. Attach the output here if
   it doesn't.

   This is LuaJIT 2.0's own `xedkbuild.bat` with `/fp:strict` added (so the
   compiler can't fuse multiply-adds; otherwise some seeds give different
   random numbers than on PC) and `LUA_BUILD_AS_DLL` removed (static link). Use the Visual Studio prompt, not
   the Xbox 360 Command Prompt: the script first builds small helper tools
   for your PC with the plain `cl` on `PATH`, and only then calls the XDK's
   cross compiler through `%XEDK%`.

3. Build the **Release LuaJIT** configuration:

   ```
   msbuild build\xbox360\Balatro360.sln /t:Build /p:Configuration="Release LuaJIT" /p:Platform="Xbox 360" /m > build_log.txt
   ```

   The XEX lands in `build\xbox360\out\Release LuaJIT\default.xex`. Pass that
   path to `prepare_game.py --xex` below.

You only redo step 2 if LuaJIT itself is updated. (`Debug LuaJIT` needs a
debug LuaJIT: `tools\xbox360\build_luajit.bat debug`.)

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

### Testing in Xenia

Xenia runs `default.xex` directly; keep the `game\` folder next to it.

* The runtime's log goes to Xenia's own log (`xenia.log`) as well as to
  `b360.log`.
* Xenia normally doesn't let a game write next to its XEX, so saves and
  `b360.log` may be missing. In Xenia Canary, setting
  `allow_game_relative_writes = true` in `xenia-canary.config.toml` should
  allow it.
* Lines like `Unimplemented instruction: InstrEmit_addx` / `mullwx` /
  `subfx` in `xenia.log` are expected: LuaJIT's PowerPC interpreter uses
  the overflow-checking forms of those instructions, which Xenia's
  recompiler doesn't implement yet. Real hardware has them.
* A very large `xenia.log` full of `d>` lines means kernel call logging is
  on; lowering Xenia's log level (`log_level` in its config) keeps it small.
  `b360.log` only contains the runtime's own messages.
* Frame rate in Xenia says little about the console; for real numbers use
  `B360_LOG_PERF = true` (and `B360_PROFILE = true` for where the time goes)
  in `game\b360_config.lua` on hardware (see README).

## 5. Reporting results back

The quickest loop is:

1. If the build fails, save the full build output (the `msbuild ... >
   build_log.txt` command above, or copy everything from Visual Studio's
   **Output** window).
2. If it builds but misbehaves on the console, grab `b360.log` from next to
   `default.xex` and, if possible, a photo of the screen.
3. Attach them in the chat. They are plain text and contain no game data.

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

**`D3DRS_PRESENTINTERVAL` is undeclared**: add
`B360_HAS_PRESENTINTERVAL_RS=0` to **C/C++ → Preprocessor → Preprocessor
Definitions**. 30 fps is then paced with timed waits instead.

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
committed, so you never need to run these yourself; they are only needed
after changing the source:

```
python tools\gen_vcxproj.py      # after adding/removing source files
python tools\embed_lua.py        # after editing src\lua\*.lua
```
