// Thread-safe copy of a Lua value, used by love.thread channels and the
// event queue. Tables are copied deeply (cycles are cut at a depth limit).
#ifndef B360_VARIANT_H
#define B360_VARIANT_H

#include "luax.h"
#include <string>
#include <vector>

namespace rt {

class Variant {
public:
    enum Type { NIL, BOOLEAN, NUMBER, STRING, TABLE, OBJECT };

    Variant();
    Variant(const Variant& o);
    Variant& operator=(const Variant& o);
    ~Variant();

    static Variant fromNumber(double d);
    static Variant fromString(const std::string& s);
    static Variant fromBool(bool b);
    static Variant fromObject(const char* type, Object* o);

    // Returns false (and a nil variant) for unsupported values (functions,
    // coroutines, plain userdata).
    static bool fromLua(lua_State* L, int idx, Variant& out, int depth = 0);
    void toLua(lua_State* L) const;

    Type type() const { return t; }

private:
    void clear();
    void copyFrom(const Variant& o);

    Type t;
    bool b;
    double n;
    std::string s;
    std::vector<std::pair<Variant, Variant> >* table;
    Object* obj;
    const char* objType;
};

}  // namespace rt

#endif
