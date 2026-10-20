// Linux host implementation of the platform layer.
//
// This build has no window: rendering goes to an offscreen OSMesa
// context and input comes from a scripted event file, which lets the whole
// runtime be exercised against the real game data in CI / cloud sessions.
//
// Environment variables:
//   B360_GAME_DIR   extracted game directory (default ./game)
//   B360_SAVE_DIR   save directory (default ./save)
//   B360_SCRIPT     input script (see docs/HOST_TESTING.md)
//   B360_FIXED_DT   if set, love.timer.step() returns this fixed dt (seconds) and
//                   love.timer.sleep() doesn't sleep, so runs are deterministic
//                   and independent of host speed. getTime() stays real time
//                   (Balatro budgets its GC work with it).
//   B360_WAV        if set, write mixed audio to this WAV file
//   B360_MAX_FRAMES quit after this many frames
#include "../platform.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include <map>

namespace plat {

namespace {

struct ScriptCmd {
    long frame;
    std::string op;
    std::string a;
    std::string b;
};

std::vector<ScriptCmd> g_script;
size_t g_scriptPos = 0;
long g_frame = 0;
long g_maxFrames = -1;
bool g_quit = false;
PadState g_pad;
std::map<long, std::vector<std::string> > g_pendingReleases;
std::vector<KeyEvent> g_keys;
std::string g_shotPath;
double g_fixedDt = 0.0;
double g_startTime = 0.0;
float g_mouseX = 0.0f, g_mouseY = 0.0f;

const char* kButtonNames[PAD_BUTTON_COUNT] = {
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
    "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright"};
const char* kAxisNames[PAD_AXIS_COUNT] = {
    "leftx", "lefty", "rightx", "righty", "triggerleft", "triggerright"};

int buttonIndex(const std::string& name) {
    for (int i = 0; i < PAD_BUTTON_COUNT; ++i)
        if (name == kButtonNames[i]) return i;
    return -1;
}

int axisIndex(const std::string& name) {
    for (int i = 0; i < PAD_AXIS_COUNT; ++i)
        if (name == kAxisNames[i]) return i;
    return -1;
}

double realTime() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void loadScript(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        log("[host] cannot open script %s", path);
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* hash = strchr(line, '#');
        if (hash) *hash = 0;
        char op[64] = {0}, a[256] = {0}, b[256] = {0};
        long frame = 0;
        int n = sscanf(line, "%ld %63s %255s %255s", &frame, op, a, b);
        if (n < 2) continue;
        ScriptCmd c;
        c.frame = frame;
        c.op = op;
        c.a = n > 2 ? a : "";
        c.b = n > 3 ? b : "";
        g_script.push_back(c);
    }
    fclose(f);
    log("[host] loaded %d script commands", (int)g_script.size());
}

void setButton(const std::string& name, bool down) {
    int idx = buttonIndex(name);
    if (idx >= 0) {
        g_pad.buttons[idx] = down;
        return;
    }
    KeyEvent k;
    k.pressed = down;
    k.key = name;
    g_keys.push_back(k);
}

void runScript() {
    std::map<long, std::vector<std::string> >::iterator it = g_pendingReleases.find(g_frame);
    if (it != g_pendingReleases.end()) {
        for (size_t i = 0; i < it->second.size(); ++i) setButton(it->second[i], false);
        g_pendingReleases.erase(it);
    }
    while (g_scriptPos < g_script.size() && g_script[g_scriptPos].frame <= g_frame) {
        const ScriptCmd& c = g_script[g_scriptPos++];
        if (c.op == "press") {
            setButton(c.a, true);
        } else if (c.op == "release") {
            setButton(c.a, false);
        } else if (c.op == "tap") {
            setButton(c.a, true);
            g_pendingReleases[g_frame + 4].push_back(c.a);
        } else if (c.op == "axis") {
            int idx = axisIndex(c.a);
            if (idx >= 0) g_pad.axes[idx] = (float)atof(c.b.c_str());
        } else if (c.op == "screenshot") {
            g_shotPath = c.a;
        } else if (c.op == "mouse") {
            g_mouseX = (float)atof(c.a.c_str());
            g_mouseY = (float)atof(c.b.c_str());
        } else if (c.op == "quit") {
            g_quit = true;
        } else if (c.op == "log") {
            log("[script] %s %s", c.a.c_str(), c.b.c_str());
        }
    }
}

}  // namespace

bool init(Config& cfg) {
    (void)cfg;
    g_startTime = realTime();
    memset(&g_pad, 0, sizeof(g_pad));
    g_pad.connected = true;
    const char* s = getenv("B360_SCRIPT");
    if (s && *s) loadScript(s);
    const char* dt = getenv("B360_FIXED_DT");
    if (dt && *dt) g_fixedDt = atof(dt);
    const char* mf = getenv("B360_MAX_FRAMES");
    if (mf && *mf) g_maxFrames = atol(mf);
    return true;
}

void shutdown() {}

const char* osName() { return "Linux"; }

bool pump() {
    // Called at most once per frame from present().
    ++g_frame;
    runScript();
    if (g_maxFrames >= 0 && g_frame >= g_maxFrames) g_quit = true;
    return !g_quit;
}

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

void fatal(const char* msg) {
    fprintf(stderr, "FATAL: %s\n", msg);
    fflush(stderr);
    exit(1);
}

double getTime() { return realTime() - g_startTime; }

void sleep(double seconds) {
    if (g_fixedDt > 0.0) return;
    if (seconds <= 0) return;
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

u64 unixTime() {
    // B360_FIXED_TIME pins os.time() (Balatro seeds new runs from it), so
    // scripted runs are reproducible frame for frame.
    const char* t = getenv("B360_FIXED_TIME");
    if (t && *t) return (u64)strtoull(t, NULL, 10);
    return (u64)time(NULL);
}

// ------------------------------------------------------------------ threads
struct Thread {
    pthread_t th;
    ThreadFunc fn;
    void* arg;
};

static void* threadTrampoline(void* p) {
    Thread* t = (Thread*)p;
    t->fn(t->arg);
    return NULL;
}

Thread* threadCreate(ThreadFunc fn, void* arg, int preferredCore) {
    (void)preferredCore;
    Thread* t = new Thread;
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    if (pthread_create(&t->th, &attr, threadTrampoline, t) != 0) {
        pthread_attr_destroy(&attr);
        delete t;
        return NULL;
    }
    pthread_attr_destroy(&attr);
    return t;
}

void threadJoin(Thread* t) {
    if (!t) return;
    pthread_join(t->th, NULL);
    delete t;
}

Mutex::Mutex() {
    pthread_mutex_t* m = new pthread_mutex_t;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    h = m;
}
Mutex::~Mutex() {
    pthread_mutex_destroy((pthread_mutex_t*)h);
    delete (pthread_mutex_t*)h;
}
void Mutex::lock() { pthread_mutex_lock((pthread_mutex_t*)h); }
void Mutex::unlock() { pthread_mutex_unlock((pthread_mutex_t*)h); }

Cond::Cond() {
    pthread_cond_t* c = new pthread_cond_t;
    pthread_cond_init(c, NULL);
    h = c;
}
Cond::~Cond() {
    pthread_cond_destroy((pthread_cond_t*)h);
    delete (pthread_cond_t*)h;
}
void Cond::wait(Mutex& m) {
    pthread_cond_wait((pthread_cond_t*)h, (pthread_mutex_t*)m.handle());
}
bool Cond::waitTimeout(Mutex& m, double seconds) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double t = (double)ts.tv_nsec * 1e-9 + seconds;
    ts.tv_sec += (time_t)t;
    ts.tv_nsec = (long)((t - (double)(time_t)t) * 1e9);
    return pthread_cond_timedwait((pthread_cond_t*)h, (pthread_mutex_t*)m.handle(), &ts) == 0;
}
void Cond::signal() { pthread_cond_signal((pthread_cond_t*)h); }
void Cond::broadcast() { pthread_cond_broadcast((pthread_cond_t*)h); }

i32 atomicInc(volatile i32* v) { return __sync_add_and_fetch(v, 1); }
i32 atomicDec(volatile i32* v) { return __sync_sub_and_fetch(v, 1); }

// -------------------------------------------------------------- file system
std::string sourceRoot() {
    const char* d = getenv("B360_GAME_DIR");
    return (d && *d) ? std::string(d) : std::string("game");
}

std::string saveRoot() {
    const char* d = getenv("B360_SAVE_DIR");
    return (d && *d) ? std::string(d) : std::string("save");
}

std::string joinPath(const std::string& base, const std::string& rel) {
    if (rel.empty()) return base;
    if (base.empty()) return rel;
    if (base[base.size() - 1] == '/') return base + rel;
    return base + "/" + rel;
}

bool fileRead(const std::string& path, std::vector<u8>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || S_ISDIR(st.st_mode)) {
        fclose(f);
        return false;
    }
    out.resize((size_t)st.st_size);
    size_t n = out.empty() ? 0 : fread(&out[0], 1, out.size(), f);
    fclose(f);
    out.resize(n);
    return true;
}

bool fileWrite(const std::string& path, const void* data, size_t size, bool append) {
    FILE* f = fopen(path.c_str(), append ? "ab" : "wb");
    if (!f) return false;
    size_t n = size ? fwrite(data, 1, size, f) : 0;
    fclose(f);
    return n == size;
}

bool fileRemove(const std::string& path) {
    if (remove(path.c_str()) == 0) return true;
    return rmdir(path.c_str()) == 0;
}

bool dirCreate(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty()) {
                if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
            }
        }
        if (i < path.size()) cur += path[i];
    }
    return true;
}

bool pathInfo(const std::string& path, PathInfo& info) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    info.type = S_ISDIR(st.st_mode) ? PATH_DIR : PATH_FILE;
    info.size = (i64)st.st_size;
    info.modtime = (i64)st.st_mtime;
    return true;
}

bool dirList(const std::string& path, std::vector<std::string>& names) {
    DIR* d = opendir(path.c_str());
    if (!d) return false;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        names.push_back(e->d_name);
    }
    closedir(d);
    return true;
}

// ------------------------------------------------------------------- input
void padPoll(PadState pads[MAX_PADS]) {
    for (int i = 0; i < MAX_PADS; ++i) memset(&pads[i], 0, sizeof(PadState));
    pads[0] = g_pad;
}

void padVibrate(int pad, float left, float right) {
    (void)pad;
    (void)left;
    (void)right;
}

void keyPoll(std::vector<KeyEvent>& out) {
    out.insert(out.end(), g_keys.begin(), g_keys.end());
    g_keys.clear();
}

bool mousePosition(float& x, float& y) {
    x = g_mouseX;
    y = g_mouseY;
    return true;
}

// ------------------------------------------------------------------- audio
namespace {
AudioCallback g_audioCb = NULL;
void* g_audioUser = NULL;
volatile bool g_audioRun = false;
Thread* g_audioThread = NULL;
FILE* g_wav = NULL;
u32 g_wavBytes = 0;

void put32(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
void put16(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

void writeWavHeader() {
    u8 h[44];
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + g_wavBytes);
    memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 2);
    put32(h + 24, AUDIO_RATE); put32(h + 28, AUDIO_RATE * 4); put16(h + 32, 4); put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, g_wavBytes);
    fseek(g_wav, 0, SEEK_SET);
    fwrite(h, 1, 44, g_wav);
    fseek(g_wav, 0, SEEK_END);
}

void audioThread(void*) {
    const int frames = 1024;
    std::vector<i16> buf(frames * 2);
    double next = realTime();
    while (g_audioRun) {
        g_audioCb(&buf[0], frames, g_audioUser);
        if (g_wav) {
            // WAV is little endian; host is little endian too.
            fwrite(&buf[0], 4, frames, g_wav);
            g_wavBytes += frames * 4;
        }
        next += (double)frames / AUDIO_RATE;
        double now = realTime();
        if (next > now) {
            // Real sleep even in fixed-timestep mode (plat::sleep is a no-op there).
            double s = next - now;
            struct timespec ts;
            ts.tv_sec = (time_t)s;
            ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
            nanosleep(&ts, NULL);
        }
        else if (now - next > 0.5) next = now;
    }
}
}  // namespace

bool audioStart(AudioCallback cb, void* user) {
    g_audioCb = cb;
    g_audioUser = user;
    const char* wav = getenv("B360_WAV");
    if (wav && *wav) {
        g_wav = fopen(wav, "wb");
        if (g_wav) writeWavHeader();
    }
    g_audioRun = true;
    g_audioThread = threadCreate(audioThread, NULL, -1);
    return g_audioThread != NULL;
}

void audioStop() {
    if (!g_audioThread) return;
    g_audioRun = false;
    threadJoin(g_audioThread);
    g_audioThread = NULL;
    if (g_wav) {
        writeWavHeader();
        fclose(g_wav);
        g_wav = NULL;
    }
}

// ------------------------------------------------------------------- system
bool userName(std::string& name) {
    const char* u = getenv("USER");
    if (!u) return false;
    name = u;
    return true;
}

int processorCount() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

double fixedTimestep() { return g_fixedDt; }

bool screenshotRequested(std::string& path) {
    if (g_shotPath.empty()) return false;
    path = g_shotPath;
    g_shotPath.clear();
    return true;
}

}  // namespace plat
