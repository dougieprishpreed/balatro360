// Sampling Lua profiler for testing on hardware (b360_config.lua:
// B360_PROFILE = true).
//
// A helper thread arms a one-shot count hook about once per millisecond
// (lua_sethook is meant to be callable asynchronously, which is how lua.c
// handles Ctrl+C). The hook runs on the game thread at its next Lua
// instruction, disarms itself and records where it is, so the game runs at
// full speed between samples. Time spent inside C functions
// (drawing, presenting) is charged to the Lua code that called them.
//
// Every report interval the hottest functions (self and inclusive) and lines
// are written to the log.
#include "modules.h"
#include "../platform/platform.h"

#include <algorithm>
#include <map>
#include <string.h>

namespace rt {

namespace {

struct FuncKey {
    const char* source;   // interned Lua string, stable while the function exists
    int line;
    bool operator<(const FuncKey& o) const {
        return source != o.source ? source < o.source : line < o.line;
    }
};

struct FuncStats {
    std::string label;
    int self;
    int incl;
    int stamp;   // last sample that counted this function as inclusive
    FuncStats() : self(0), incl(0), stamp(-1) {}
};

lua_State* g_L = NULL;
plat::Thread* g_thread = NULL;
volatile i32 g_run = 0;
volatile i32 g_taken = 0;
double g_interval = 0.001;
double g_reportEvery = 30.0;
double g_windowStart = 0;
int g_samples = 0;
std::map<FuncKey, FuncStats> g_funcs;
std::map<FuncKey, int> g_lines;

std::string intStr(int v) {
    char buf[16];
    char* p = buf + sizeof(buf);
    *--p = 0;
    bool neg = v < 0;
    unsigned u = neg ? 0u - (unsigned)v : (unsigned)v;
    do {
        *--p = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg) *--p = '-';
    return p;
}

std::string shortSource(const char* src) {
    std::string s = src ? src : "?";
    if (!s.empty() && (s[0] == '@' || s[0] == '=')) s = s.substr(1);
    return s;
}

std::string funcLabel(const char* name, const char* src, int line) {
    return std::string(name ? name : "?") + " (" + shortSource(src) + ":" + intStr(line) + ")";
}

void hook(lua_State* L, lua_Debug* ar) {
    (void)ar;
    // Disarm here, on the game thread: changing hooks from the helper thread
    // while a hook is running races with the VM's own hook bookkeeping.
    lua_sethook(L, NULL, 0, 0);
    lua_Debug d;
    if (!lua_getstack(L, 0, &d)) {
        g_taken = 1;
        return;
    }
    lua_getinfo(L, "Sln", &d);
    ++g_samples;
    FuncKey k;
    k.source = d.source;
    k.line = d.linedefined;
    FuncStats& fs = g_funcs[k];
    if (fs.label.empty()) fs.label = funcLabel(d.name, d.source, d.linedefined);
    ++fs.self;
    FuncKey lk;
    lk.source = d.source;
    lk.line = d.currentline;
    ++g_lines[lk];
    // Inclusive: every distinct function on the stack, once per sample.
    for (int level = 0; level < 64; ++level) {
        lua_Debug c;
        if (!lua_getstack(L, level, &c)) break;
        if (level > 0) lua_getinfo(L, "Sn", &c);
        else c = d;
        if (c.what && strcmp(c.what, "C") == 0) continue;
        FuncKey ck;
        ck.source = c.source;
        ck.line = c.linedefined;
        FuncStats& cs = g_funcs[ck];
        if (cs.label.empty()) cs.label = funcLabel(c.name, c.source, c.linedefined);
        if (cs.stamp != g_samples) {
            cs.stamp = g_samples;
            ++cs.incl;
        }
    }
    g_taken = 1;
}

void sampler(void*) {
    g_taken = 1;
    while (g_run) {
        // Wait for the previous sample to finish, then one interval (with a
        // busy wait where plat::sleep is a no-op, i.e. fixed-step host runs).
        while (!g_taken && g_run) plat::sleep(0.0005);
        double t0 = plat::getTime();
        while (plat::getTime() - t0 < g_interval && g_run) plat::sleep(g_interval);
        if (!g_run) break;
        g_taken = 0;
        lua_sethook(g_L, hook, LUA_MASKCOUNT, 1);
    }
}

struct BySelf {
    bool operator()(const std::pair<std::string, int>& a, const std::pair<std::string, int>& b) const {
        return a.second > b.second;
    }
};

void printTop(const char* title, std::vector<std::pair<std::string, int> >& v, int n) {
    std::sort(v.begin(), v.end(), BySelf());
    plat::log("[profile] %s", title);
    for (int i = 0; i < n && i < (int)v.size(); ++i)
        plat::log("[profile] %5.1f%%  %s", 100.0 * v[i].second / g_samples, v[i].first.c_str());
}

}  // namespace

void profilerStart(lua_State* L, double intervalMs, double reportSeconds) {
    if (g_thread) return;
    g_L = L;
    g_interval = intervalMs > 0.1 ? intervalMs / 1000.0 : 0.001;
    g_reportEvery = reportSeconds > 1 ? reportSeconds : 30.0;
    g_windowStart = plat::getTime();
    g_run = 1;
    g_thread = plat::threadCreate(sampler, NULL, 5);
    plat::log("[profile] sampling Lua every %.1f ms, report every %.0f s", g_interval * 1000.0, g_reportEvery);
}

// Called once per frame on the game thread (no sample is being taken then
// unless the hook is armed, and the hook also runs on this thread).
void profilerFrame() {
    if (!g_thread) return;
    double now = plat::getTime();
    if (now - g_windowStart < g_reportEvery || g_samples == 0) return;
    plat::log("[profile] %d samples in %.1f s", g_samples, now - g_windowStart);
    std::vector<std::pair<std::string, int> > self, incl, lines;
    for (std::map<FuncKey, FuncStats>::iterator it = g_funcs.begin(); it != g_funcs.end(); ++it) {
        if (it->second.self) self.push_back(std::make_pair(it->second.label, it->second.self));
        incl.push_back(std::make_pair(it->second.label, it->second.incl));
    }
    for (std::map<FuncKey, int>::iterator it = g_lines.begin(); it != g_lines.end(); ++it) {
        lines.push_back(std::make_pair(shortSource(it->first.source) + ":" + intStr(it->first.line), it->second));
    }
    printTop("self time (functions):", self, 25);
    printTop("inclusive time (functions):", incl, 25);
    printTop("self time (lines):", lines, 15);
    g_funcs.clear();
    g_lines.clear();
    g_samples = 0;
    g_windowStart = now;
}

void profilerStop() {
    if (!g_thread) return;
    g_run = 0;
    plat::threadJoin(g_thread);
    g_thread = NULL;
    lua_sethook(g_L, NULL, 0, 0);   // in case a sample was still armed
}

}  // namespace rt
