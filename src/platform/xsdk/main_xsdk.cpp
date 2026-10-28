// Xbox 360 entry point (xSDK). xSDK's start-up opens its own log
// (default.log), runs the static constructors and calls main(); returning
// from main goes back to the dashboard.
#include "../../runtime/runtime.h"

#include <xsdk/thread.h>

#include <pthread.h>

namespace {

int g_exitCode = 0;

// The main thread has a 1 MB stack; Lua and Balatro's UI code recurse
// deeply, so the game runs on a thread with a large stack instead, on
// hardware thread 0 like the XDK build.
void* gameThread(void*) {
    xsdk_thread_move(0);
    char arg0[] = "game:\\default.xex";
    char* argv[] = {arg0, NULL};
    g_exitCode = rt::run(1, argv);
    return NULL;
}

}  // namespace

int main() {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);
    pthread_t t;
    if (pthread_create(&t, &attr, gameThread, NULL) == 0) pthread_join(t, NULL);
    pthread_attr_destroy(&attr);
    return g_exitCode;
}
