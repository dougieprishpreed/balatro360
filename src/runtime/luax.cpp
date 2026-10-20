#include "luax.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <map>

namespace rt {

namespace {

struct Proxy {
    u32 magic;
    Object* obj;
    const char* type;   // static string owned by the TypeDef table
};
const u32 kProxyMagic = 0xB360B1EC;

// Type hierarchy: name -> parent. Filled when types are registered, which
// happens on the main thread before any other thread exists (threads then
// register the same names again, which changes nothing), so lookups need no
// lock. The table is tiny; pointer comparisons usually hit first.
struct TypeEntry {
    const char* name;
    const char* parent;
};
enum { MAX_TYPES = 64 };
TypeEntry g_types[MAX_TYPES];
volatile int g_typeCount = 0;
plat::Mutex& typeMutex() {
    static plat::Mutex m;
    return m;
}

const char* typeParent(const char* t) {
    int n = g_typeCount;
    for (int i = 0; i < n; ++i)
        if (g_types[i].name == t) return g_types[i].parent;
    for (int i = 0; i < n; ++i)
        if (strcmp(g_types[i].name, t) == 0) return g_types[i].parent;
    return NULL;
}

const char* kObjectsKey = "b360.objects";

int proxyGC(lua_State* L) {
    Proxy* p = (Proxy*)lua_touserdata(L, 1);
    if (p && p->obj) {
        p->obj->release();
        p->obj = NULL;
    }
    return 0;
}

int proxyToString(lua_State* L) {
    Proxy* p = (Proxy*)lua_touserdata(L, 1);
    lua_pushfstring(L, "%s: %p", p ? p->type : "?", p ? (void*)p->obj : NULL);
    return 1;
}

int proxyType(lua_State* L) {
    Proxy* p = (Proxy*)lua_touserdata(L, 1);
    lua_pushstring(L, p ? p->type : "Object");
    return 1;
}

int proxyTypeOf(lua_State* L) {
    Proxy* p = (Proxy*)lua_touserdata(L, 1);
    const char* name = luaL_checkstring(L, 2);
    lua_pushboolean(L, p && luax_istype(p->type, name));
    return 1;
}

int proxyRelease(lua_State* L) {
    Proxy* p = (Proxy*)lua_touserdata(L, 1);
    bool had = p && p->obj;
    if (had) {
        // Drop from the identity cache too.
        lua_getfield(L, LUA_REGISTRYINDEX, kObjectsKey);
        lua_pushlightuserdata(L, p->obj);
        lua_pushnil(L);
        lua_rawset(L, -3);
        lua_pop(L, 1);
        p->obj->release();
        p->obj = NULL;
    }
    lua_pushboolean(L, had);
    return 1;
}

int proxyEq(lua_State* L) {
    Proxy* a = (Proxy*)lua_touserdata(L, 1);
    Proxy* b = (Proxy*)lua_touserdata(L, 2);
    lua_pushboolean(L, a && b && a->obj == b->obj);
    return 1;
}

}  // namespace

bool luax_istype(const char* type, const char* base) {
    if (!type || !base) return false;
    int guard = 0;
    for (const char* t = type; t && guard++ < 16; t = typeParent(t))
        if (t == base || strcmp(t, base) == 0) return true;
    return strcmp(base, "Object") == 0;
}

void luax_registertype(lua_State* L, const TypeDef& def) {
    {
        plat::Lock lock(typeMutex());
        bool known = false;
        for (int i = 0; i < g_typeCount && !known; ++i) known = strcmp(g_types[i].name, def.name) == 0;
        if (!known && g_typeCount < MAX_TYPES) {
            g_types[g_typeCount].name = def.name;
            g_types[g_typeCount].parent = def.parent;
            ++g_typeCount;
        }
    }
    std::string key = std::string("b360.type.") + def.name;
    luaL_newmetatable(L, key.c_str());
    // __index table with methods of this type and all parents. Parents are
    // registered first so we can copy their method tables.
    lua_newtable(L);
    if (def.parent) {
        std::string pkey = std::string("b360.type.") + def.parent;
        luaL_getmetatable(L, pkey.c_str());
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "__index");
            if (lua_istable(L, -1)) {
                lua_pushnil(L);
                while (lua_next(L, -2)) {
                    lua_pushvalue(L, -2);
                    lua_insert(L, -2);
                    lua_settable(L, -6);
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pushcfunction(L, proxyType);
    lua_setfield(L, -2, "type");
    lua_pushcfunction(L, proxyTypeOf);
    lua_setfield(L, -2, "typeOf");
    lua_pushcfunction(L, proxyRelease);
    lua_setfield(L, -2, "release");
    if (def.methods) {
        for (const luaL_Reg* r = def.methods; r->name; ++r) {
            lua_pushcfunction(L, r->func);
            lua_setfield(L, -2, r->name);
        }
    }
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, proxyGC);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, proxyToString);
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, proxyEq);
    lua_setfield(L, -2, "__eq");
    lua_pushstring(L, def.name);
    lua_setfield(L, -2, "__typename");
    lua_pop(L, 1);
}

void luax_pushobject(lua_State* L, const char* type, Object* obj) {
    if (!obj) {
        lua_pushnil(L);
        return;
    }
    // Identity cache (weak values).
    lua_getfield(L, LUA_REGISTRYINDEX, kObjectsKey);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_newtable(L);
        lua_pushstring(L, "v");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, kObjectsKey);
    }
    lua_pushlightuserdata(L, obj);
    lua_rawget(L, -2);
    if (lua_isuserdata(L, -1)) {
        Proxy* p = (Proxy*)lua_touserdata(L, -1);
        if (p->obj == obj) {
            lua_remove(L, -2);
            return;
        }
    }
    lua_pop(L, 1);

    Proxy* p = (Proxy*)lua_newuserdata(L, sizeof(Proxy));
    p->magic = kProxyMagic;
    p->obj = obj;
    p->type = type;
    obj->retain();
    std::string key = std::string("b360.type.") + type;
    luaL_getmetatable(L, key.c_str());
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "internal error: type %s not registered", type);
    }
    lua_setmetatable(L, -2);

    lua_pushlightuserdata(L, obj);
    lua_pushvalue(L, -2);
    lua_rawset(L, -4);
    lua_remove(L, -2);
}

const char* luax_objecttype(lua_State* L, int idx) {
    // Only our proxies are full userdata of exactly this size with the magic
    // number (no metatable lookups: this runs for every draw call).
    if (lua_type(L, idx) != LUA_TUSERDATA || lua_objlen(L, idx) != sizeof(Proxy)) return NULL;
    Proxy* p = (Proxy*)lua_touserdata(L, idx);
    return p && p->magic == kProxyMagic ? p->type : NULL;
}

Object* luax_toobject(lua_State* L, int idx, const char* type) {
    const char* t = luax_objecttype(L, idx);
    if (!t || !luax_istype(t, type)) return NULL;
    Proxy* p = (Proxy*)lua_touserdata(L, idx);
    return p->obj;
}

Object* luax_checkobject(lua_State* L, int idx, const char* type) {
    Object* o = luax_toobject(L, idx, type);
    if (!o) {
        const char* t = luax_objecttype(L, idx);
        if (t && !luax_istype(t, type)) {
            luaL_error(L, "bad argument #%d (%s expected, got %s)", idx, type, t);
        } else if (t) {
            luaL_error(L, "Cannot use object after it has been released.");
        } else {
            luaL_error(L, "bad argument #%d (%s expected, got %s)", idx, type, luaL_typename(L, idx));
        }
    }
    return o;
}

void luax_registermodule(lua_State* L, const char* name, const luaL_Reg* funcs) {
    lua_getglobal(L, "love");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "love");
    }
    lua_newtable(L);
    if (funcs) {
        for (const luaL_Reg* r = funcs; r->name; ++r) {
            lua_pushcfunction(L, r->func);
            lua_setfield(L, -2, r->name);
        }
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name);   // love.<name>

    lua_getglobal(L, "package");
    lua_getfield(L, -1, "loaded");
    lua_pushvalue(L, -3);
    std::string full = std::string("love.") + name;
    lua_setfield(L, -2, full.c_str());   // package.loaded["love.<name>"]
    lua_pop(L, 4);
}

bool luax_optboolean(lua_State* L, int idx, bool def) {
    if (lua_isnoneornil(L, idx)) return def;
    return lua_toboolean(L, idx) != 0;
}

bool luax_toboolean(lua_State* L, int idx) { return lua_toboolean(L, idx) != 0; }

std::string luax_checkstring(lua_State* L, int idx) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, idx, &len);
    return std::string(s, len);
}

double luax_numberfield(lua_State* L, int idx, const char* key, double def) {
    lua_getfield(L, idx, key);
    double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

double luax_numberindex(lua_State* L, int idx, int i, double def) {
    lua_rawgeti(L, idx, i);
    double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

int luax_error(lua_State* L, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return luaL_error(L, "%s", buf);
}

}  // namespace rt
