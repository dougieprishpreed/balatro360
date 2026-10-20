// Runtime entry: creates Lua states, registers the love modules and runs
// the embedded boot script.
#include "runtime.h"
#include "modules.h"
#include "event.h"
#include "embedded_lua.h"

#include <string.h>

namespace rt {

bool initGraphics(int width, int height);
void shutdownAudio();

namespace {

int luaPrint(lua_State* L) {
    int n = lua_gettop(L);
    std::string line;
    lua_getglobal(L, "tostring");
    for (int i = 1; i <= n; ++i) {
        lua_pushvalue(L, -1);
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        const char* s = lua_tostring(L, -1);
        if (!s) return luaL_error(L, "'tostring' must return a string to 'print'");
        if (i > 1) line += "\t";
        line += s;
        lua_pop(L, 1);
    }
    plat::log("%s", line.c_str());
    return 0;
}

void openStandardLibs(lua_State* L) {
    // linit.c would also open io and os, which need CRT functions the 360
    // doesn't have; we provide our own os library in open_compat().
    static const luaL_Reg libs[] = {
        {"", luaopen_base},
        {LUA_LOADLIBNAME, luaopen_package},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_DBLIBNAME, luaopen_debug},
        {NULL, NULL}};
    for (const luaL_Reg* lib = libs; lib->func; ++lib) {
        lua_pushcfunction(L, lib->func);
        lua_pushstring(L, lib->name);
        lua_call(L, 1, 0);
    }
    lua_pushcfunction(L, luaPrint);
    lua_setglobal(L, "print");
}

bool runEmbedded(lua_State* L, const unsigned char* code, unsigned int size, const char* name, int nresults) {
    if (luaL_loadbuffer(L, (const char*)code, size, name) != 0 || lua_pcall(L, 0, nresults, 0) != 0) {
        plat::log("[runtime] %s failed: %s", name, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

}  // namespace

lua_State* newLuaState(bool mainThread) {
    lua_State* L = luaL_newstate();
    openStandardLibs(L);
    open_compat(L);

    lua_newtable(L);
    lua_setglobal(L, "love");
    open_filesystem(L);
    open_timer(L);
    open_event(L);
    open_thread(L);
    open_audio(L);
    open_sound(L);
    open_system(L);
    open_math(L);
    open_data(L);
    open_joystick(L);
    open_input_stubs(L);
    open_window(L);
    open_image(L);
    if (mainThread) open_graphics(L);

    runEmbedded(L, kLua_common, kLua_common_size, "=[b360 common]", 0);
    return L;
}

int run(int argc, char** argv) {
    plat::Config cfg;
    if (!plat::init(cfg)) {
        plat::fatal("Platform initialisation failed");
        return 1;
    }
    plat::log("Balatro360 runtime starting (%s)", plat::osName());
    if (!initGraphics(cfg.width, cfg.height)) {
        plat::fatal("Could not initialise graphics");
        return 1;
    }

    lua_State* L = newLuaState(true);

    // arg table
    lua_newtable(L);
    for (int i = 0; i < argc; ++i) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i);
    }
    lua_setglobal(L, "arg");

    // Poll controllers once so love.joystick.getJoysticks() works in love.load.
    joystick::update();

    // Hooks are installed by boot.lua after conf.lua has run.
    lua_getglobal(L, "love");
    if (luaL_loadbuffer(L, (const char*)kLua_hooks, kLua_hooks_size, "=[b360 hooks]") == 0) {
        lua_setfield(L, -2, "_b360_hooks");
    } else {
        plat::log("[runtime] hooks failed to load: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    int code = 0;
    if (luaL_loadbuffer(L, (const char*)kLua_boot, kLua_boot_size, "=[love \"boot.lua\"]") != 0) {
        plat::log("[runtime] boot failed to load: %s", lua_tostring(L, -1));
        code = 1;
    } else if (lua_pcall(L, 0, 1, 0) != 0) {
        plat::log("[runtime] boot error: %s", lua_tostring(L, -1));
        code = 1;
    } else {
        code = lua_isnumber(L, -1) ? (int)lua_tonumber(L, -1) : 0;
    }
    plat::log("Balatro360 exiting with code %d", code);
    shutdownAudio();
    plat::shutdown();
    // Worker threads (sound/save managers) block forever on their channels,
    // exactly like in LÖVE; the process exit tears them down.
    return code;
}

}  // namespace rt
