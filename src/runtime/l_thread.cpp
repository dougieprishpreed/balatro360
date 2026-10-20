// love.thread: OS threads running independent Lua states, communicating
// through named or anonymous Channels.
#include "modules.h"
#include "variant.h"
#include "event.h"

#include <deque>
#include <map>

namespace rt {

namespace {

class Channel : public Object {
public:
    Channel() : sent(0), received(0) {}

    u64 push(const Variant& v) {
        plat::Lock lock(mu);
        queue.push_back(v);
        ++sent;
        cond.broadcast();
        return sent;
    }

    bool pop(Variant& out) {
        plat::Lock lock(mu);
        if (queue.empty()) return false;
        out = queue.front();
        queue.pop_front();
        ++received;
        cond.broadcast();
        return true;
    }

    // timeout < 0: wait forever.
    bool demand(Variant& out, double timeout) {
        plat::Lock lock(mu);
        double start = plat::getTime();
        while (queue.empty()) {
            if (timeout < 0) {
                cond.wait(mu);
            } else {
                double left = timeout - (plat::getTime() - start);
                if (left <= 0) return false;
                cond.waitTimeout(mu, left);
            }
        }
        out = queue.front();
        queue.pop_front();
        ++received;
        cond.broadcast();
        return true;
    }

    // Push and wait until the value was received.
    bool supply(const Variant& v, double timeout) {
        plat::Lock lock(mu);
        queue.push_back(v);
        u64 id = ++sent;
        cond.broadcast();
        double start = plat::getTime();
        while (received < id) {
            if (timeout < 0) {
                cond.wait(mu);
            } else {
                double left = timeout - (plat::getTime() - start);
                if (left <= 0) return false;
                cond.waitTimeout(mu, left);
            }
        }
        return true;
    }

    bool peek(Variant& out) {
        plat::Lock lock(mu);
        if (queue.empty()) return false;
        out = queue.front();
        return true;
    }

    int count() {
        plat::Lock lock(mu);
        return (int)queue.size();
    }

    bool hasRead(u64 id) {
        plat::Lock lock(mu);
        return received >= id;
    }

    void clear() {
        plat::Lock lock(mu);
        received += queue.size();
        queue.clear();
        cond.broadcast();
    }

    plat::Mutex mu;

private:
    plat::Cond cond;
    std::deque<Variant> queue;
    u64 sent, received;
};

plat::Mutex& namedMutex() {
    static plat::Mutex m;
    return m;
}
std::map<std::string, Channel*>& namedChannels() {
    static std::map<std::string, Channel*> m;
    return m;
}

Channel* getNamedChannel(const std::string& name) {
    plat::Lock lock(namedMutex());
    std::map<std::string, Channel*>& m = namedChannels();
    std::map<std::string, Channel*>::iterator it = m.find(name);
    if (it != m.end()) return it->second;
    Channel* c = new Channel;   // named channels live forever (refcount 1)
    m[name] = c;
    return c;
}

class LuaThread : public Object {
public:
    LuaThread(const std::string& code, const std::string& name)
        : code(code), name(name), th(NULL), running(false), started(false) {}

    ~LuaThread() {
        if (th) plat::threadJoin(th);
    }

    bool start(const std::vector<Variant>& a) {
        plat::Lock lock(mu);
        if (running) return false;
        if (th) {
            plat::threadJoin(th);
            th = NULL;
        }
        args = a;
        error.clear();
        running = true;
        started = true;
        retain();   // released when the thread exits
        static int core = 2;
        th = plat::threadCreate(entry, this, core);
        core = core >= 5 ? 2 : core + 1;
        if (!th) {
            running = false;
            release();
            return false;
        }
        return true;
    }

    void wait() {
        plat::Thread* t;
        {
            plat::Lock lock(mu);
            t = th;
            th = NULL;
        }
        if (t) plat::threadJoin(t);
    }

    bool isRunning() {
        plat::Lock lock(mu);
        return running;
    }

    std::string getError() {
        plat::Lock lock(mu);
        return error;
    }

private:
    static void entry(void* p) {
        LuaThread* self = (LuaThread*)p;
        self->run();
        {
            plat::Lock lock(self->mu);
            self->running = false;
        }
        self->release();
    }

    void run() {
        lua_State* L = newLuaState(false);
        lua_getglobal(L, "debug");
        lua_getfield(L, -1, "traceback");
        lua_remove(L, -2);
        int tb = lua_gettop(L);
        std::string chunk = "@" + name;
        int status = luaL_loadbuffer(L, code.data(), code.size(), chunk.c_str());
        if (status == 0) {
            for (size_t i = 0; i < args.size(); ++i) args[i].toLua(L);
            status = lua_pcall(L, (int)args.size(), 0, tb);
        }
        if (status != 0) {
            const char* msg = lua_tostring(L, -1);
            std::string err = msg ? msg : "unknown error";
            plat::log("[thread %s] error: %s", name.c_str(), err.c_str());
            {
                plat::Lock lock(mu);
                error = err;
            }
            std::vector<Variant> ev;
            ev.push_back(Variant::fromObject("Thread", this));
            ev.push_back(Variant::fromString(err));
            event::push("threaderror", ev);
        }
        lua_close(L);
    }

    std::string code;
    std::string name;
    plat::Thread* th;
    plat::Mutex mu;
    bool running;
    bool started;
    std::string error;
    std::vector<Variant> args;
};

// ------------------------------------------------------------ Lua wrappers
Channel* checkChannel(lua_State* L, int idx) { return (Channel*)luax_checkobject(L, idx, "Channel"); }
LuaThread* checkThread(lua_State* L, int idx) { return (LuaThread*)luax_checkobject(L, idx, "Thread"); }

Variant checkVariant(lua_State* L, int idx) {
    Variant v;
    if (!Variant::fromLua(L, idx, v)) luaL_argerror(L, idx, "boolean, number, string, love type, or flat table expected");
    return v;
}

int c_push(lua_State* L) {
    Channel* c = checkChannel(L, 1);
    Variant v = checkVariant(L, 2);
    lua_pushnumber(L, (lua_Number)c->push(v));
    return 1;
}

int c_supply(lua_State* L) {
    Channel* c = checkChannel(L, 1);
    Variant v = checkVariant(L, 2);
    double timeout = lua_isnumber(L, 3) ? lua_tonumber(L, 3) : -1.0;
    lua_pushboolean(L, c->supply(v, timeout));
    return 1;
}

int c_pop(lua_State* L) {
    Channel* c = checkChannel(L, 1);
    Variant v;
    if (c->pop(v)) v.toLua(L);
    else lua_pushnil(L);
    return 1;
}

int c_demand(lua_State* L) {
    Channel* c = checkChannel(L, 1);
    double timeout = lua_isnumber(L, 2) ? lua_tonumber(L, 2) : -1.0;
    Variant v;
    if (c->demand(v, timeout)) v.toLua(L);
    else lua_pushnil(L);
    return 1;
}

int c_peek(lua_State* L) {
    Channel* c = checkChannel(L, 1);
    Variant v;
    if (c->peek(v)) v.toLua(L);
    else lua_pushnil(L);
    return 1;
}

int c_getCount(lua_State* L) {
    lua_pushinteger(L, checkChannel(L, 1)->count());
    return 1;
}

int c_hasRead(lua_State* L) {
    Channel* c = checkChannel(L, 1);
    lua_pushboolean(L, c->hasRead((u64)luaL_checknumber(L, 2)));
    return 1;
}

int c_clear(lua_State* L) {
    checkChannel(L, 1)->clear();
    return 0;
}

int c_performAtomic(lua_State* L) {
    Channel* c = checkChannel(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    int n = lua_gettop(L);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 1);
    for (int i = 3; i <= n; ++i) lua_pushvalue(L, i);
    int status;
    {
        // Recursive mutex: the function may call other methods of this
        // channel. pcall so the lock is always released before an error
        // propagates (LuaJIT unwinds errors with longjmp).
        plat::Lock lock(c->mu);
        status = lua_pcall(L, n - 1, LUA_MULTRET, 0);
    }
    if (status != 0) return lua_error(L);
    return lua_gettop(L) - n;
}

int t_start(lua_State* L) {
    LuaThread* t = checkThread(L, 1);
    std::vector<Variant> args;
    for (int i = 2; i <= lua_gettop(L); ++i) args.push_back(checkVariant(L, i));
    t->start(args);
    return 0;
}

int t_wait(lua_State* L) {
    checkThread(L, 1)->wait();
    return 0;
}

int t_isRunning(lua_State* L) {
    lua_pushboolean(L, checkThread(L, 1)->isRunning());
    return 1;
}

int t_getError(lua_State* L) {
    std::string e = checkThread(L, 1)->getError();
    if (e.empty()) lua_pushnil(L);
    else lua_pushstring(L, e.c_str());
    return 1;
}

int w_newThread(lua_State* L) {
    std::string arg = luax_checkstring(L, 1);
    std::string code, name;
    bool isCode = arg.find('\n') != std::string::npos || arg.size() > 1024;
    if (!isCode) {
        std::vector<u8> data;
        std::string filedata;
        lua_getglobal(L, "love");
        lua_getfield(L, -1, "filesystem");
        lua_getfield(L, -1, "read");
        lua_pushstring(L, arg.c_str());
        lua_call(L, 1, 1);
        if (lua_isstring(L, -1)) {
            size_t len;
            const char* s = lua_tolstring(L, -1, &len);
            code.assign(s, len);
            name = arg;
        } else {
            isCode = true;
        }
        lua_pop(L, 3);
    }
    if (isCode) {
        code = arg;
        name = "thread code";
    }
    LuaThread* t = new LuaThread(code, name);
    luax_pushobject(L, "Thread", t);
    t->release();
    return 1;
}

int w_newChannel(lua_State* L) {
    Channel* c = new Channel;
    luax_pushobject(L, "Channel", c);
    c->release();
    return 1;
}

int w_getChannel(lua_State* L) {
    luax_pushobject(L, "Channel", getNamedChannel(luax_checkstring(L, 1)));
    return 1;
}

const luaL_Reg kChannelMethods[] = {
    {"push", c_push},       {"supply", c_supply}, {"pop", c_pop},
    {"demand", c_demand},   {"peek", c_peek},     {"getCount", c_getCount},
    {"hasRead", c_hasRead}, {"clear", c_clear},   {"performAtomic", c_performAtomic},
    {NULL, NULL}};

const luaL_Reg kThreadMethods[] = {
    {"start", t_start}, {"wait", t_wait}, {"isRunning", t_isRunning}, {"getError", t_getError}, {NULL, NULL}};

const luaL_Reg kFuncs[] = {
    {"newThread", w_newThread}, {"newChannel", w_newChannel}, {"getChannel", w_getChannel}, {NULL, NULL}};

}  // namespace

void open_thread(lua_State* L) {
    TypeDef ch = {"Channel", NULL, kChannelMethods};
    TypeDef th = {"Thread", NULL, kThreadMethods};
    luax_registertype(L, ch);
    luax_registertype(L, th);
    luax_registermodule(L, "thread", kFuncs);
}

}  // namespace rt
