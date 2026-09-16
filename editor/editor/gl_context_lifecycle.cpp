#include "gl_context_lifecycle.h"

namespace glctx {

namespace {
    // Simple atomic flag — destructors may run on the main thread only,
    // but using atomic here is cheap insurance against future refactors
    // and lets debug tooling query the state safely.
    bool g_alive = false;
}

void setAlive(bool alive) {
    g_alive = alive;
}

bool isAlive() {
    return g_alive;
}

} // namespace glctx
