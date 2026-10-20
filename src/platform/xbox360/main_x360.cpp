// Xbox 360 entry point.
#include <xtl.h>

#include "../../runtime/runtime.h"

namespace {

int g_exitCode = 0;

// The XEX main thread has a small stack; Lua and Balatro's UI code recurse
// deeply, so the game runs on a thread with a large stack instead.
DWORD WINAPI gameThread(LPVOID) {
    char arg0[] = "game:\\default.xex";
    char* argv[] = {arg0, NULL};
    g_exitCode = rt::run(1, argv);
    return 0;
}

}  // namespace

void __cdecl main() {
    DWORD id = 0;
    HANDLE h = CreateThread(NULL, 4 * 1024 * 1024, gameThread, NULL, CREATE_SUSPENDED, &id);
    if (h) {
        XSetThreadProcessor(h, 0);
        ResumeThread(h);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    // Back to the dashboard / homebrew launcher.
    XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
}
