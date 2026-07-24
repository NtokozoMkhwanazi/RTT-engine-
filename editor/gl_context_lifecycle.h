#pragma once
/**
 * GL Context Lifecycle Guard
 *
 * Tracks whether a valid OpenGL context is currently active.
 *
 * Why this exists:
 * - In test.cpp, main() calls glfwDestroyWindow + glfwTerminate BEFORE returning.
 * - After main() returns, C++ static destructors run.
 * - Several globally-owned objects (g_editor.renderer, g_editor.geoTerrainRenderer,
 *   RenderPipeline singleton) have destructors that perform GL cleanup.
 * - Calling any GL function after the context is destroyed is undefined behavior
 *   and on Linux/GLX manifests as a SIGSEGV at exit.
 *
 * Contract:
 * - main() sets this true after GLFW window creation + GLAD load.
 * - main() sets this false immediately before glfwTerminate().
 * - Any destructor that issues GL calls must early-out when this is false.
 *
 * Note: this does NOT replace per-object init/shutdown discipline (Renderer::Shutdown
 * still uses its m_initialized flag). It is an additional safety net for the
 * "static destructor runs after glfwTerminate" path specifically.
 */
namespace glctx {

// Set true once gladLoadGLLoader has succeeded and the context is current.
// Set false immediately before glfwDestroyWindow/glfwTerminate.
void setAlive(bool alive);

// Query current state. Safe to call from any thread, including destructors.
bool isAlive();

} // namespace glctx
