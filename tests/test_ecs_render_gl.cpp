// ============================================================================
// ECS render GL tests - verify that spawned primitives (MeshComponent) and
// models (ModelComponent) actually draw visible pixels into the framebuffer.
// This is the path that was broken: the ECS world was never rendered, the
// editor systems pointed at a renderer nobody drew from, and the editor shader
// read its model transform from an instanced attribute nobody fed.
//
// Uses a hidden 64x64 GL context; SKIPs cleanly when no context is available.
// ============================================================================

#include <gtest/gtest.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <fstream>

#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "ecs/components/ModelComponent.h"
#include "ecs/systems/RenderSystem.h"
#include "ecs/systems/ModelRenderSystem.h"
#include "renderer/Renderer.h"
#include "renderer/MeshRegistry.h"
#include "modelSystem/ModelManager.h"
#include "editor/shader_manager.h"
#include "editor/mesh_builder.h"

namespace {

GLFWwindow* g_win = nullptr;

bool EnsureGL() {
    static const bool ok = []() {
        if (!glfwInit()) return false;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        g_win = glfwCreateWindow(64, 64, "ecs-render-test", nullptr, nullptr);
        if (!g_win) { glfwTerminate(); return false; }
        glfwMakeContextCurrent(g_win);
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            glfwDestroyWindow(g_win); g_win = nullptr;
            glfwTerminate(); return false;
        }
        return true;
    }();
    return ok;
}

// The object must occupy a REGION of the framebuffer, not collapse to a single
// point. (A broken model transform collapses every vertex to the screen center,
// which a center-only check would miss.) Counts non-clear pixels and also
// samples a few off-center spots.
void ExpectObjectOccupiesRegion() {
    std::vector<unsigned char> pixels(64 * 64 * 4);
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    auto isClear = [&](int x, int y) {
        const size_t idx = (y * 64 + x) * 4;
        return pixels[idx] == 26 && pixels[idx + 1] == 51 && pixels[idx + 2] == 230;
    };

    // Off-center samples inside the expected object footprint (cube ~24px wide
    // at distance 3; bot larger). A degenerate point leaves these clear.
    EXPECT_FALSE(isClear(32 + 8, 32)) << "off-center pixel is clear - transform collapsed?";
    EXPECT_FALSE(isClear(32, 32 + 8)) << "off-center pixel is clear - transform collapsed?";
    EXPECT_FALSE(isClear(32 - 6, 32 - 6)) << "off-center pixel is clear - transform collapsed?";

    int lit = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            if (!isClear(x, y)) ++lit;
        }
    }
    EXPECT_GT(lit, 50) << "only " << lit << " non-clear pixels - object too small/degenerate";
}

} // namespace

class ECSRenderGLTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!EnsureGL()) GTEST_SKIP() << "No OpenGL context available";
        ShaderManager::InitShaders();
        MeshBuilder::InitAll();
        auto& reg = MeshRegistry::getInstance();
        reg.registerMesh(ecs::MeshType::Cube, MeshBuilder::GetCube().vao, MeshBuilder::GetCube().vbo,
                         MeshBuilder::GetCube().ebo, MeshBuilder::GetCube().indexCount);
        reg.registerMesh(ecs::MeshType::Sphere, MeshBuilder::GetSphere().vao, MeshBuilder::GetSphere().vbo,
                         MeshBuilder::GetSphere().ebo, MeshBuilder::GetSphere().indexCount);
    }

    void TearDown() override {
        // Context-local handles; reset globals so the engine recompiles in its
        // own context instead of reusing handles that are invalid there.
        ShaderManager::CleanupShaders();
        MeshBuilder::CleanupAll();
    }
};

// ----------------------------------------------------------------------------
// Primitive (MeshComponent) rendering
// ----------------------------------------------------------------------------
TEST_F(ECSRenderGLTest, PrimitiveRendersVisiblePixels) {
    ecs::World world;
    world.init();

    Renderer renderer;
    renderer.Initialize();
    renderer.SetViewport(0, 0, 64, 64);  // must match the window/FBO size

    ecs::RenderSystem sys;
    sys.setWorld(&world);
    sys.setRenderer(&renderer);
    sys.setDefaultShaderProgram(ShaderManager::GetMainShaderProgram());
    world.addStaticSystem(&sys);

    // Spawn a cube at the origin - same components the UI's CreatePrimitive sets.
    auto e = world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    ASSERT_TRUE(e.isValid());
    if (auto* m = world.getComponentArchetype<ecs::MeshComponent>(e)) {
        m->meshType = ecs::MeshType::Cube;
        m->visible = true;
    }

    const glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 3), glm::vec3(0), glm::vec3(0, 1, 0));
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    renderer.SetCameraMatrices(view, proj);
    renderer.SetLightParameters(glm::vec3(5, 5, 5), glm::vec3(0, 0, 3));

    glClearColor(0.1f, 0.2f, 0.9f, 1.0f);  // unmistakable clear color
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    world.render();

    ExpectObjectOccupiesRegion();

    renderer.Shutdown();
    world.shutdown();
}

// ----------------------------------------------------------------------------
// Model (ModelComponent) rendering - uses the model registry + ModelRenderSystem
// ----------------------------------------------------------------------------
TEST_F(ECSRenderGLTest, ModelRendersVisiblePixels) {
    // Asset dependent: skip cleanly when bot.fbx is not available.
    {
        std::ifstream f("assets/bot.fbx");
        if (!f.good()) GTEST_SKIP() << "assets/bot.fbx not present";
    }

    auto handle = ModelSystem::ModelRegistry::getInstance().load("assets/bot.fbx");
    Model* model = ModelSystem::ModelRegistry::getInstance().get(handle);
    if (!model || model->GetMeshCount() == 0) {
        GTEST_SKIP() << "bot.fbx failed to load";
    }

    ecs::World world;
    world.init();

    Renderer renderer;
    renderer.Initialize();
    renderer.SetViewport(0, 0, 64, 64);

    ecs::ModelRenderSystem sys;
    sys.setWorld(&world);
    sys.setRenderer(&renderer);
    sys.setDefaultShaderProgram(ShaderManager::GetMainShaderProgram());
    world.addStaticSystem(&sys);

    auto e = world.createEntityWithComponents<ecs::TransformComponent, ecs::ModelComponent>();
    ASSERT_TRUE(e.isValid());
    if (auto* m = world.getComponentArchetype<ecs::ModelComponent>(e)) {
        m->modelHandle = handle;
        m->visible = true;
        m->setModelPath("assets/bot.fbx");
    }

    const glm::mat4 view = glm::lookAt(glm::vec3(0, 2, 6), glm::vec3(0, 1, 0), glm::vec3(0, 1, 0));
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    renderer.SetCameraMatrices(view, proj);
    renderer.SetLightParameters(glm::vec3(10, 15, 10), glm::vec3(0, 2, 6));

    glClearColor(0.1f, 0.2f, 0.9f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    world.render();

    ExpectObjectOccupiesRegion();

    renderer.Shutdown();
    world.shutdown();
}
