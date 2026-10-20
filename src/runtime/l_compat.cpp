// LuaJIT compatibility for PUC Lua 5.1.
//
// Balatro ships for LÖVE, which runs on LuaJIT 2.1. LuaJIT cannot run on the
// Xbox 360 (no PowerPC/Xenon support, no executable memory for homebrew in
// general), so we run the game on the reference Lua 5.1 interpreter and fill
// in the LuaJIT behaviour the game depends on:
//
//   * math.random / math.randomseed use LuaJIT's Tausworthe (TW223)
//     generator, bit for bit. Balatro's seeded runs call
//     math.randomseed(seed) + math.random() for every random decision, so
//     this keeps seeds identical to the PC version.
//   * math.log(x, base) accepts the optional base (Lua 5.2 / LuaJIT).
//   * a "jit" table (jit.arch, jit.os, jit.on/off/flush/status).
//   * the "bit" module (LuaBitOp, which is what LuaJIT's bit is based on).
//   * a small os library that only uses functions available on the 360.
#include "modules.h"

#include <math.h>
#include <string.h>
#include <time.h>

#if !defined(B360_LUAJIT)
extern int luaopen_bit(lua_State* L);
#endif

namespace rt {

namespace {

#if !defined(B360_LUAJIT)
// --------------------------------------------------- LuaJIT TW223 generator
struct RandomState {
    u64 gen[4];
};

union U64Double {
    u64 u;
    double d;
};

#define TW223_GEN(i, k, q, s)                                                         \
    z = rs->gen[i];                                                                   \
    z = (((z << q) ^ z) >> (k - s)) ^ ((z & ((u64)(i64)-1 << (64 - k))) << s);        \
    r ^= z;                                                                           \
    rs->gen[i] = z;

// Returns a double in [1.0, 2.0) as raw bits.
u64 randomStep(RandomState* rs) {
    u64 z, r = 0;
    TW223_GEN(0, 63, 31, 18)
    TW223_GEN(1, 58, 19, 28)
    TW223_GEN(2, 55, 24, 7)
    TW223_GEN(3, 47, 21, 8)
    return (r & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
}

void randomInit(RandomState* rs, double d) {
    u32 r = 0x11090601;   // 64-k[i] as four 8 bit constants.
    for (int i = 0; i < 4; ++i) {
        U64Double u;
        u32 m = 1u << (r & 255);
        r >>= 8;
        // Two separate operations: must not be contracted into a fused
        // multiply-add, which would change the result on PowerPC.
        volatile double t = d * 3.14159265358979323846;
        d = t + 2.7182818284590452354;
        u.d = d;
        if (u.u < m) u.u += m;   // Ensure k[i] MSB of gen[i] are non-zero.
        rs->gen[i] = u.u;
    }
    for (int i = 0; i < 10; ++i) randomStep(rs);
}

RandomState* getRandomState(lua_State* L) {
    return (RandomState*)lua_touserdata(L, lua_upvalueindex(1));
}

int math_random(lua_State* L) {
    RandomState* rs = getRandomState(L);
    int n = lua_gettop(L);
    U64Double u;
    u.u = randomStep(rs);
    double d = u.d - 1.0;
    if (n > 0) {
        double r1 = luaL_checknumber(L, 1);
        if (n == 1) {
            d = floor(d * r1) + 1.0;   // d is an int in range [1, r1]
        } else {
            double r2 = luaL_checknumber(L, 2);
            d = floor(d * (r2 - r1 + 1.0)) + r1;   // d is an int in range [r1, r2]
        }
    }
    lua_pushnumber(L, d);
    return 1;
}

int math_randomseed(lua_State* L) {
    RandomState* rs = getRandomState(L);
    randomInit(rs, luaL_checknumber(L, 1));
    return 0;
}

// LuaJIT: log2(x) * (1 / log2(base)) -- "Do NOT join the expression to x / y."
// The VS2010 CRT has no log2(); split off the binary exponent so powers of
// two stay exact and the rest is accurate to about an ulp.
double portableLog2(double x) {
    if (!(x > 0) || x != x || x > 1.7976931348623157e308) return log(x) * 1.44269504088896340736;
    int e = 0;
    double m = frexp(x, &e);   // x = m * 2^e, m in [0.5, 1)
    if (m == 0.5) return (double)(e - 1);
    return (double)e + log(m) * 1.44269504088896340736;
}

int math_log(lua_State* L) {
    double x = luaL_checknumber(L, 1);
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        double y = luaL_checknumber(L, 2);
        volatile double lx = portableLog2(x);
        volatile double ly = 1.0 / portableLog2(y);
        double r = lx * ly;
        // LuaJIT's log2 is correctly rounded, so exact powers (log(1e7, 10))
        // give exact integers; Balatro floors these results.
        double n = floor(r + 0.5);
        if (fabs(r - n) < 1e-9 && pow(y, n) == x) r = n;
        lua_pushnumber(L, r);
    } else {
        lua_pushnumber(L, log(x));
    }
    return 1;
}

// ------------------------------------------------------------------ jit stub
int jit_noop(lua_State* L) {
    (void)L;
    return 0;
}

int jit_status(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

#endif  // !B360_LUAJIT

// ------------------------------------------------------------------------ os
int os_time(lua_State* L) {
    if (lua_istable(L, 1)) {
        struct tm ts;
        memset(&ts, 0, sizeof(ts));
        ts.tm_sec = (int)luax_numberfield(L, 1, "sec", 0);
        ts.tm_min = (int)luax_numberfield(L, 1, "min", 0);
        ts.tm_hour = (int)luax_numberfield(L, 1, "hour", 12);
        ts.tm_mday = (int)luax_numberfield(L, 1, "day", 1);
        ts.tm_mon = (int)luax_numberfield(L, 1, "month", 1) - 1;
        ts.tm_year = (int)luax_numberfield(L, 1, "year", 1970) - 1900;
        ts.tm_isdst = -1;
        time_t t = mktime(&ts);
        lua_pushnumber(L, (lua_Number)t);
        return 1;
    }
    lua_pushnumber(L, (lua_Number)plat::unixTime());
    return 1;
}

int os_clock(lua_State* L) {
    lua_pushnumber(L, plat::getTime());
    return 1;
}

int os_difftime(lua_State* L) {
    lua_pushnumber(L, luaL_checknumber(L, 1) - luaL_optnumber(L, 2, 0));
    return 1;
}

void setfield(lua_State* L, const char* k, int v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, k);
}

int os_date(lua_State* L) {
    const char* fmt = luaL_optstring(L, 1, "%c");
    time_t t = lua_isnoneornil(L, 2) ? (time_t)plat::unixTime() : (time_t)luaL_checknumber(L, 2);
    struct tm* stm;
    if (*fmt == '!') {
        stm = gmtime(&t);
        ++fmt;
    } else {
        stm = localtime(&t);
    }
    if (!stm) {
        lua_pushnil(L);
        return 1;
    }
    if (strcmp(fmt, "*t") == 0) {
        lua_createtable(L, 0, 9);
        setfield(L, "sec", stm->tm_sec);
        setfield(L, "min", stm->tm_min);
        setfield(L, "hour", stm->tm_hour);
        setfield(L, "day", stm->tm_mday);
        setfield(L, "month", stm->tm_mon + 1);
        setfield(L, "year", stm->tm_year + 1900);
        setfield(L, "wday", stm->tm_wday + 1);
        setfield(L, "yday", stm->tm_yday + 1);
        lua_pushboolean(L, stm->tm_isdst);
        lua_setfield(L, -2, "isdst");
        return 1;
    }
    char buf[256];
    size_t n = strftime(buf, sizeof(buf), fmt, stm);
    lua_pushlstring(L, buf, n);
    return 1;
}

int os_getenv(lua_State* L) {
    lua_pushnil(L);
    return 1;
}

int os_exit(lua_State* L) {
    // Route through love.event.quit so the platform shuts down cleanly.
    lua_getglobal(L, "love");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "event");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "quit");
            if (lua_isfunction(L, -1)) lua_call(L, 0, 0);
            else lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return 0;
}

int os_tmpname(lua_State* L) {
    lua_pushstring(L, "tmp");
    return 1;
}

int os_remove(lua_State* L) {
    lua_pushnil(L);
    lua_pushstring(L, "os.remove is not available");
    return 2;
}

const luaL_Reg kOs[] = {
    {"time", os_time}, {"clock", os_clock}, {"difftime", os_difftime}, {"date", os_date},
    {"getenv", os_getenv}, {"exit", os_exit}, {"tmpname", os_tmpname}, {"remove", os_remove},
    {"rename", os_remove}, {NULL, NULL}};

}  // namespace

void open_compat(lua_State* L) {
    // os (both Lua flavours: only uses functions the 360 CRT has)
    luaL_register(L, "os", kOs);
    lua_pop(L, 1);

#if defined(B360_LUAJIT)
    // LuaJIT provides math.random, math.log(x, base), bit and jit itself.
#else
    // math.random / math.randomseed (LuaJIT TW223), math.log(x, base)
    lua_getglobal(L, "math");
    RandomState* rs = (RandomState*)lua_newuserdata(L, sizeof(RandomState));
    randomInit(rs, 0.0);
    lua_pushvalue(L, -1);
    lua_pushcclosure(L, math_random, 1);
    lua_setfield(L, -3, "random");
    lua_pushcclosure(L, math_randomseed, 1);
    lua_setfield(L, -2, "randomseed");
    lua_pushcfunction(L, math_log);
    lua_setfield(L, -2, "log");
    lua_pop(L, 1);

    // bit (also available through require "bit")
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(L, luaopen_bit);
    lua_setfield(L, -2, "bit");
    lua_pop(L, 2);
    lua_pushcfunction(L, luaopen_bit);
    lua_call(L, 0, 0);   // also sets the global "bit"

    // jit
    lua_newtable(L);
    lua_pushstring(L, "ppc");
    lua_setfield(L, -2, "arch");
    lua_pushstring(L, plat::osName());
    lua_setfield(L, -2, "os");
    lua_pushstring(L, "Lua 5.1 (LuaJIT compatibility layer)");
    lua_setfield(L, -2, "version");
    lua_pushinteger(L, 20100);
    lua_setfield(L, -2, "version_num");
    lua_pushcfunction(L, jit_noop);
    lua_setfield(L, -2, "on");
    lua_pushcfunction(L, jit_noop);
    lua_setfield(L, -2, "off");
    lua_pushcfunction(L, jit_noop);
    lua_setfield(L, -2, "flush");
    lua_pushcfunction(L, jit_status);
    lua_setfield(L, -2, "status");
    lua_pushvalue(L, -1);
    lua_setglobal(L, "jit");
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "loaded");
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "jit");
    lua_pop(L, 3);
#endif
}

}  // namespace rt
