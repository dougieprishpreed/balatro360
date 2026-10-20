#include "variant.h"

namespace rt {

struct Variant::TableData : public Object {
    std::vector<std::pair<Variant, Variant> > items;
};

Variant::Variant() : t(NIL), b(false), n(0), table(NULL), obj(NULL), objType(NULL) {}

Variant::Variant(const Variant& o) : t(NIL), b(false), n(0), table(NULL), obj(NULL), objType(NULL) {
    copyFrom(o);
}

Variant& Variant::operator=(const Variant& o) {
    if (this != &o) {
        clear();
        copyFrom(o);
    }
    return *this;
}

Variant::~Variant() { clear(); }

void Variant::clear() {
    if (table) {
        table->release();
        table = NULL;
    }
    if (obj) {
        obj->release();
        obj = NULL;
    }
    s.clear();
    t = NIL;
}

void Variant::copyFrom(const Variant& o) {
    t = o.t;
    b = o.b;
    n = o.n;
    s = o.s;
    objType = o.objType;
    table = o.table;
    if (table) table->retain();
    obj = o.obj;
    if (obj) obj->retain();
}

Variant Variant::fromNumber(double d) {
    Variant v;
    v.t = NUMBER;
    v.n = d;
    return v;
}

Variant Variant::fromString(const std::string& str) {
    Variant v;
    v.t = STRING;
    v.s = str;
    return v;
}

Variant Variant::fromBool(bool bv) {
    Variant v;
    v.t = BOOLEAN;
    v.b = bv;
    return v;
}

Variant Variant::fromObject(const char* type, Object* o) {
    Variant v;
    if (!o) return v;
    v.t = OBJECT;
    v.obj = o;
    v.objType = type;
    o->retain();
    return v;
}

bool Variant::fromLua(lua_State* L, int idx, Variant& out, int depth) {
    out.clear();
    if (idx < 0 && idx > LUA_REGISTRYINDEX) idx = lua_gettop(L) + idx + 1;
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        return true;
    case LUA_TBOOLEAN:
        out.t = BOOLEAN;
        out.b = lua_toboolean(L, idx) != 0;
        return true;
    case LUA_TNUMBER:
        out.t = NUMBER;
        out.n = lua_tonumber(L, idx);
        return true;
    case LUA_TSTRING: {
        size_t len = 0;
        const char* str = lua_tolstring(L, idx, &len);
        out.t = STRING;
        out.s.assign(str, len);
        return true;
    }
    case LUA_TTABLE: {
        if (depth > 64 || !lua_checkstack(L, 4)) return false;
        size_t n = 0;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            ++n;
            lua_pop(L, 1);
        }
        out.t = TABLE;
        out.table = new TableData;   // refcount 1, owned by out
        std::vector<std::pair<Variant, Variant> >& items = out.table->items;
        items.reserve(n);
        // Fill entries in place: no temporaries, no copies on growth.
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            items.push_back(std::pair<Variant, Variant>());
            std::pair<Variant, Variant>& e = items.back();
            if (!fromLua(L, -2, e.first, depth + 1) || e.first.t == NIL || !fromLua(L, -1, e.second, depth + 1))
                items.pop_back();
            lua_pop(L, 1);
        }
        return true;
    }
    case LUA_TUSERDATA: {
        const char* type = luax_objecttype(L, idx);
        if (!type) return false;
        Object* o = luax_toobject(L, idx, type);
        if (!o) return false;
        out.t = OBJECT;
        out.obj = o;
        out.objType = type;
        o->retain();
        return true;
    }
    case LUA_TLIGHTUSERDATA:
    default:
        return false;
    }
}

void Variant::toLua(lua_State* L) const {
    switch (t) {
    case BOOLEAN:
        lua_pushboolean(L, b);
        break;
    case NUMBER:
        lua_pushnumber(L, n);
        break;
    case STRING:
        lua_pushlstring(L, s.data(), s.size());
        break;
    case TABLE:
        lua_checkstack(L, 4);
        lua_createtable(L, 0, (int)table->items.size());
        for (size_t i = 0; i < table->items.size(); ++i) {
            table->items[i].first.toLua(L);
            table->items[i].second.toLua(L);
            lua_rawset(L, -3);
        }
        break;
    case OBJECT:
        luax_pushobject(L, objType, obj);
        break;
    default:
        lua_pushnil(L);
        break;
    }
}

}  // namespace rt
