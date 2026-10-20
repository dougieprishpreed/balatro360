# Architecture

Balatro360 is a minimal reimplementation of the LÖVE 11 API, just big
enough to run Balatro, designed so that only a thin platform layer and one
renderer differ between the Xbox 360 and the Linux development build.

```
                 Balatro (Lua, unmodified, from your Balatro.exe)
                                    |
      src/lua/boot.lua, hooks.lua, common.lua  (embedded LÖVE-style boot)
                                    |
  src/runtime/  love.graphics  love.audio  love.thread  love.filesystem ...
                (C++98, platform independent)
          |                 |                          |
   src/gfx/device.h   plat:: audio callback   src/platform/platform.h
     /     |     \                                /             \
  d3d9/   gl/   null/                       xbox360/           posix/
 (360)  (Linux) (tests)                 (XDK: XInput,     (pthreads, files,
                                         XAudio2, files)   scripted input)
```

## Lua

The runtime only uses the Lua 5.1 C API, so it builds against either:

* **LuaJIT 2.0** (`B360_LUAJIT`): LuaJIT's official Xbox 360 build
  (`src/xedkbuild.bat`, interpreter only, VM in PowerPC assembly). This is the
  fast option (about 2x PUC Lua on the Balatro workload) and is LuaJIT
  itself, so behaviour matches PC exactly. LuaJIT unwinds errors with longjmp
  there, so runtime code never holds locks across calls that can raise Lua
  errors.
  LuaJIT 2.1 removed the PowerPC PE object writer that build needs (2020),
  so the 360 uses the maintained 2.0 branch; both share the same random
  number generator and `math.log`.
* **PUC Lua 5.1.5** (the plain `Release` configuration), compiled **as C++**
  so that Lua errors are C++ exceptions and unwind our C++ frames correctly
  (RAII locks, std::string temporaries).
* `src/runtime/l_compat.cpp` restores the LuaJIT behaviour Balatro relies on:
  * `math.random`/`math.randomseed` are a bit-exact port of LuaJIT 2.1's
    Tausworthe generator. Balatro reseeds and draws from it for every random
    decision, so this is what makes seeds identical to PC.
    `tools/` tests compare against real LuaJIT on x86-64 and big-endian
    PowerPC.
  * `math.log(x, base)` with LuaJIT's `log2(x) * (1/log2(base))` formulation
    (Balatro floors its results).
  * `bit` (LuaBitOp, the library LuaJIT's `bit` is based on), a `jit` stub
    table, and an `os` library that only uses functions the 360 CRT has.
* Lua sources and `l_compat.cpp` are built with `/fp:strict` on the 360 so
  the compiler can't fuse multiply-adds, which would change results in the
  last bit compared to PC.
* `ltable.c` is patched to reuse dead keys in a key's own collision chain.
  Balatro constantly sets fields to `nil` and back (`self.CALCING = true`,
  ...). Stock 5.1 turns every removed key into a dead node at each GC cycle
  and allocates a fresh node on re-insertion, so those chains grew to 30+
  nodes and table lookups dominated the profile.
* After all game code is loaded, inherited methods are copied into each
  subclass (`hooks.lua`, `flatten_classes`) so method calls don't walk the
  `__index` chain.
* Every Lua state gets its own small-block allocator (`lua_alloc.cpp`):
  blocks up to 256 bytes come from per-size free lists without locks or
  headers, larger ones from the CRT heap. (64-bit host LuaJIT keeps its own.)

## Frame pacing

`love.graphics.present()` goes through `FramePacer` (`src/runtime/pacing.cpp`).
At 60 fps a new frame is shown every vblank; at 30 fps every second one
(`D3DRS_PRESENTINTERVAL` on the 360). In `"auto"` mode (the default on the
console) the pacer drops to 30 when 8 of the last 60 frames missed a vblank
and returns to 60 once the CPU time per frame has stayed below 13.3 ms for
3 s; if that attempt fails again within 10 s, the next one waits twice as
long (up to a minute). Balatro animates with the measured frame time, so
gameplay speed is the same either way.

## Performance tools

* `B360_LOG_PERF` wraps `love.update`, `love.draw` and `present` and logs
  their average time per frame with draw call and vertex counts.
* `B360_PROFILE` (`src/runtime/profiler.cpp`) is a sampling profiler: a
  helper thread arms a one-shot Lua count hook about every millisecond, and
  the hook records the running function and its callers on the game thread.
  Time in C functions is charged to the Lua line that called them.

On the Linux build, nearly all of a gameplay frame is Balatro's own Lua
(card movement, UI layout and drawing, its garbage collector budget); the
runtime's C++ side is a few percent.

## Boot sequence

`rt::run()` (`src/runtime/runtime.cpp`) initialises the platform and the
renderer, creates the main Lua state with every `love.*` module and runs the
embedded `boot.lua`:

1. `common.lua`, which runs in every Lua state including `love.thread`
   workers: `require` searches the game folder through `love.filesystem`;
   stub `https`/`luasteam` modules.
2. `conf.lua` / `love.conf`, like LÖVE.
3. `hooks.lua`: wraps `require` so code can run right after specific game
   modules load. It wraps `Game.init` (after `game.lua` loads) to set the
   game's own console feature flags (`F_VIDEO_SETTINGS = false`,
   `F_HIDE_BG = true`, rumble, the gamertag in the main menu, ...) and
   applies the optional `b360_config.lua`. No game files are modified.
4. `main.lua` and `love.run()`, with LÖVE 11's error handler flow (Balatro's
   own `love.errhand` is used for its error screen).

## Graphics

`src/runtime/l_graphics.cpp` implements the state machine (colour, shader,
canvas, blend, scissor, transform stack with `push`/`pop`), and turns every
draw into CPU-transformed vertices in pixel coordinates. Vertices are batched
until the texture, shader or state changes. Like LÖVE 11, shaders receive
vertex positions already in screen pixels; several of Balatro's vertex
shaders depend on that (card hover tilt).

Backends implement `gfx::Device` (`src/gfx/device.h`): textures, render
targets, shaders, triangle lists. All backends use the same orientation (row 0
at the top), so canvases are drawn like any other image.

### Shaders

LÖVE shaders are GLSL with LÖVE extras (`extern`, `number`, `Image`,
`Texel`, `effect()`/`position()` entry points). `src/gfx/shader_translate.cpp`:

* analyses the source: which entry points exist, which uniforms are declared
  (used for `Shader:send` type conversion);
* wraps it into complete GLSL 1.20 stages for the OpenGL backend; and
* translates it into **HLSL shader model 3** for Direct3D. The translator is
  token based and handles the GLSL subset LÖVE shaders use: type names
  (`vec4` to `float4`, ...), single-argument constructors (scalar broadcast
  and vector truncation become casts), `mix`/`fract`/`mod`/`atan(y,x)`/
  `texture2D`, matrix-vector `*` to `mul()`, `static` for globals, HLSL
  reserved words used as identifiers (`texture`, `sampler`, ...), and
  `bool`/`int` uniforms as floats.

The translation runs on the console at load time, so modded shaders work
too. All 19 Balatro shaders compile as `vs_3_0`/`ps_3_0` with vkd3d's HLSL
compiler; the only remaining vkd3d failures are loops, which vkd3d's SM3
backend doesn't implement yet and Microsoft's compiler handles.

### Xbox 360 renderer (`src/gfx/d3d9/device_d3d9.cpp`)

* Back buffer 1280x720 A8R8G8B8, vsync on. The console's scaler handles other
  output modes.
* Canvases: one EDRAM render target placed after the back buffer and shared
  by all canvases. Each canvas has its own texture; switching away from a
  canvas `Resolve()`s EDRAM into it, and re-binding a canvas without clearing
  first copies the texture back into EDRAM.
* CPU textures (images, glyph atlases) use `D3DFMT_LIN_A8R8G8B8`, written as
  native-endian `0xAARRGGBB` DWORDs, so no tiling code is needed.
* Shaders are compiled with `D3DXCompileShader`. Each uniform's constant
  registers are looked up once in the D3DX constant tables, and values are
  then written with `Set*ShaderConstantF`. Constants are device-global, so
  after another shader drew everything is re-uploaded; otherwise only the
  uniforms that changed. The core skips `Shader:send` calls that don't
  change a value (Balatro re-sends about a dozen uniforms before every
  sprite).

### Fonts

stb_truetype rasterises glyphs on demand into 1024x1024 atlas pages. Balatro
creates its fonts at 200px and draws them scaled down, so glyphs are
rasterised at most 64px and scaled; this saves memory with no visible
difference at 720p. Metrics follow FreeType's rounding (ascender rounded up,
descender down, advances rounded) to keep text layout close to PC.

## Audio

`src/runtime/l_audio.cpp` is a software mixer: sources are resampled with
linear interpolation (for pitch), mixed in float and clipped to 16-bit stereo
at 48 kHz. Static sources are decoded fully at load; streams (music) decode
incrementally with stb_vorbis. The platform pulls blocks from the mixer on its
own thread (an XAudio2 source voice on the 360).

Balatro runs its sound manager in a `love.thread`, so audio objects are
thread safe (one mixer lock).

## Threads and channels

`love.thread` creates a fresh Lua state per thread (all modules except
graphics). Channels deep-copy values (`src/runtime/variant.h`) including nested
tables, which Balatro uses to send save data to its save thread. A copied
table is immutable and reference counted, so it is built once and never
copied again on its way through the channel. On the 360
Lua workers are spread over hardware threads 2 to 5, audio runs on 3, and the
game on 0.

## File system

Game paths are `/`-separated and relative. Reads check the save folder first,
then the game folder; writes always go to the save folder. Directory listings
are merged and sorted (like PhysFS). Save files use LÖVE's formats
(`love.data.compress` raw deflate via miniz), so PC saves are compatible.

## Input

XInput state is polled once per `love.event.pump()` and turned into
`joystickadded`, `gamepadpressed/released`, `gamepadaxis` and the
`joystick*` events. Sticks use SDL conventions (Y down) with XInput's standard
dead zones. The joystick reports an SDL mapping string for an Xbox 360
controller, so Balatro shows Xbox button prompts.
