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
   /     |     |    \                          /          |          \
vulkan/ d3d9/  gl/  null/                   xsdk/      xbox360/      posix/
(360 xSDK (360   (Linux) (tests)           (pthreads,  (XDK: XInput, (pthreads,
 + Linux) XDK)                              XAM input,  XAudio2,      files,
                                            xSDK audio) files)        scripted input)
```

The xSDK build (`build/xsdk`, `tools/xsdk`) uses `vulkan/` and `xsdk/`; the
older XDK build (`build/xbox360`) uses `d3d9/` and `xbox360/`.

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

In the xSDK build the device is `src/gfx/threaded_device.cpp`, which wraps
the Vulkan one: the game's thread records a frame's device calls into a
packet (vertices, pixels and uniform values copied; shader creation and big
uploads wait for the render thread) and hands it over at present(); the
render thread replays packets on the real device. With `B360_INTERPOLATE =
true` (off by default), at 30 fps it presents a frame halfway between each
two first: draws that match the previous
packet's (same target, texture, shader, blend and vertex count, in order;
none that moved more than 192 pixels) average their vertex positions and
colours, and uniforms named after time are averaged. Each packet restates
its starting state and each shader's uniforms, so replaying one twice
draws the same. What a packet destroys is freed once it won't be replayed
again.

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

### Vulkan renderer (`src/gfx/vulkan/device_vk.cpp`)

The xSDK build's renderer, on vulkan4xenon; on Linux the same code draws
offscreen on any Vulkan 1.0 driver (`make GFX=vulkan`), which is how it is
tested without a console.

* One command buffer per frame, two frames in flight. vulkan4xenon replays
  it into the GPU's ring at `vkQueueSubmit`. Each frame has its own region
  of a vertex ring (4 MB), a uniform ring (1 MB) and a staging ring (8 MB);
  a frame that fills one submits what it has and waits.
* The back buffer (the swapchain on the console) and every canvas are
  `B8G8R8A8` render targets, drawn in render passes that clear, load or
  ignore what the image held, chosen when the pass begins; vulkan4xenon
  keeps the targets in eDRAM and resolves them into their images. Without a
  display (Linux) the back buffer is an offscreen image read back for
  screenshots.
* Images are `R8G8B8A8` optimal-tiled images filled with
  `vkCmdCopyBufferToImage` from staging rows spaced as xsdk spaces a linear
  texture's rows, at 4 KB-aligned offsets: vulkan4xenon then draws the copy
  on the GPU instead of copying on the CPU after waiting for the GPU. Glyph
  updates to a font page grow to vulkan4xenon's 8-pixel grid from a CPU
  copy of the page for the same reason. Images made with `mipmaps = true`
  (Balatro's atlases) get a full chain from a 2x2 box filter and are
  sampled trilinearly.
* Shaders: `buildVulkanGLSLStage` (`shader_translate.cpp`) wraps a LÖVE
  shader into Vulkan GLSL 4.50. All uniforms go into one std140 block at a
  dynamic offset (built-in projection and `love_ScreenSize` first; bools
  and ints as floats); the main texture is binding 1. glslang
  (glslang-xenon on the console) makes SPIR-V; on the console
  vulkan4xenon compiles that with xost, xSDK's shader compiler, when the
  module is made. Both results are cached under `game:\cache`.
  The translator also keeps to xost's subset: functions with early
  returns are rewritten to return once (xost inlines only those); bool
  uniforms are tested with `> 0.5` (`bool(x)` is an unordered compare xost
  lacks); `atan(y, x)` becomes a polynomial `b360_atan2`; `int n = 4;`
  locals that never change become `const`; and the innermost loop of each
  nest gets a bound of `+ int(VaryingTexCoord.z)` (always 0), which makes it
  a hardware loop: unrolled, CRT's 7x7 bloom needed 67 registers and the
  hologram's 9x9 glow 175, of the 64 a pixel shader has.
  `tools/xsdk/check_shaders.py` runs the game's shaders through the same
  steps and xost on the PC. A shader that still fails to build is drawn
  with the default shader and logged, rather than stopping the game.
* Writes into the GPU's write-combined memory go through xSDK's `memcpy`,
  never inlined copies, which GCC turns into 8-byte stores that fault
  there when not 8-byte aligned.
* 30 fps: the swapchain shows a frame every vblank, so the 30 fps mode
  queues each frame at least two vblanks after the last
  (`xsdk_present_stats`).

### Xbox 360 renderer, XDK (`src/gfx/d3d9/device_d3d9.cpp`)

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

Fonts load lazily (Balatro creates one per language, 60+ MB of CJK fonts)
except for the current language's font, which `hooks.lua` loads during
start-up. Loading rasterises printable ASCII straight away: adding a glyph
later means writing to an atlas the GPU may be reading, which on the 360
waits for the GPU to finish all queued work.

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
then the game folder; writes always go to the save folder. Reads take no lock, so the
game thread never waits for another thread's file I/O (Balatro's sound
thread loads multi-megabyte music tracks, its save thread writes saves);
only writers are serialised. Reads or writes slower than 50 ms are noted in
the log (`[fs]`). Directory listings
are merged and sorted (like PhysFS). Save files use LÖVE's formats
(`love.data.compress` raw deflate via miniz), so PC saves are compatible.

## Input

XInput state is polled once per `love.event.pump()` and turned into
`joystickadded`, `gamepadpressed/released`, `gamepadaxis` and the
`joystick*` events. Sticks use SDL conventions (Y down) with XInput's standard
dead zones. The joystick reports an SDL mapping string for an Xbox 360
controller, so Balatro shows Xbox button prompts.
