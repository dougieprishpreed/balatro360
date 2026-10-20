// love.event and love.timer
#include "modules.h"
#include "event.h"

#include <deque>
#include <math.h>

namespace rt {

namespace event {

namespace {
struct Event {
    std::string name;
    std::vector<Variant> args;
};
plat::Mutex& queueMutex() {
    static plat::Mutex m;
    return m;
}
std::deque<Event>& queue() {
    static std::deque<Event> q;
    return q;
}
}  // namespace

void push(const std::string& name, const std::vector<Variant>& args) {
    plat::Lock lock(queueMutex());
    Event e;
    e.name = name;
    e.args = args;
    queue().push_back(e);
}

bool pop(std::string& name, std::vector<Variant>& args) {
    plat::Lock lock(queueMutex());
    if (queue().empty()) return false;
    name = queue().front().name;
    args = queue().front().args;
    queue().pop_front();
    return true;
}

void clear() {
    plat::Lock lock(queueMutex());
    queue().clear();
}

void pumpPlatform() {
    if (!plat::pump()) {
        std::vector<Variant> none;
        push("quit", none);
    }
    joystick::update();
    input::update();
}

}  // namespace event

namespace {

// ---------------------------------------------------------------- event
int pollIter(lua_State* L) {
    std::string name;
    std::vector<Variant> args;
    if (!event::pop(name, args)) return 0;
    lua_pushstring(L, name.c_str());
    for (size_t i = 0; i < args.size(); ++i) args[i].toLua(L);
    return 1 + (int)args.size();
}

int w_poll(lua_State* L) {
    lua_pushcfunction(L, pollIter);
    return 1;
}

int w_pump(lua_State* L) {
    (void)L;
    event::pumpPlatform();
    return 0;
}

int w_push(lua_State* L) {
    std::string name = luax_checkstring(L, 1);
    std::vector<Variant> args;
    for (int i = 2; i <= lua_gettop(L) && i <= 7; ++i) {
        Variant v;
        Variant::fromLua(L, i, v);
        args.push_back(v);
    }
    event::push(name, args);
    lua_pushboolean(L, 1);
    return 1;
}

int w_quit(lua_State* L) {
    std::vector<Variant> args;
    if (!lua_isnoneornil(L, 1)) {
        Variant v;
        Variant::fromLua(L, 1, v);
        args.push_back(v);
    }
    event::push("quit", args);
    lua_pushboolean(L, 1);
    return 1;
}

int w_clear(lua_State* L) {
    (void)L;
    event::clear();
    return 0;
}

int w_wait(lua_State* L) {
    for (;;) {
        std::string name;
        std::vector<Variant> args;
        if (event::pop(name, args)) {
            lua_pushstring(L, name.c_str());
            for (size_t i = 0; i < args.size(); ++i) args[i].toLua(L);
            return 1 + (int)args.size();
        }
        event::pumpPlatform();
        plat::sleep(0.005);
    }
}

const luaL_Reg kEventFuncs[] = {
    {"poll", w_poll}, {"pump", w_pump}, {"push", w_push}, {"quit", w_quit},
    {"clear", w_clear}, {"wait", w_wait}, {NULL, NULL}};

// ---------------------------------------------------------------- timer
struct TimerState {
    double prev;
    double dt;
    double fpsTime;
    int frames;
    int fps;
    double avgDelta;
    TimerState() : prev(0), dt(0), fpsTime(0), frames(0), fps(0), avgDelta(0) {}
};
TimerState g_timer;

int w_getTime(lua_State* L) {
    lua_pushnumber(L, plat::getTime());
    return 1;
}

int w_step(lua_State* L) {
    double now = plat::getTime();
    if (g_timer.prev == 0 && g_timer.fpsTime == 0) {
        g_timer.prev = now;
        g_timer.fpsTime = now;
    }
    g_timer.dt = now - g_timer.prev;
    g_timer.prev = now;
    ++g_timer.frames;
    double since = now - g_timer.fpsTime;
    if (since >= 1.0) {
        g_timer.fps = (int)floor(g_timer.frames / since + 0.5);
        g_timer.avgDelta = since / g_timer.frames;
        g_timer.fpsTime = now;
        g_timer.frames = 0;
    }
    lua_pushnumber(L, g_timer.dt);
    return 1;
}

int w_getDelta(lua_State* L) {
    lua_pushnumber(L, g_timer.dt);
    return 1;
}

int w_getFPS(lua_State* L) {
    lua_pushinteger(L, g_timer.fps);
    return 1;
}

int w_getAverageDelta(lua_State* L) {
    lua_pushnumber(L, g_timer.avgDelta);
    return 1;
}

int w_sleep(lua_State* L) {
    plat::sleep(luaL_checknumber(L, 1));
    return 0;
}

const luaL_Reg kTimerFuncs[] = {
    {"getTime", w_getTime}, {"step", w_step}, {"getDelta", w_getDelta}, {"getFPS", w_getFPS},
    {"getAverageDelta", w_getAverageDelta}, {"sleep", w_sleep}, {NULL, NULL}};

}  // namespace

void open_event(lua_State* L) { luax_registermodule(L, "event", kEventFuncs); }
void open_timer(lua_State* L) { luax_registermodule(L, "timer", kTimerFuncs); }

}  // namespace rt
