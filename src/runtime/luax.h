// Small helpers for binding C++ objects to Lua 5.1.
#ifndef B360_LUAX_H
#define B360_LUAX_H

// Lua is compiled as C++ (errors are C++ exceptions, so destructors run
// when a Lua error unwinds through our code). Do not wrap in extern "C".
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "object.h"
#include <string>

namespace rt {

// Type names form a simple hierarchy (e.g. "Canvas" -> "Texture" ->
// "Drawable" -> "Object") used by type()/typeOf() and luax_checkobject.
struct TypeDef {
    const char* name;
    const char* parent;   // NULL for root types (implicitly "Object")
    const luaL_Reg* methods;
};

// Register a type's metatable in the registry. Safe to call per lua_State.
void luax_registertype(lua_State* L, const TypeDef& def);

// Push obj as userdata of the given type. The same object always maps to
// the same userdata within one lua_State (so == works on e.g. joysticks).
// Retains the object. Pushes nil if obj is NULL.
void luax_pushobject(lua_State* L, const char* type, Object* obj);

// Returns the object at idx if it is (a subtype of) type, else NULL.
Object* luax_toobject(lua_State* L, int idx, const char* type);
// Same, but raises a Lua error on mismatch.
Object* luax_checkobject(lua_State* L, int idx, const char* type);
// Name of the concrete type of the object at idx, or NULL.
const char* luax_objecttype(lua_State* L, int idx);
bool luax_istype(const char* type, const char* base);

// Register a module table as love.<name> and package.preload["love.<name>"].
void luax_registermodule(lua_State* L, const char* name, const luaL_Reg* funcs);

bool luax_optboolean(lua_State* L, int idx, bool def);
bool luax_toboolean(lua_State* L, int idx);
std::string luax_checkstring(lua_State* L, int idx);

// Read a number field from the table at idx (default if missing).
double luax_numberfield(lua_State* L, int idx, const char* key, double def);
// Read t[i] (1-based) from the table at idx.
double luax_numberindex(lua_State* L, int idx, int i, double def);

int luax_error(lua_State* L, const char* fmt, ...);

}  // namespace rt

#endif
