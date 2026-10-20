// Registration entry points for the love.* modules and Lua compatibility
// shims. Each open_* function installs one module into a lua_State.
#ifndef B360_MODULES_H
#define B360_MODULES_H

#include "luax.h"

namespace rt {

// Modules safe to use from any thread.
void open_filesystem(lua_State* L);
void open_timer(lua_State* L);
void open_event(lua_State* L);
void open_thread(lua_State* L);
void open_audio(lua_State* L);
void open_sound(lua_State* L);
void open_system(lua_State* L);
void open_math(lua_State* L);
void open_data(lua_State* L);
void open_joystick(lua_State* L);
void open_input_stubs(lua_State* L);   // keyboard, mouse, touch
void open_window(lua_State* L);
void open_image(lua_State* L);

// Main thread only.
void open_graphics(lua_State* L);

// LuaJIT compatibility: bit, jit, math.random (TW223), math.log(x, base),
// a portable os library.
void open_compat(lua_State* L);

// Sampling Lua profiler (profiler.cpp). profilerFrame() is called once per
// frame by love.graphics.present() and writes a report to the log when one
// is due.
void profilerStart(lua_State* L, double intervalMs, double reportSeconds);
void profilerFrame();
void profilerStop();

// Bare Lua state with the runtime's allocator (lua_alloc.cpp); close it
// with closeLuaState().
lua_State* newRawLuaState();
void closeLuaState(lua_State* L);

// Creates a fully initialised Lua state (all libraries + love modules).
lua_State* newLuaState(bool mainThread);

// Loads a file through love.filesystem as a Lua chunk ("@path" name).
int loadFileChunk(lua_State* L, const std::string& path);

}  // namespace rt

#endif
