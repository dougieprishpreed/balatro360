// Native versions of Balatro hot paths that hooks.lua puts in place of the
// game's own, for the exact game files they were written against (see the
// hashes there). They do the game's arithmetic in its order, so they give the
// same results; the game's Lua stays in use for any other version.
#include "modules.h"

#include <math.h>

namespace rt {

namespace {

// Keys, held as the closure's upvalues so lookups don't hash the string again.
enum Key { K_CONTAINER = 1, K_CT, K_T, K_STATES, K_HOVER, K_IS, K_X, K_Y, K_W, K_H, K_R, K_BUFFER, K_COUNT };
const char* const kKeys[K_COUNT] = {NULL, "container", "CT", "T", "states", "hover", "is", "x", "y", "w", "h", "r",
                                    "COLLISION_BUFFER"};

// Pushes t[key], metamethods included; t is an absolute index.
void get(lua_State* L, int t, int key) {
    lua_pushvalue(L, lua_upvalueindex(key));
    lua_gettable(L, t);
}

double num(lua_State* L, int t, int key) {
    get(L, t, key);
    double v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

const double kPi = 3.14159265358979323846;   // math.pi

// love._b360_collidesWithPoint(node, point): Node:collides_with_point
// (engine/node.lua) with point_translate and point_rotate
// (functions/misc_functions.lua). Returns true on a hit and nothing
// otherwise, as the game's does.
int w_collidesWithPoint(lua_State* L) {
    lua_settop(L, 2);
    get(L, 1, K_CONTAINER);   // 3
    if (!lua_toboolean(L, 3)) return 0;
    get(L, 1, K_CT);          // 4: T = self.CT or self.T
    if (!lua_toboolean(L, 4)) {
        lua_pop(L, 1);
        get(L, 1, K_T);
    }
    double b = 0;   // self.states.hover.is and G.COLLISION_BUFFER or 0
    get(L, 1, K_STATES);
    get(L, lua_gettop(L), K_HOVER);
    get(L, lua_gettop(L), K_IS);
    bool hover = lua_toboolean(L, -1) != 0;
    lua_pop(L, 3);
    if (hover) {
        lua_getglobal(L, "G");
        get(L, lua_gettop(L), K_BUFFER);
        if (lua_toboolean(L, -1)) b = lua_tonumber(L, -1);
        lua_pop(L, 2);
    }
    double px = num(L, 2, K_X), py = num(L, 2, K_Y);
    if (!lua_equal(L, 3, 1)) {
        get(L, 3, K_T);   // 5: the container's T
        double cr = num(L, 5, K_R);
        if (fabs(cr) < 0.1) {
            double cw = num(L, 5, K_W), ch = num(L, 5, K_H);
            px = px + -cw / 2;
            py = py + -ch / 2;
            double c = cos(cr + kPi / 2), s = sin(cr + kPi / 2), ox = px, oy = py;
            px = -oy * c + ox * s;
            py = oy * s + ox * c;
            double cx = num(L, 5, K_X), cy = num(L, 5, K_Y);
            px = px + (cw / 2 - cx);
            py = py + (ch / 2 - cy);
        } else {
            px = px + -num(L, 5, K_X);
            py = py + -num(L, 5, K_Y);
        }
        lua_pop(L, 1);
    }
    double r = num(L, 4, K_R), x = num(L, 4, K_X), y = num(L, 4, K_Y), w = num(L, 4, K_W), h = num(L, 4, K_H);
    if (fabs(r) >= 0.1) {
        double c = cos(r + kPi / 2), s = sin(r + kPi / 2);
        double qx = px - (x + 0.5 * w), qy = py - (y + 0.5 * h);
        double tx = qy * c - qx * s, ty = qy * s + qx * c;
        px = tx + (x + 0.5 * w);
        py = ty + (y + 0.5 * h);
    }
    if (px >= x - b && py >= y - b && px <= x + w + b && py <= y + h + b) {
        lua_pushboolean(L, 1);
        return 1;
    }
    return 0;
}

}  // namespace

void open_accel(lua_State* L) {
    lua_getglobal(L, "love");
    for (int k = 1; k < K_COUNT; ++k) lua_pushstring(L, kKeys[k]);
    lua_pushcclosure(L, w_collidesWithPoint, K_COUNT - 1);
    lua_setfield(L, -2, "_b360_collidesWithPoint");
    lua_pop(L, 1);
}

}  // namespace rt
