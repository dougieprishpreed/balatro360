// Small love modules: keyboard, mouse, touch, window, system, math, data.
#include "modules.h"
#include "event.h"

#include "miniz.h"

#include <math.h>
#include <string.h>
#include <map>

namespace rt {

int graphicsWidth();
int graphicsHeight();

namespace {

int w_true(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}
int w_false(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}
int w_none(lua_State* L) {
    (void)L;
    return 0;
}
int w_nil(lua_State* L) {
    lua_pushnil(L);
    return 1;
}
int w_emptytable(lua_State* L) {
    lua_newtable(L);
    return 1;
}
int w_one(lua_State* L) {
    lua_pushinteger(L, 1);
    return 1;
}

// ---------------------------------------------------------------- keyboard
plat::Mutex& keyMutex() {
    static plat::Mutex m;
    return m;
}
std::map<std::string, bool>& keysDown() {
    static std::map<std::string, bool> m;
    return m;
}

int k_isDown(lua_State* L) {
    plat::Lock lock(keyMutex());
    for (int i = 1; i <= lua_gettop(L); ++i) {
        std::map<std::string, bool>::iterator it = keysDown().find(luaL_checkstring(L, i));
        if (it != keysDown().end() && it->second) {
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

int k_getKeyFromScancode(lua_State* L) {
    lua_pushvalue(L, 1);
    return 1;
}

const luaL_Reg kKeyboard[] = {
    {"isDown", k_isDown}, {"isScancodeDown", k_isDown}, {"setKeyRepeat", w_none},
    {"hasKeyRepeat", w_false}, {"setTextInput", w_none}, {"hasTextInput", w_false},
    {"getKeyFromScancode", k_getKeyFromScancode}, {"getScancodeFromKey", k_getKeyFromScancode},
    {"hasScreenKeyboard", w_false}, {NULL, NULL}};

// ------------------------------------------------------------------- mouse
bool g_mouseVisible = true;

int m_getPosition(lua_State* L) {
    float x = 0, y = 0;
    plat::mousePosition(x, y);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}
int m_getX(lua_State* L) {
    float x = 0, y = 0;
    plat::mousePosition(x, y);
    lua_pushnumber(L, x);
    return 1;
}
int m_getY(lua_State* L) {
    float x = 0, y = 0;
    plat::mousePosition(x, y);
    lua_pushnumber(L, y);
    return 1;
}
int m_setVisible(lua_State* L) {
    g_mouseVisible = lua_toboolean(L, 1) != 0;
    return 0;
}
int m_isVisible(lua_State* L) {
    lua_pushboolean(L, g_mouseVisible);
    return 1;
}

const luaL_Reg kMouse[] = {
    {"getPosition", m_getPosition}, {"getX", m_getX}, {"getY", m_getY},
    {"setPosition", w_none}, {"setX", w_none}, {"setY", w_none},
    {"setVisible", m_setVisible}, {"isVisible", m_isVisible}, {"setGrabbed", w_none},
    {"isGrabbed", w_false}, {"setRelativeMode", w_none}, {"getRelativeMode", w_false},
    {"isDown", w_false}, {"isCursorSupported", w_false}, {"setCursor", w_none},
    {"getCursor", w_nil}, {"getSystemCursor", w_nil}, {NULL, NULL}};

const luaL_Reg kTouch[] = {{"getTouches", w_emptytable}, {"getPosition", w_none}, {"getPressure", w_none}, {NULL, NULL}};

// ------------------------------------------------------------------ window
std::string g_title = "Untitled";

int win_getMode(lua_State* L) {
    lua_pushinteger(L, graphicsWidth());
    lua_pushinteger(L, graphicsHeight());
    lua_newtable(L);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "fullscreen");
    lua_pushstring(L, "desktop");
    lua_setfield(L, -2, "fullscreentype");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "vsync");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "msaa");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "resizable");
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "borderless");
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "centered");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "display");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "minwidth");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "minheight");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "highdpi");
    lua_pushinteger(L, 60);
    lua_setfield(L, -2, "refreshrate");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "y");
    return 3;
}

int win_getDesktopDimensions(lua_State* L) {
    lua_pushinteger(L, graphicsWidth());
    lua_pushinteger(L, graphicsHeight());
    return 2;
}

int win_getFullscreenModes(lua_State* L) {
    lua_newtable(L);
    lua_newtable(L);
    lua_pushinteger(L, graphicsWidth());
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, graphicsHeight());
    lua_setfield(L, -2, "height");
    lua_rawseti(L, -2, 1);
    return 1;
}

int win_identity(lua_State* L) {
    lua_pushnumber(L, luaL_checknumber(L, 1));
    if (lua_isnumber(L, 2)) {
        lua_pushnumber(L, lua_tonumber(L, 2));
        return 2;
    }
    return 1;
}

int win_setTitle(lua_State* L) {
    g_title = luaL_checkstring(L, 1);
    return 0;
}

int win_getTitle(lua_State* L) {
    lua_pushstring(L, g_title.c_str());
    return 1;
}

int win_getDisplayName(lua_State* L) {
    lua_pushstring(L, "Television");
    return 1;
}

int win_getPosition(lua_State* L) {
    lua_pushinteger(L, 0);
    lua_pushinteger(L, 0);
    lua_pushinteger(L, 1);
    return 3;
}

int win_getFullscreen(lua_State* L) {
    lua_pushboolean(L, 1);
    lua_pushstring(L, "desktop");
    return 2;
}

int win_showMessageBox(lua_State* L) {
    plat::log("[messagebox] %s: %s", luaL_optstring(L, 1, ""), luaL_optstring(L, 2, ""));
    if (lua_istable(L, 3)) lua_pushinteger(L, 1);
    else lua_pushboolean(L, 1);
    return 1;
}

int win_getDPIScale(lua_State* L) {
    lua_pushnumber(L, 1.0);
    return 1;
}

const luaL_Reg kWindow[] = {
    {"getMode", win_getMode}, {"setMode", w_true}, {"updateMode", w_true},
    {"getDesktopDimensions", win_getDesktopDimensions}, {"getFullscreenModes", win_getFullscreenModes},
    {"toPixels", win_identity}, {"fromPixels", win_identity}, {"getDPIScale", win_getDPIScale},
    {"getPixelScale", win_getDPIScale}, {"isOpen", w_true}, {"isCreated", w_true},
    {"setTitle", win_setTitle}, {"getTitle", win_getTitle}, {"showMessageBox", win_showMessageBox},
    {"getDisplayCount", w_one}, {"getDisplayName", win_getDisplayName}, {"setIcon", w_true},
    {"getIcon", w_nil}, {"setVSync", w_none}, {"getVSync", w_one}, {"hasFocus", w_true},
    {"hasMouseFocus", w_true}, {"isVisible", w_true}, {"isMinimized", w_false},
    {"isMaximized", w_true}, {"minimize", w_none}, {"maximize", w_none}, {"restore", w_none},
    {"close", w_none}, {"requestAttention", w_none}, {"setPosition", w_none},
    {"getPosition", win_getPosition}, {"setFullscreen", w_true}, {"getFullscreen", win_getFullscreen},
    {"getSafeArea", w_none}, {"setDisplaySleepEnabled", w_none}, {"isDisplaySleepEnabled", w_false},
    {NULL, NULL}};

// ------------------------------------------------------------------ system
plat::Mutex& clipMutex() {
    static plat::Mutex m;
    return m;
}
std::string& clipboard() {
    static std::string s;
    return s;
}

int sys_getOS(lua_State* L) {
    lua_pushstring(L, plat::osName());
    return 1;
}

int sys_getProcessorCount(lua_State* L) {
    lua_pushinteger(L, plat::processorCount());
    return 1;
}

int sys_getClipboardText(lua_State* L) {
    plat::Lock lock(clipMutex());
    lua_pushstring(L, clipboard().c_str());
    return 1;
}

int sys_setClipboardText(lua_State* L) {
    plat::Lock lock(clipMutex());
    clipboard() = luaL_checkstring(L, 1);
    return 0;
}

int sys_getPowerInfo(lua_State* L) {
    lua_pushstring(L, "nobattery");
    lua_pushnil(L);
    lua_pushnil(L);
    return 3;
}

int sys_getPreferredLocales(lua_State* L) {
    lua_newtable(L);
    lua_pushstring(L, "en-US");
    lua_rawseti(L, -2, 1);
    return 1;
}

// Extension: player name (Xbox gamertag), or nil.
int sys_getUsername(lua_State* L) {
    std::string name;
    if (plat::userName(name)) lua_pushstring(L, name.c_str());
    else lua_pushnil(L);
    return 1;
}

const luaL_Reg kSystem[] = {
    {"getOS", sys_getOS}, {"getProcessorCount", sys_getProcessorCount},
    {"getClipboardText", sys_getClipboardText}, {"setClipboardText", sys_setClipboardText},
    {"openURL", w_false}, {"getPowerInfo", sys_getPowerInfo}, {"vibrate", w_none},
    {"hasBackgroundMusic", w_false}, {"getPreferredLocales", sys_getPreferredLocales},
    {"getUsername", sys_getUsername}, {NULL, NULL}};

// -------------------------------------------------------------------- math
// love.math.random uses its own xorshift generator (like LÖVE's).
u64 g_rngState = 0x2545F4914F6CDD1DULL;

double rngNext() {
    g_rngState ^= g_rngState >> 12;
    g_rngState ^= g_rngState << 25;
    g_rngState ^= g_rngState >> 27;
    u64 r = g_rngState * 2685821657736338717ULL;
    return (double)(r >> 11) * (1.0 / 9007199254740992.0);
}

int lm_random(lua_State* L) {
    double r = rngNext();
    if (lua_isnumber(L, 1)) {
        double lo = 1, hi = lua_tonumber(L, 1);
        if (lua_isnumber(L, 2)) {
            lo = hi;
            hi = lua_tonumber(L, 2);
        }
        lua_pushnumber(L, floor(r * (hi - lo + 1)) + lo);
    } else {
        lua_pushnumber(L, r);
    }
    return 1;
}

int lm_setRandomSeed(lua_State* L) {
    u64 seed = (u64)(i64)luaL_checknumber(L, 1);
    g_rngState = seed ? seed : 0x2545F4914F6CDD1DULL;
    for (int i = 0; i < 3; ++i) rngNext();
    return 0;
}

int lm_randomNormal(lua_State* L) {
    double stddev = luaL_optnumber(L, 1, 1.0);
    double mean = luaL_optnumber(L, 2, 0.0);
    double u1 = rngNext(), u2 = rngNext();
    if (u1 < 1e-300) u1 = 1e-300;
    lua_pushnumber(L, mean + stddev * sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2));
    return 1;
}

int lm_gammaToLinear(lua_State* L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        double c = luaL_checknumber(L, i);
        lua_pushnumber(L, c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
    }
    return n;
}

int lm_linearToGamma(lua_State* L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        double c = luaL_checknumber(L, i);
        lua_pushnumber(L, c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055);
    }
    return n;
}

const luaL_Reg kMath[] = {
    {"random", lm_random}, {"setRandomSeed", lm_setRandomSeed}, {"randomNormal", lm_randomNormal},
    {"gammaToLinear", lm_gammaToLinear}, {"linearToGamma", lm_linearToGamma}, {NULL, NULL}};

// -------------------------------------------------------------------- data
enum Format { FMT_DEFLATE, FMT_ZLIB, FMT_GZIP, FMT_UNKNOWN };

Format parseFormat(const char* f) {
    if (strcmp(f, "deflate") == 0) return FMT_DEFLATE;
    if (strcmp(f, "zlib") == 0) return FMT_ZLIB;
    if (strcmp(f, "gzip") == 0) return FMT_GZIP;
    return FMT_UNKNOWN;
}

// love.data.compress(container, format, rawstring [, level])
int d_compress(lua_State* L) {
    Format fmt = parseFormat(luaL_checkstring(L, 2));
    size_t len = 0;
    const char* src = luaL_checklstring(L, 3, &len);
    int level = (int)luaL_optnumber(L, 4, -1);
    if (fmt == FMT_UNKNOWN || fmt == FMT_GZIP) return luaL_error(L, "Unsupported compression format");
    if (level < 0) level = MZ_DEFAULT_LEVEL;
    if (level > 9) level = 9;
    int flags = tdefl_create_comp_flags_from_zip_params(level, fmt == FMT_ZLIB ? 15 : -15, MZ_DEFAULT_STRATEGY);
    size_t outLen = 0;
    void* out = tdefl_compress_mem_to_heap(src, len, &outLen, flags);
    if (!out) return luaL_error(L, "Could not compress data.");
    lua_pushlstring(L, (const char*)out, outLen);
    mz_free(out);
    return 1;
}

// love.data.decompress(container, format, compressedstring)
int d_decompress(lua_State* L) {
    Format fmt = parseFormat(luaL_checkstring(L, 2));
    size_t len = 0;
    const char* src = luaL_checklstring(L, 3, &len);
    if (fmt == FMT_UNKNOWN || fmt == FMT_GZIP) return luaL_error(L, "Unsupported compression format");
    int flags = fmt == FMT_ZLIB ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0;
    size_t outLen = 0;
    void* out = tinfl_decompress_mem_to_heap(src, len, &outLen, flags);
    if (!out) return luaL_error(L, "Could not decompress data.");
    lua_pushlstring(L, (const char*)out, outLen);
    mz_free(out);
    return 1;
}

const luaL_Reg kData[] = {{"compress", d_compress}, {"decompress", d_decompress}, {NULL, NULL}};

const luaL_Reg kSound[] = {{NULL, NULL}};
const luaL_Reg kImage[] = {{NULL, NULL}};

}  // namespace

namespace input {
void update() {
    std::vector<plat::KeyEvent> keys;
    plat::keyPoll(keys);
    for (size_t i = 0; i < keys.size(); ++i) {
        {
            plat::Lock lock(keyMutex());
            keysDown()[keys[i].key] = keys[i].pressed;
        }
        std::vector<Variant> a;
        a.push_back(Variant::fromString(keys[i].key));
        a.push_back(Variant::fromString(keys[i].key));
        a.push_back(Variant::fromBool(false));
        event::push(keys[i].pressed ? "keypressed" : "keyreleased", a);
    }
}
}  // namespace input

void open_input_stubs(lua_State* L) {
    luax_registermodule(L, "keyboard", kKeyboard);
    luax_registermodule(L, "mouse", kMouse);
    luax_registermodule(L, "touch", kTouch);
}
void open_window(lua_State* L) { luax_registermodule(L, "window", kWindow); }
void open_system(lua_State* L) { luax_registermodule(L, "system", kSystem); }
void open_math(lua_State* L) { luax_registermodule(L, "math", kMath); }
void open_data(lua_State* L) { luax_registermodule(L, "data", kData); }
void open_sound(lua_State* L) { luax_registermodule(L, "sound", kSound); }
void open_image(lua_State* L) { luax_registermodule(L, "image", kImage); }

}  // namespace rt
