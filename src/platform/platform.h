// Platform abstraction layer.
//
// Everything the LÖVE runtime needs from the OS goes through this header:
// time, threads, the file system, controllers, audio output and a few
// system queries. There are two implementations:
//   src/platform/posix/   - Linux host build (headless, used for testing)
//   src/platform/xbox360/ - Xbox 360 XDK build
#ifndef B360_PLATFORM_H
#define B360_PLATFORM_H

#include "../common/types.h"
#include <string>
#include <vector>

namespace plat {

// ---------------------------------------------------------------- lifecycle
struct Config {
    int width;   // back buffer size
    int height;
    Config() : width(1280), height(720) {}
};

bool init(Config& cfg);
void shutdown();

// Name returned by love.system.getOS().
const char* osName();

// Called once per frame from love.graphics.present() / love.event.pump().
// Returns false when the platform wants the game to quit.
bool pump();

// Write a line to the debug output (stdout on host, OutputDebugString +
// log file on 360).
void log(const char* fmt, ...);

// Fatal error: show/print the message and stop.
void fatal(const char* msg);

// --------------------------------------------------------------------- time
double getTime();          // seconds, monotonic
void sleep(double seconds);
u64 unixTime();            // wall clock seconds since 1970 (for os.time)

// ------------------------------------------------------------------ threads
typedef void (*ThreadFunc)(void* arg);
struct Thread;
// preferredCore: hint for the 360's six hardware threads (-1 = any).
// stackBytes: 0 for the default (1 MB).
Thread* threadCreate(ThreadFunc fn, void* arg, int preferredCore, size_t stackBytes = 0);
void threadJoin(Thread* t);   // waits and frees

class Mutex {
public:
    Mutex();
    ~Mutex();
    void lock();
    void unlock();
    void* handle() { return h; }
private:
    Mutex(const Mutex&);
    Mutex& operator=(const Mutex&);
    void* h;
};

class Lock {
public:
    explicit Lock(Mutex& m) : mu(m) { mu.lock(); }
    ~Lock() { mu.unlock(); }
private:
    Lock(const Lock&);
    Lock& operator=(const Lock&);
    Mutex& mu;
};

class Cond {
public:
    Cond();
    ~Cond();
    void wait(Mutex& m);
    // Returns false on timeout.
    bool waitTimeout(Mutex& m, double seconds);
    void signal();
    void broadcast();
private:
    Cond(const Cond&);
    Cond& operator=(const Cond&);
    void* h;
};

i32 atomicInc(volatile i32* v);  // returns new value
i32 atomicDec(volatile i32* v);  // returns new value

// -------------------------------------------------------------- file system
// Paths passed to these functions are *native* paths built with joinPath().
std::string sourceRoot();  // read-only game data (extracted .love contents)
std::string saveRoot();    // writable save directory
std::string joinPath(const std::string& base, const std::string& rel);

enum PathType { PATH_NONE = 0, PATH_FILE, PATH_DIR };
struct PathInfo {
    PathType type;
    i64 size;
    i64 modtime;
};

bool fileRead(const std::string& path, std::vector<u8>& out);
bool fileWrite(const std::string& path, const void* data, size_t size, bool append);
bool fileRemove(const std::string& path);   // file or empty directory
bool dirCreate(const std::string& path);    // creates parents as needed
bool pathInfo(const std::string& path, PathInfo& info);
bool dirList(const std::string& path, std::vector<std::string>& names);

// ------------------------------------------------------------------- input
enum PadButton {
    PAD_A = 0, PAD_B, PAD_X, PAD_Y, PAD_BACK, PAD_GUIDE, PAD_START,
    PAD_LEFTSTICK, PAD_RIGHTSTICK, PAD_LEFTSHOULDER, PAD_RIGHTSHOULDER,
    PAD_DPUP, PAD_DPDOWN, PAD_DPLEFT, PAD_DPRIGHT,
    PAD_BUTTON_COUNT
};
enum PadAxis {
    AXIS_LEFTX = 0, AXIS_LEFTY, AXIS_RIGHTX, AXIS_RIGHTY,
    AXIS_TRIGGERLEFT, AXIS_TRIGGERRIGHT,
    PAD_AXIS_COUNT
};
enum { MAX_PADS = 4 };

struct PadState {
    bool connected;
    bool buttons[PAD_BUTTON_COUNT];
    float axes[PAD_AXIS_COUNT];   // sticks -1..1 (y down positive), triggers 0..1
};

void padPoll(PadState pads[MAX_PADS]);
void padVibrate(int pad, float left, float right);

// Keyboard/mouse events (host testing only; the 360 returns nothing).
struct KeyEvent {
    bool pressed;
    std::string key;
};
void keyPoll(std::vector<KeyEvent>& out);
bool mousePosition(float& x, float& y);

// ------------------------------------------------------------------- audio
// The runtime mixes everything itself into interleaved 16-bit stereo at
// AUDIO_RATE and the platform just pulls blocks from this callback on its
// own thread.
enum { AUDIO_RATE = 48000 };
typedef void (*AudioCallback)(i16* out, int frames, void* user);
bool audioStart(AudioCallback cb, void* user);
void audioStop();

// ------------------------------------------------------------------- system
// Player name (gamertag on 360). Returns false if unavailable.
bool userName(std::string& name);
int processorCount();

// Host test hooks (no-ops on 360): frame-indexed screenshots etc.
// Returns a fixed timestep if deterministic mode is enabled, else <= 0.
double fixedTimestep();
// True if a screenshot of the frame being presented should be written.
bool screenshotRequested(std::string& path);

}  // namespace plat

#endif
