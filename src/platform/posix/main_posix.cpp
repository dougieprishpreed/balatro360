#include "../../runtime/runtime.h"

#include <stdlib.h>

int main(int argc, char** argv) {
    int code = rt::run(argc, argv);
    // Lua worker threads may still be blocked on channels; exit hard.
    exit(code);
    return code;
}
