// Xbox 360 implementation of the platform layer on the open xSDK
// (github.com/xenosworks/xsdk): POSIX threads and files from xSDK's C
// runtime, controllers and the gamertag through XAM, audio through xSDK's
// feeder for the kernel's render driver.
//
// Paths on the console, as in the XDK build:
//   game:\game\    extracted game files (main.lua, resources\...)
//   game:\save\    save files (created on first run)
//   game:\b360.log runtime log
// "game:" is the folder the XEX was launched from.
#include "../platform.h"

#include <xsdk/audio.h>
#include <xsdk/kernel.h>
#include <xsdk/log.h>
#include <xsdk/thread.h>
#include <xsdk/xam.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace plat {

namespace {

const char* kSourceRoot = "game:/game";
const char* kSaveRoot = "game:/save";
const char* kLogPath = "game:/b360.log";

// XInput's standard dead zones (the XDK's XINPUT_GAMEPAD_* constants).
const int kLeftDeadzone = 7849;
const int kRightDeadzone = 8689;
const int kTriggerThreshold = 30;

double g_start = 0.0;
bool g_quit = false;
double g_quitComboStart = -1.0;
// Hold BACK and press RB for a screenshot (Balatro doesn't use BACK).
bool g_shotHeld = false;
bool g_shotWanted = false;
int g_shotNumber = 0;

double monotonic() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// b360.log is written by a background thread: writing and flushing to a USB
// drive takes milliseconds per call, which stalled the game whenever it
// printed (Balatro prints on every slow frame, so one stall caused more).
int g_logFile = -1;
pthread_mutex_t g_logLock = PTHREAD_MUTEX_INITIALIZER;       // guards g_logPending
pthread_mutex_t g_logFileLock = PTHREAD_MUTEX_INITIALIZER;   // keeps batches in order
pthread_cond_t g_logWake = PTHREAD_COND_INITIALIZER;
std::string g_logPending;
pthread_t g_logThread;
bool g_logThreadStarted = false;
volatile bool g_logRun = false;

void flushLog() {
    pthread_mutex_lock(&g_logFileLock);
    std::string out;
    pthread_mutex_lock(&g_logLock);
    out.swap(g_logPending);
    pthread_mutex_unlock(&g_logLock);
    if (!out.empty() && g_logFile >= 0) {
        size_t done = 0;
        while (done < out.size()) {
            ssize_t n = write(g_logFile, out.data() + done, out.size() - done);
            if (n <= 0) break;
            done += (size_t)n;
        }
        fsync(g_logFile);
    }
    pthread_mutex_unlock(&g_logFileLock);
}

void* logWriter(void*) {
    xsdk_thread_move(4);
    while (g_logRun) {
        pthread_mutex_lock(&g_logLock);
        if (g_logPending.empty() && g_logRun) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 250 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g_logWake, &g_logLock, &ts);
        }
        pthread_mutex_unlock(&g_logLock);
        flushLog();
    }
    flushLog();
    return NULL;
}

}  // namespace

// ---------------------------------------------------------------- lifecycle
bool init(Config& cfg) {
    g_start = monotonic();
    g_logFile = open(kLogPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    g_logRun = true;
    if (pthread_create(&g_logThread, NULL, logWriter, NULL) == 0) {
        g_logThreadStarted = true;
    } else {
        g_logRun = false;   // log() then writes directly
    }
    cfg.width = 1280;
    cfg.height = 720;
    return true;
}

void shutdown() {
    if (g_logThreadStarted) {
        pthread_mutex_lock(&g_logLock);
        g_logRun = false;
        pthread_cond_signal(&g_logWake);
        pthread_mutex_unlock(&g_logLock);
        pthread_join(g_logThread, NULL);
        g_logThreadStarted = false;
    }
    flushLog();
    if (g_logFile >= 0) {
        close(g_logFile);
        g_logFile = -1;
    }
}

const char* osName() { return "Xbox 360"; }

bool pump() {
    // Hold BACK + START for one second to quit to the dashboard. This also
    // works on error screens, which otherwise wait for a keyboard Escape.
    bool combo = false, shot = false;
    for (u32 i = 0; i < MAX_PADS; ++i) {
        INPUT_STATE st;
        if (XamInputGetState(i, 0, &st) == 0) {
            u16 b = st.gamepad.buttons;
            if ((b & INPUT_GAMEPAD_BACK) && (b & INPUT_GAMEPAD_START)) combo = true;
            if ((b & INPUT_GAMEPAD_BACK) && (b & INPUT_GAMEPAD_RIGHT_SHOULDER)) shot = true;
        }
    }
    if (shot && !g_shotHeld) g_shotWanted = true;
    g_shotHeld = shot;
    double now = getTime();
    if (combo) {
        if (g_quitComboStart < 0) g_quitComboStart = now;
        else if (now - g_quitComboStart > 1.0) g_quit = true;
    } else {
        g_quitComboStart = -1.0;
    }
    return !g_quit;
}

void log(const char* fmt, ...) {
    char buf[2048];
    // Seconds since start, so b360.log shows load times and stalls.
    int pre = snprintf(buf, 32, "[%8.3f] ", getTime());
    if (pre < 0 || pre > 31) pre = 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pre, sizeof(buf) - 2 - pre, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof(buf) - 3 - pre) n = (int)strlen(buf + pre);
    n += pre;
    buf[n++] = '\r';
    buf[n++] = '\n';
    buf[n] = 0;
    pthread_mutex_lock(&g_logLock);
    if (g_logPending.size() < 4 * 1024 * 1024) g_logPending.append(buf, n);
    if (g_logRun) pthread_cond_signal(&g_logWake);
    pthread_mutex_unlock(&g_logLock);
    if (!g_logRun) flushLog();
}

void fatal(const char* msg) {
    log("FATAL: %s", msg);
    // Also in xSDK's own log (default.log), which is written at once.
    xsdk_log_write("FATAL: %s", msg);
    flushLog();
}

// --------------------------------------------------------------------- time
double getTime() { return monotonic() - g_start; }

void sleep(double seconds) {
    if (seconds <= 0) return;
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

u64 unixTime() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (u64)ts.tv_sec;
}

// ------------------------------------------------------------------ threads
struct Thread {
    pthread_t t;
    ThreadFunc fn;
    void* arg;
    int core;
};

static void* threadTrampoline(void* p) {
    Thread* t = (Thread*)p;
    // Threads start on their creator's hardware thread until moved.
    if (t->core >= 0 && t->core < XSDK_HARDWARE_THREADS) xsdk_thread_move((u32)t->core);
    t->fn(t->arg);
    return NULL;
}

Thread* threadCreate(ThreadFunc fn, void* arg, int preferredCore) {
    Thread* t = new Thread;
    t->fn = fn;
    t->arg = arg;
    t->core = preferredCore;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    int rc = pthread_create(&t->t, &attr, threadTrampoline, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        delete t;
        return NULL;
    }
    return t;
}

void threadJoin(Thread* t) {
    if (!t) return;
    pthread_join(t->t, NULL);
    delete t;
}

// xSDK's mutexes are kernel critical sections, recursive like the XDK's.
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
void Cond::wait(Mutex& m) { pthread_cond_wait((pthread_cond_t*)h, (pthread_mutex_t*)m.handle()); }
bool Cond::waitTimeout(Mutex& m, double seconds) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (seconds < 0) seconds = 0;
    time_t whole = (time_t)seconds;
    ts.tv_sec += whole;
    ts.tv_nsec += (long)((seconds - (double)whole) * 1e9);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait((pthread_cond_t*)h, (pthread_mutex_t*)m.handle(), &ts) == 0;
}
void Cond::signal() { pthread_cond_signal((pthread_cond_t*)h); }
void Cond::broadcast() { pthread_cond_broadcast((pthread_cond_t*)h); }

i32 atomicInc(volatile i32* v) { return __sync_add_and_fetch(v, 1); }
i32 atomicDec(volatile i32* v) { return __sync_sub_and_fetch(v, 1); }

// -------------------------------------------------------------- file system
std::string sourceRoot() { return kSourceRoot; }
std::string saveRoot() { return kSaveRoot; }

std::string joinPath(const std::string& base, const std::string& rel) {
    if (rel.empty()) return base;
    if (base.empty()) return rel;
    if (base[base.size() - 1] == '/') return base + rel;
    return base + "/" + rel;
}

bool fileRead(const std::string& path, std::vector<u8>& out) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return false;
    }
    size_t size = (size_t)st.st_size;
    out.resize(size);
    size_t total = 0;
    while (total < size) {
        ssize_t got = read(fd, &out[total], size - total);
        if (got <= 0) break;
        total += (size_t)got;
    }
    close(fd);
    out.resize(total);
    return total == size;
}

bool fileWrite(const std::string& path, const void* data, size_t size, bool append) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
    if (fd < 0) return false;
    const u8* p = (const u8*)data;
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, p + done, size - done);
        if (n <= 0) break;
        done += (size_t)n;
    }
    // Saves must survive Xbox Home, which ends the title without exit().
    fsync(fd);
    close(fd);
    return done == size;
}

bool fileRemove(const std::string& path) {
    // xSDK's remove() takes empty folders too (there is no rmdir).
    return remove(path.c_str()) == 0;
}

bool dirCreate(const std::string& path) {
    // Skip the drive ("game:/").
    size_t start = path.find(":/");
    start = (start == std::string::npos) ? 0 : start + 2;
    for (size_t i = start; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            std::string sub = path.substr(0, i);
            if (sub.size() > start) mkdir(sub.c_str(), 0755);
        }
    }
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool pathInfo(const std::string& path, PathInfo& info) {
    std::string p = path;
    while (p.size() > 1 && p[p.size() - 1] == '/') p.erase(p.size() - 1);
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
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
namespace {
float stick(i16 v, int deadzone) {
    if (v > -deadzone && v < deadzone) return 0.0f;
    return v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f;
}
}  // namespace

void padPoll(PadState pads[MAX_PADS]) {
    for (int i = 0; i < MAX_PADS; ++i) {
        PadState& p = pads[i];
        memset(&p, 0, sizeof(p));
        INPUT_STATE st;
        if (XamInputGetState((u32)i, 0, &st) != 0) continue;
        p.connected = true;
        const INPUT_GAMEPAD& g = st.gamepad;
        u16 b = g.buttons;
        p.buttons[PAD_A] = (b & INPUT_GAMEPAD_A) != 0;
        p.buttons[PAD_B] = (b & INPUT_GAMEPAD_B) != 0;
        p.buttons[PAD_X] = (b & INPUT_GAMEPAD_X) != 0;
        p.buttons[PAD_Y] = (b & INPUT_GAMEPAD_Y) != 0;
        p.buttons[PAD_BACK] = (b & INPUT_GAMEPAD_BACK) != 0;
        p.buttons[PAD_START] = (b & INPUT_GAMEPAD_START) != 0;
        p.buttons[PAD_LEFTSTICK] = (b & INPUT_GAMEPAD_LEFT_THUMB) != 0;
        p.buttons[PAD_RIGHTSTICK] = (b & INPUT_GAMEPAD_RIGHT_THUMB) != 0;
        p.buttons[PAD_LEFTSHOULDER] = (b & INPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        // RB with BACK held takes a screenshot (pump), not a game input.
        p.buttons[PAD_RIGHTSHOULDER] = (b & INPUT_GAMEPAD_RIGHT_SHOULDER) != 0 && !(b & INPUT_GAMEPAD_BACK);
        p.buttons[PAD_DPUP] = (b & INPUT_GAMEPAD_DPAD_UP) != 0;
        p.buttons[PAD_DPDOWN] = (b & INPUT_GAMEPAD_DPAD_DOWN) != 0;
        p.buttons[PAD_DPLEFT] = (b & INPUT_GAMEPAD_DPAD_LEFT) != 0;
        p.buttons[PAD_DPRIGHT] = (b & INPUT_GAMEPAD_DPAD_RIGHT) != 0;
        // SDL/LÖVE conventions: Y axes point down, triggers 0..1.
        p.axes[AXIS_LEFTX] = stick(g.thumb_lx, kLeftDeadzone);
        p.axes[AXIS_LEFTY] = -stick(g.thumb_ly, kLeftDeadzone);
        p.axes[AXIS_RIGHTX] = stick(g.thumb_rx, kRightDeadzone);
        p.axes[AXIS_RIGHTY] = -stick(g.thumb_ry, kRightDeadzone);
        p.axes[AXIS_TRIGGERLEFT] = g.left_trigger > kTriggerThreshold ? g.left_trigger / 255.0f : 0.0f;
        p.axes[AXIS_TRIGGERRIGHT] = g.right_trigger > kTriggerThreshold ? g.right_trigger / 255.0f : 0.0f;
    }
}

void padVibrate(int pad, float left, float right) {
    // Balatro sets the vibration every frame; only talk to the controller
    // when the value actually changes.
    static u16 lastL[MAX_PADS] = {0, 0, 0, 0};
    static u16 lastR[MAX_PADS] = {0, 0, 0, 0};
    if (pad < 0 || pad >= MAX_PADS) return;
    INPUT_VIBRATION v;
    v.left_motor_speed = (u16)(left * 65535.0f);
    v.right_motor_speed = (u16)(right * 65535.0f);
    if (v.left_motor_speed == lastL[pad] && v.right_motor_speed == lastR[pad]) return;
    lastL[pad] = v.left_motor_speed;
    lastR[pad] = v.right_motor_speed;
    XamInputSetState((u32)pad, 0, &v);
}

void keyPoll(std::vector<KeyEvent>& out) { (void)out; }

bool mousePosition(float& x, float& y) {
    x = 0;
    y = 0;
    return false;
}

// ------------------------------------------------------------------- audio
//
// xSDK's feeder model: our thread waits for a free slot in the kernel's
// queue (a frame is 256 samples of 6 big-endian float channels at 48 kHz),
// mixes into it and queues it. The mix is stereo, so the centre, LFE and
// back channels stay silent.
namespace {

AudioCallback g_audioCb = NULL;
void* g_audioUser = NULL;
volatile bool g_audioRun = false;
Thread* g_audioThread = NULL;
i16 g_audioMix[XSDK_AUDIO_FRAME_SAMPLES * 2];

void audioFeeder(void*) {
    const float scale = 1.0f / 32768.0f;
    while (g_audioRun) {
        float* frame = NULL;
        NTSTATUS st = xsdk_audio_next_frame(100, &frame);
        if (st == STATUS_IO_TIMEOUT) continue;
        if (!NT_SUCCESS(st)) break;   // closed
        g_audioCb(g_audioMix, XSDK_AUDIO_FRAME_SAMPLES, g_audioUser);
        float* left = frame + XSDK_AUDIO_FRONT_LEFT * XSDK_AUDIO_FRAME_SAMPLES;
        float* right = frame + XSDK_AUDIO_FRONT_RIGHT * XSDK_AUDIO_FRAME_SAMPLES;
        for (int i = 0; i < XSDK_AUDIO_FRAME_SAMPLES; ++i) {
            left[i] = g_audioMix[i * 2] * scale;
            right[i] = g_audioMix[i * 2 + 1] * scale;
        }
        // Slots are reused: write silence into the other channels each time.
        memset(frame + XSDK_AUDIO_CENTER * XSDK_AUDIO_FRAME_SAMPLES, 0,
               (XSDK_AUDIO_CHANNELS - 2) * XSDK_AUDIO_FRAME_SAMPLES * sizeof(float));
        xsdk_audio_queue_frame();
    }
}

}  // namespace

bool audioStart(AudioCallback cb, void* user) {
    g_audioCb = cb;
    g_audioUser = user;
    NTSTATUS st = xsdk_audio_open(0);
    if (!NT_SUCCESS(st)) {
        log("[audio] xsdk_audio_open failed: 0x%08X", (unsigned)st);
        return false;
    }
    g_audioRun = true;
    // Hardware thread 3 (core 1, second thread); the game runs on 0, Lua
    // workers on 2/4/5.
    g_audioThread = threadCreate(audioFeeder, NULL, 3);
    return g_audioThread != NULL;
}

void audioStop() {
    if (!g_audioThread) return;
    g_audioRun = false;
    xsdk_audio_close();   // wakes the feeder
    threadJoin(g_audioThread);
    g_audioThread = NULL;
    XsdkAudioStats s;
    xsdk_audio_stats(&s);
    log("[audio] %u frames played, %u silent (underruns), longest wait %u us", (unsigned)s.played,
        (unsigned)s.silent, (unsigned)s.longest_wait_us);
}

// ------------------------------------------------------------------- system
bool userName(std::string& name) {
    u32 index = 0;
    char tag[XSDK_XAM_GAMERTAG_BYTES];
    memset(tag, 0, sizeof(tag));
    if (xsdk_xam_signed_in_user(&index, tag) != 0 || !tag[0]) return false;
    name = tag;
    return true;
}

int processorCount() { return XSDK_HARDWARE_THREADS; }

double fixedTimestep() { return 0.0; }

bool screenshotRequested(std::string& path) {
    if (!g_shotWanted) return false;
    g_shotWanted = false;
    dirCreate("game:/shots");
    // shot_0001.png and on, past the ones already there.
    for (;;) {
        ++g_shotNumber;
        char digits[5];
        int v = g_shotNumber;
        for (int i = 3; i >= 0; --i, v /= 10) digits[i] = (char)('0' + v % 10);
        digits[4] = 0;
        path = std::string("game:/shots/shot_") + digits + ".png";
        struct stat sb;
        if (stat(path.c_str(), &sb) != 0 || g_shotNumber >= 9999) break;
    }
    return true;
}

}  // namespace plat
