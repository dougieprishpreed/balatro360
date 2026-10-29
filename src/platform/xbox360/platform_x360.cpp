// Xbox 360 implementation of the platform layer (XDK).
//
// Paths on the console:
//   game:\game\    extracted game files (main.lua, resources\...)
//   game:\save\    save files (created on first run)
//   game:\b360.log runtime log
// "game:" is the folder the XEX was launched from.
#include "../platform.h"

#include <xtl.h>

#include <string>
#include <xaudio2.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Not defined by the XDK's headers.
#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

namespace plat {

namespace {

LARGE_INTEGER g_freq;
LARGE_INTEGER g_start;
// b360.log is written by a background thread: writing and flushing to a USB
// drive takes milliseconds per call, which stalled the game whenever it
// printed (Balatro prints on every slow frame, so one stall caused more).
HANDLE g_logFile = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_logLock;       // guards g_logPending
CRITICAL_SECTION g_logFileLock;   // keeps batches in order when flushing
std::string g_logPending;
HANDLE g_logEvent = NULL;
HANDLE g_logThread = NULL;
volatile LONG g_logRun = 0;

void flushLog() {
    EnterCriticalSection(&g_logFileLock);
    std::string out;
    EnterCriticalSection(&g_logLock);
    out.swap(g_logPending);
    LeaveCriticalSection(&g_logLock);
    if (!out.empty() && g_logFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_logFile, out.data(), (DWORD)out.size(), &written, NULL);
        FlushFileBuffers(g_logFile);
    }
    LeaveCriticalSection(&g_logFileLock);
}

DWORD WINAPI logWriter(LPVOID) {
    while (g_logRun) {
        WaitForSingleObject(g_logEvent, 250);
        flushLog();
    }
    flushLog();
    return 0;
}
bool g_quit = false;
double g_quitComboStart = -1.0;

const char* kSourceRoot = "game:\\game";
const char* kSaveRoot = "game:\\save";
const char* kLogPath = "game:\\b360.log";

std::string toNative(const std::string& p) {
    std::string out = p;
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i] == '/') out[i] = '\\';
    return out;
}

}  // namespace

// ---------------------------------------------------------------- lifecycle
bool init(Config& cfg) {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    InitializeCriticalSection(&g_logLock);
    InitializeCriticalSection(&g_logFileLock);
    g_logFile = CreateFileA(kLogPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    g_logEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_logRun = 1;
    g_logThread = CreateThread(NULL, 64 * 1024, logWriter, NULL, CREATE_SUSPENDED, NULL);
    if (g_logThread) {
        XSetThreadProcessor(g_logThread, 4);
        ResumeThread(g_logThread);
    } else {
        g_logRun = 0;   // log() then writes directly
    }
    cfg.width = 1280;
    cfg.height = 720;
    return true;
}

void shutdown() {
    if (g_logThread) {
        g_logRun = 0;
        SetEvent(g_logEvent);
        WaitForSingleObject(g_logThread, INFINITE);
        CloseHandle(g_logThread);
        g_logThread = NULL;
    }
    flushLog();
    if (g_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
}

const char* osName() { return "Xbox 360"; }

bool pump() {
    // Hold BACK + START for one second to quit to the dashboard. This also
    // works on error screens, which otherwise wait for a keyboard Escape.
    XINPUT_STATE st;
    bool combo = false;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (XInputGetState(i, &st) == ERROR_SUCCESS) {
            WORD b = st.Gamepad.wButtons;
            if ((b & XINPUT_GAMEPAD_BACK) && (b & XINPUT_GAMEPAD_START)) combo = true;
        }
    }
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
    OutputDebugStringA(buf);
    EnterCriticalSection(&g_logLock);
    if (g_logPending.size() < 4 * 1024 * 1024) g_logPending.append(buf, n);
    LeaveCriticalSection(&g_logLock);
    if (g_logRun) SetEvent(g_logEvent);
    else flushLog();
}

void fatal(const char* msg) {
    log("FATAL: %s", msg);
    flushLog();
}

// --------------------------------------------------------------------- time
double getTime() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - g_start.QuadPart) / (double)g_freq.QuadPart;
}

void sleep(double seconds) {
    if (seconds <= 0) return;
    Sleep((DWORD)(seconds * 1000.0 + 0.5));
}

u64 unixTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    u64 t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ULL) / 10000000ULL;
}

// ------------------------------------------------------------------ threads
struct Thread {
    HANDLE h;
    ThreadFunc fn;
    void* arg;
};

static DWORD WINAPI threadTrampoline(LPVOID p) {
    Thread* t = (Thread*)p;
    t->fn(t->arg);
    return 0;
}

Thread* threadCreate(ThreadFunc fn, void* arg, int preferredCore, size_t stackBytes) {
    Thread* t = new Thread;
    t->fn = fn;
    t->arg = arg;
    DWORD id = 0;
    t->h = CreateThread(NULL, stackBytes ? (SIZE_T)stackBytes : 1024 * 1024, threadTrampoline, t, CREATE_SUSPENDED,
                        &id);
    if (!t->h) {
        delete t;
        return NULL;
    }
    if (preferredCore >= 0 && preferredCore < 6) XSetThreadProcessor(t->h, preferredCore);
    ResumeThread(t->h);
    return t;
}

void threadJoin(Thread* t) {
    if (!t) return;
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    delete t;
}

Mutex::Mutex() {
    CRITICAL_SECTION* cs = new CRITICAL_SECTION;
    InitializeCriticalSection(cs);   // recursive
    h = cs;
}
Mutex::~Mutex() {
    DeleteCriticalSection((CRITICAL_SECTION*)h);
    delete (CRITICAL_SECTION*)h;
}
void Mutex::lock() { EnterCriticalSection((CRITICAL_SECTION*)h); }
void Mutex::unlock() { LeaveCriticalSection((CRITICAL_SECTION*)h); }

// Condition variable built from a semaphore (the 360 has no native
// condition variables). Callers always re-check their predicate in a loop,
// so the occasional spurious wakeup this allows is harmless; wakeups are
// never lost because the semaphore keeps the count.
struct CondImpl {
    HANDLE sem;
    CRITICAL_SECTION lock;
    LONG waiters;
};

Cond::Cond() {
    CondImpl* c = new CondImpl;
    c->sem = CreateSemaphore(NULL, 0, 0x7fffffff, NULL);
    InitializeCriticalSection(&c->lock);
    c->waiters = 0;
    h = c;
}
Cond::~Cond() {
    CondImpl* c = (CondImpl*)h;
    CloseHandle(c->sem);
    DeleteCriticalSection(&c->lock);
    delete c;
}
static bool condWait(CondImpl* c, Mutex& m, DWORD ms) {
    EnterCriticalSection(&c->lock);
    ++c->waiters;
    LeaveCriticalSection(&c->lock);
    m.unlock();
    DWORD r = WaitForSingleObject(c->sem, ms);
    EnterCriticalSection(&c->lock);
    --c->waiters;
    LeaveCriticalSection(&c->lock);
    m.lock();
    return r == WAIT_OBJECT_0;
}
void Cond::wait(Mutex& m) { condWait((CondImpl*)h, m, INFINITE); }
bool Cond::waitTimeout(Mutex& m, double seconds) {
    DWORD ms = seconds <= 0 ? 0 : (DWORD)(seconds * 1000.0 + 0.5);
    return condWait((CondImpl*)h, m, ms);
}
void Cond::signal() {
    CondImpl* c = (CondImpl*)h;
    EnterCriticalSection(&c->lock);
    if (c->waiters > 0) ReleaseSemaphore(c->sem, 1, NULL);
    LeaveCriticalSection(&c->lock);
}
void Cond::broadcast() {
    CondImpl* c = (CondImpl*)h;
    EnterCriticalSection(&c->lock);
    if (c->waiters > 0) ReleaseSemaphore(c->sem, c->waiters, NULL);
    LeaveCriticalSection(&c->lock);
}

i32 atomicInc(volatile i32* v) { return (i32)InterlockedIncrement((volatile LONG*)v); }
i32 atomicDec(volatile i32* v) { return (i32)InterlockedDecrement((volatile LONG*)v); }

// -------------------------------------------------------------- file system
std::string sourceRoot() { return kSourceRoot; }
std::string saveRoot() { return kSaveRoot; }

std::string joinPath(const std::string& base, const std::string& rel) {
    if (rel.empty()) return base;
    std::string r = toNative(rel);
    if (base.empty()) return r;
    if (base[base.size() - 1] == '\\') return base + r;
    return base + "\\" + r;
}

bool fileRead(const std::string& path, std::vector<u8>& out) {
    HANDLE f = CreateFileA(toNative(path).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE) {
        CloseHandle(f);
        return false;
    }
    out.resize(size);
    DWORD total = 0;
    while (total < size) {
        DWORD got = 0;
        if (!ReadFile(f, &out[total], size - total, &got, NULL) || got == 0) break;
        total += got;
    }
    CloseHandle(f);
    out.resize(total);
    return total == size;
}

bool fileWrite(const std::string& path, const void* data, size_t size, bool append) {
    HANDLE f = CreateFileA(toNative(path).c_str(), GENERIC_WRITE, 0, NULL, append ? OPEN_ALWAYS : CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    if (append) SetFilePointer(f, 0, NULL, FILE_END);
    DWORD written = 0;
    BOOL ok = size == 0 || WriteFile(f, data, (DWORD)size, &written, NULL);
    CloseHandle(f);
    return ok && written == (DWORD)size;
}

bool fileRemove(const std::string& path) {
    std::string p = toNative(path);
    if (DeleteFileA(p.c_str())) return true;
    return RemoveDirectoryA(p.c_str()) != 0;
}

bool dirCreate(const std::string& path) {
    std::string p = toNative(path);
    // Skip the device prefix ("game:\").
    size_t start = p.find(":\\");
    start = (start == std::string::npos) ? 0 : start + 2;
    for (size_t i = start; i <= p.size(); ++i) {
        if (i == p.size() || p[i] == '\\') {
            std::string sub = p.substr(0, i);
            if (sub.size() > start) CreateDirectoryA(sub.c_str(), NULL);
        }
    }
    DWORD attr = GetFileAttributesA(p.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool pathInfo(const std::string& path, PathInfo& info) {
    std::string p = toNative(path);
    while (p.size() > 1 && p[p.size() - 1] == '\\') p.erase(p.size() - 1);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(p.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        // Roots like "game:\save" can't always be found with FindFirstFile.
        DWORD attr = GetFileAttributesA(p.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) return false;
        info.type = (attr & FILE_ATTRIBUTE_DIRECTORY) ? PATH_DIR : PATH_FILE;
        info.size = 0;
        info.modtime = 0;
        return true;
    }
    FindClose(h);
    info.type = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? PATH_DIR : PATH_FILE;
    info.size = ((i64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    u64 t = ((u64)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
    info.modtime = t > 116444736000000000ULL ? (i64)((t - 116444736000000000ULL) / 10000000ULL) : 0;
    return true;
}

bool dirList(const std::string& path, std::vector<std::string>& names) {
    std::string pattern = toNative(path);
    if (!pattern.empty() && pattern[pattern.size() - 1] != '\\') pattern += "\\";
    pattern += "*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        names.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return true;
}

// ------------------------------------------------------------------- input
namespace {
float stick(SHORT v, SHORT deadzone) {
    if (v > -deadzone && v < deadzone) return 0.0f;
    float f = v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f;
    return f;
}
}  // namespace

void padPoll(PadState pads[MAX_PADS]) {
    for (int i = 0; i < MAX_PADS; ++i) {
        PadState& p = pads[i];
        memset(&p, 0, sizeof(p));
        XINPUT_STATE st;
        if (XInputGetState((DWORD)i, &st) != ERROR_SUCCESS) continue;
        p.connected = true;
        WORD b = st.Gamepad.wButtons;
        p.buttons[PAD_A] = (b & XINPUT_GAMEPAD_A) != 0;
        p.buttons[PAD_B] = (b & XINPUT_GAMEPAD_B) != 0;
        p.buttons[PAD_X] = (b & XINPUT_GAMEPAD_X) != 0;
        p.buttons[PAD_Y] = (b & XINPUT_GAMEPAD_Y) != 0;
        p.buttons[PAD_BACK] = (b & XINPUT_GAMEPAD_BACK) != 0;
        p.buttons[PAD_START] = (b & XINPUT_GAMEPAD_START) != 0;
        p.buttons[PAD_LEFTSTICK] = (b & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
        p.buttons[PAD_RIGHTSTICK] = (b & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
        p.buttons[PAD_LEFTSHOULDER] = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        p.buttons[PAD_RIGHTSHOULDER] = (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
        p.buttons[PAD_DPUP] = (b & XINPUT_GAMEPAD_DPAD_UP) != 0;
        p.buttons[PAD_DPDOWN] = (b & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
        p.buttons[PAD_DPLEFT] = (b & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
        p.buttons[PAD_DPRIGHT] = (b & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
        // SDL/LÖVE conventions: Y axes point down, triggers 0..1.
        p.axes[AXIS_LEFTX] = stick(st.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        p.axes[AXIS_LEFTY] = -stick(st.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        p.axes[AXIS_RIGHTX] = stick(st.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        p.axes[AXIS_RIGHTY] = -stick(st.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        p.axes[AXIS_TRIGGERLEFT] =
            st.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? st.Gamepad.bLeftTrigger / 255.0f : 0.0f;
        p.axes[AXIS_TRIGGERRIGHT] =
            st.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? st.Gamepad.bRightTrigger / 255.0f : 0.0f;
    }
}

void padVibrate(int pad, float left, float right) {
    // Balatro sets the vibration every frame; only talk to the controller
    // when the value actually changes.
    static WORD lastL[MAX_PADS] = {0, 0, 0, 0};
    static WORD lastR[MAX_PADS] = {0, 0, 0, 0};
    if (pad < 0 || pad >= MAX_PADS) return;
    XINPUT_VIBRATION v;
    v.wLeftMotorSpeed = (WORD)(left * 65535.0f);
    v.wRightMotorSpeed = (WORD)(right * 65535.0f);
    if (v.wLeftMotorSpeed == lastL[pad] && v.wRightMotorSpeed == lastR[pad]) return;
    lastL[pad] = v.wLeftMotorSpeed;
    lastR[pad] = v.wRightMotorSpeed;
    XInputSetState((DWORD)pad, &v);
}

void keyPoll(std::vector<KeyEvent>& out) { (void)out; }

bool mousePosition(float& x, float& y) {
    x = 0;
    y = 0;
    return false;
}

// ------------------------------------------------------------------- audio
//
// One XAudio2 source voice fed by a dedicated thread with a small queue of
// buffers. XAudio2 on the 360 takes native (big-endian) 16-bit PCM.
namespace {

const int kAudioFrames = 1024;   // ~21 ms per buffer
const int kAudioBuffers = 4;

class VoiceCallback : public IXAudio2VoiceCallback {
public:
    HANDLE event;
    VoiceCallback() { event = CreateEvent(NULL, FALSE, FALSE, NULL); }
    ~VoiceCallback() { CloseHandle(event); }
    STDMETHOD_(void, OnVoiceProcessingPassStart)(UINT32) {}
    STDMETHOD_(void, OnVoiceProcessingPassEnd)() {}
    STDMETHOD_(void, OnStreamEnd)() {}
    STDMETHOD_(void, OnBufferStart)(void*) {}
    STDMETHOD_(void, OnBufferEnd)(void*) { SetEvent(event); }
    STDMETHOD_(void, OnLoopEnd)(void*) {}
    STDMETHOD_(void, OnVoiceError)(void*, HRESULT) {}
};

IXAudio2* g_xaudio = NULL;
IXAudio2MasteringVoice* g_master = NULL;
IXAudio2SourceVoice* g_voice = NULL;
VoiceCallback* g_voiceCb = NULL;
AudioCallback g_audioCb = NULL;
void* g_audioUser = NULL;
volatile bool g_audioRun = false;
Thread* g_audioThread = NULL;
i16 g_audioBuf[kAudioBuffers][kAudioFrames * 2];

void audioFeeder(void*) {
    int next = 0;
    while (g_audioRun) {
        XAUDIO2_VOICE_STATE vs;
        g_voice->GetState(&vs);
        while (vs.BuffersQueued < (UINT32)kAudioBuffers - 1) {
            g_audioCb(g_audioBuf[next], kAudioFrames, g_audioUser);
            XAUDIO2_BUFFER buf;
            memset(&buf, 0, sizeof(buf));
            buf.AudioBytes = kAudioFrames * 4;
            buf.pAudioData = (const BYTE*)g_audioBuf[next];
            g_voice->SubmitSourceBuffer(&buf);
            next = (next + 1) % kAudioBuffers;
            ++vs.BuffersQueued;
        }
        WaitForSingleObject(g_voiceCb->event, 50);
    }
}

}  // namespace

bool audioStart(AudioCallback cb, void* user) {
    g_audioCb = cb;
    g_audioUser = user;
    if (FAILED(XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        log("[audio] XAudio2Create failed");
        return false;
    }
    if (FAILED(g_xaudio->CreateMasteringVoice(&g_master))) {
        log("[audio] CreateMasteringVoice failed");
        return false;
    }
    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = AUDIO_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = AUDIO_RATE * 4;
    g_voiceCb = new VoiceCallback;
    if (FAILED(g_xaudio->CreateSourceVoice(&g_voice, &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, g_voiceCb))) {
        log("[audio] CreateSourceVoice failed");
        return false;
    }
    g_voice->Start(0);
    g_audioRun = true;
    // Hardware thread 3 (core 1, second thread); the game runs on 0, Lua
    // workers on 2/4/5.
    g_audioThread = threadCreate(audioFeeder, NULL, 3);
    return g_audioThread != NULL;
}

void audioStop() {
    if (!g_audioThread) return;
    g_audioRun = false;
    SetEvent(g_voiceCb->event);
    threadJoin(g_audioThread);
    g_audioThread = NULL;
    if (g_voice) {
        g_voice->Stop(0);
        g_voice->DestroyVoice();
        g_voice = NULL;
    }
    if (g_master) {
        g_master->DestroyVoice();
        g_master = NULL;
    }
    if (g_xaudio) {
        g_xaudio->Release();
        g_xaudio = NULL;
    }
}

// ------------------------------------------------------------------- system
bool userName(std::string& name) {
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (XUserGetSigninState(i) == eXUserSigninState_NotSignedIn) continue;
        char buf[XUSER_NAME_SIZE + 1];
        memset(buf, 0, sizeof(buf));
        if (XUserGetName(i, buf, sizeof(buf)) == ERROR_SUCCESS && buf[0]) {
            name = buf;
            return true;
        }
    }
    return false;
}

int processorCount() { return 6; }

double fixedTimestep() { return 0.0; }

bool screenshotRequested(std::string& path) {
    (void)path;
    return false;
}

}  // namespace plat
