// love.joystick, backed by plat::padPoll (XInput on the 360).
#include "modules.h"
#include "event.h"

#include <string.h>

namespace rt {

namespace {

const char* kButtonNames[plat::PAD_BUTTON_COUNT] = {
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
    "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright"};
const char* kAxisNames[plat::PAD_AXIS_COUNT] = {
    "leftx", "lefty", "rightx", "righty", "triggerleft", "triggerright"};

int buttonIndex(const char* name) {
    for (int i = 0; i < plat::PAD_BUTTON_COUNT; ++i)
        if (strcmp(name, kButtonNames[i]) == 0) return i;
    return -1;
}

int axisIndex(const char* name) {
    for (int i = 0; i < plat::PAD_AXIS_COUNT; ++i)
        if (strcmp(name, kAxisNames[i]) == 0) return i;
    return -1;
}

class Joystick : public Object {
public:
    int index;
    int instanceId;
    plat::PadState state;
    float vibL, vibR;
    double vibUntil;
    Joystick(int i) : index(i), instanceId(i + 1), vibL(0), vibR(0), vibUntil(-1) {
        memset(&state, 0, sizeof(state));
    }
};

plat::Mutex& padMutex() {
    static plat::Mutex m;
    return m;
}

Joystick* g_pads[plat::MAX_PADS] = {NULL, NULL, NULL, NULL};

Joystick* pad(int i) {
    plat::Lock lock(padMutex());
    if (!g_pads[i]) g_pads[i] = new Joystick(i);
    return g_pads[i];
}

Joystick* checkJoystick(lua_State* L, int idx) { return (Joystick*)luax_checkobject(L, idx, "Joystick"); }

int j_isGamepad(lua_State* L) {
    checkJoystick(L, 1);
    lua_pushboolean(L, 1);
    return 1;
}

int j_isConnected(lua_State* L) {
    Joystick* j = checkJoystick(L, 1);
    plat::Lock lock(padMutex());
    lua_pushboolean(L, j->state.connected);
    return 1;
}

int j_getGamepadAxis(lua_State* L) {
    Joystick* j = checkJoystick(L, 1);
    int a = axisIndex(luaL_checkstring(L, 2));
    plat::Lock lock(padMutex());
    lua_pushnumber(L, a >= 0 ? j->state.axes[a] : 0.0);
    return 1;
}

plat::PadState snapshot(Joystick* j) {
    plat::Lock lock(padMutex());
    return j->state;
}

int j_isGamepadDown(lua_State* L) {
    Joystick* j = checkJoystick(L, 1);
    plat::PadState st = snapshot(j);   // no Lua calls while holding the lock
    int n = lua_gettop(L);
    for (int i = 2; i <= n; ++i) {
        int b;
        if (lua_istable(L, i)) {
            int len = (int)lua_objlen(L, i);
            for (int k = 1; k <= len; ++k) {
                lua_rawgeti(L, i, k);
                b = buttonIndex(luaL_checkstring(L, -1));
                lua_pop(L, 1);
                if (b >= 0 && st.buttons[b]) {
                    lua_pushboolean(L, 1);
                    return 1;
                }
            }
            continue;
        }
        b = buttonIndex(luaL_checkstring(L, i));
        if (b >= 0 && st.buttons[b]) {
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

int j_isDown(lua_State* L) {
    Joystick* j = checkJoystick(L, 1);
    plat::PadState st = snapshot(j);
    for (int i = 2; i <= lua_gettop(L); ++i) {
        int b = (int)luaL_checknumber(L, i) - 1;
        if (b >= 0 && b < plat::PAD_BUTTON_COUNT && st.buttons[b]) {
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

int j_getAxis(lua_State* L) {
    Joystick* j = checkJoystick(L, 1);
    int a = (int)luaL_checknumber(L, 2) - 1;
    plat::Lock lock(padMutex());
    lua_pushnumber(L, (a >= 0 && a < plat::PAD_AXIS_COUNT) ? j->state.axes[a] : 0.0);
    return 1;
}

int j_getAxes(lua_State* L) {
    Joystick* j = checkJoystick(L, 1);
    plat::Lock lock(padMutex());
    for (int a = 0; a < plat::PAD_AXIS_COUNT; ++a) lua_pushnumber(L, j->state.axes[a]);
    return plat::PAD_AXIS_COUNT;
}

int j_getAxisCount(lua_State* L) {
    checkJoystick(L, 1);
    lua_pushinteger(L, plat::PAD_AXIS_COUNT);
    return 1;
}

int j_getButtonCount(lua_State* L) {
    checkJoystick(L, 1);
    lua_pushinteger(L, plat::PAD_BUTTON_COUNT);
    return 1;
}

int j_getHatCount(lua_State* L) {
    checkJoystick(L, 1);
    lua_pushinteger(L, 0);
    return 1;
}

int j_getName(lua_State* L) {
    checkJoystick(L, 1);
    lua_pushstring(L, "Xbox 360 Controller");
    return 1;
}

int j_getGUID(lua_State* L) {
    checkJoystick(L, 1);
    // SDL GUID of a wired Xbox 360 controller (vendor 045e, product 028e).
    lua_pushstring(L, "030000005e0400008e02000014010000");
    return 1;
}

int j_getID(lua_State* L) {
    Joystick* j = checkJoystick(L, 1);
    lua_pushinteger(L, j->index + 1);
    lua_pushinteger(L, j->instanceId);
    return 2;
}

int j_getDeviceInfo(lua_State* L) {
    checkJoystick(L, 1);
    lua_pushinteger(L, 0x045e);
    lua_pushinteger(L, 0x028e);
    lua_pushinteger(L, 0x0114);
    return 3;
}

int j_getGamepadType(lua_State* L) {
    checkJoystick(L, 1);
    lua_pushstring(L, "xbox360");
    return 1;
}

int j_setVibration(lua_State* L) {
    Joystick* j = checkJoystick(L, 1);
    float l = (float)luaL_optnumber(L, 2, 0.0);
    float r = (float)luaL_optnumber(L, 3, l);
    double dur = luaL_optnumber(L, 4, -1.0);
    if (l < 0) l = 0;
    if (l > 1) l = 1;
    if (r < 0) r = 0;
    if (r > 1) r = 1;
    {
        plat::Lock lock(padMutex());
        j->vibL = l;
        j->vibR = r;
        j->vibUntil = dur >= 0 ? plat::getTime() + dur : -1;
    }
    plat::padVibrate(j->index, l, r);
    lua_pushboolean(L, 1);
    return 1;
}

int j_getVibration(lua_State* L) {
    Joystick* j = checkJoystick(L, 1);
    lua_pushnumber(L, j->vibL);
    lua_pushnumber(L, j->vibR);
    return 2;
}

int j_isVibrationSupported(lua_State* L) {
    checkJoystick(L, 1);
    lua_pushboolean(L, 1);
    return 1;
}

const char* kMappingString =
    "030000005e0400008e02000014010000,Xbox 360 Controller,a:b0,b:b1,back:b6,dpdown:h0.4,"
    "dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a2,"
    "leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,"
    "start:b7,x:b2,y:b3,";

int j_getGamepadMapping(lua_State* L) {
    checkJoystick(L, 1);
    lua_pushnil(L);
    return 1;
}

int j_getGamepadMappingString(lua_State* L) {
    checkJoystick(L, 1);
    lua_pushstring(L, kMappingString);
    return 1;
}

const luaL_Reg kJoystickMethods[] = {
    {"isGamepad", j_isGamepad}, {"isConnected", j_isConnected}, {"getGamepadAxis", j_getGamepadAxis},
    {"isGamepadDown", j_isGamepadDown}, {"isDown", j_isDown}, {"getAxis", j_getAxis},
    {"getAxes", j_getAxes}, {"getAxisCount", j_getAxisCount}, {"getButtonCount", j_getButtonCount},
    {"getHatCount", j_getHatCount}, {"getName", j_getName}, {"getGUID", j_getGUID},
    {"getID", j_getID}, {"getDeviceInfo", j_getDeviceInfo}, {"getGamepadType", j_getGamepadType},
    {"setVibration", j_setVibration}, {"getVibration", j_getVibration},
    {"isVibrationSupported", j_isVibrationSupported}, {"getGamepadMapping", j_getGamepadMapping},
    {"getGamepadMappingString", j_getGamepadMappingString},
    {NULL, NULL}};

int w_getJoysticks(lua_State* L) {
    lua_newtable(L);
    int n = 0;
    for (int i = 0; i < plat::MAX_PADS; ++i) {
        Joystick* j = pad(i);
        bool c;
        {
            plat::Lock lock(padMutex());
            c = j->state.connected;
        }
        if (!c) continue;
        luax_pushobject(L, "Joystick", j);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int w_getJoystickCount(lua_State* L) {
    int n = 0;
    for (int i = 0; i < plat::MAX_PADS; ++i) {
        plat::Lock lock(padMutex());
        if (g_pads[i] && g_pads[i]->state.connected) ++n;
    }
    lua_pushinteger(L, n);
    return 1;
}

int w_true(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}

int w_nil(lua_State* L) {
    lua_pushnil(L);
    return 1;
}

int w_getGamepadMappingString(lua_State* L) {
    lua_pushstring(L, kMappingString);
    return 1;
}

const luaL_Reg kFuncs[] = {
    {"getJoysticks", w_getJoysticks}, {"getJoystickCount", w_getJoystickCount},
    {"loadGamepadMappings", w_true}, {"saveGamepadMappings", w_nil},
    {"setGamepadMapping", w_true}, {"getGamepadMappingString", w_getGamepadMappingString}, {NULL, NULL}};

}  // namespace

namespace joystick {

void update() {
    plat::PadState now[plat::MAX_PADS];
    plat::padPoll(now);
    double t = plat::getTime();
    for (int i = 0; i < plat::MAX_PADS; ++i) {
        Joystick* j = pad(i);
        plat::PadState prev;
        {
            plat::Lock lock(padMutex());
            prev = j->state;
            j->state = now[i];
            if (j->vibUntil >= 0 && t >= j->vibUntil) {
                j->vibUntil = -1;
                j->vibL = j->vibR = 0;
                plat::padVibrate(i, 0, 0);
            }
        }
        Variant jv = Variant::fromObject("Joystick", j);
        if (now[i].connected != prev.connected) {
            std::vector<Variant> a;
            a.push_back(jv);
            event::push(now[i].connected ? "joystickadded" : "joystickremoved", a);
            if (!now[i].connected) continue;
        }
        if (!now[i].connected) continue;
        for (int b = 0; b < plat::PAD_BUTTON_COUNT; ++b) {
            if (now[i].buttons[b] == prev.buttons[b]) continue;
            bool down = now[i].buttons[b];
            std::vector<Variant> a;
            a.push_back(jv);
            a.push_back(Variant::fromNumber(b + 1));
            event::push(down ? "joystickpressed" : "joystickreleased", a);
            std::vector<Variant> g;
            g.push_back(jv);
            g.push_back(Variant::fromString(kButtonNames[b]));
            event::push(down ? "gamepadpressed" : "gamepadreleased", g);
        }
        for (int x = 0; x < plat::PAD_AXIS_COUNT; ++x) {
            if (now[i].axes[x] == prev.axes[x]) continue;
            std::vector<Variant> a;
            a.push_back(jv);
            a.push_back(Variant::fromNumber(x + 1));
            a.push_back(Variant::fromNumber(now[i].axes[x]));
            event::push("joystickaxis", a);
            std::vector<Variant> g;
            g.push_back(jv);
            g.push_back(Variant::fromString(kAxisNames[x]));
            g.push_back(Variant::fromNumber(now[i].axes[x]));
            event::push("gamepadaxis", g);
        }
    }
}

}  // namespace joystick

void open_joystick(lua_State* L) {
    TypeDef td = {"Joystick", NULL, kJoystickMethods};
    luax_registertype(L, td);
    luax_registermodule(L, "joystick", kFuncs);
}

}  // namespace rt
