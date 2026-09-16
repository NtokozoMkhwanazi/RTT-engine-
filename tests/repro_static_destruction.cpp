/**
 * Repro for the exit-time SIGSEGV in crash.log:
 *
 *   ResourceManager (Meyers singleton) -> ~ResourceManager -> clearCache()
 *     -> ~shared_ptr<Model> -> Model::~Model -> Mesh::~Mesh -> Mesh::Clear()
 *       -> glDeleteVertexArrays        <-- GL context already destroyed
 *
 * Sequence (mirrors the editor/test_runner exit path):
 *   1. Create a GL context (GLFW + GLAD).
 *   2. Load a model into the ResourceManager singleton.
 *   3. Destroy the context WITHOUT clearing the cache.
 *   4. Return -> static destruction of the singleton -> GL calls on dead
 *      context (SIGSEGV before the fix; clean exit after).
 *
 * Build/link like bin/test_runner (Makefile SRC_CPP set). Exit code 0 =
 * fix holds; SIGSEGV = regression.
 */
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include "modelSystem/Model.h"
#include "editor/resource_manager.h"
#include "editor/gl_context_lifecycle.h"

int main() {
    if (!glfwInit()) { std::cerr << "glfwInit failed\n"; return 2; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "repro", nullptr, nullptr);
    if (!win) { std::cerr << "window failed\n"; glfwTerminate(); return 2; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "glad failed\n"; glfwDestroyWindow(win); glfwTerminate(); return 2;
    }
    glctx::setAlive(true);

    // Populate the singleton cache (the whole point: it outlives main()).
    auto& rm = Resources::ResourceManager::getInstance();
    // Path starts with '.' so resolveModelPath returns it as-is; the config
    // default modelPath is ./models which is gitignored/empty here, while the
    // engine's world-object path uses ./assets/World_objects directly.
    auto model = rm.loadModel("./assets/World_objects/Rock0.fbx");
    if (!model) {
        std::cerr << "model load failed - aborting repro\n";
        glctx::setAlive(false);
        glfwDestroyWindow(win); glfwTerminate();
        return 2;   // can't exercise the path without a cached model
    }
    std::cout << "cached model meshes=" << model->GetMeshCount()
              << " cacheSize=" << rm.getCacheSize() << "\n";

    // Tear down the context WITHOUT clearing the cache - exactly what the
    // RHI/editor does before static destruction runs.
    glctx::setAlive(false);
    glfwDestroyWindow(win);
    glfwTerminate();

    std::cout << "context torn down, returning -> static destruction of singleton...\n";
    return 0;
}
