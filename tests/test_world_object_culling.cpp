/**
 * World-object culling tests.
 *
 * WorldObjectManager::update(cameraPos) culls vegetation/rocks farther than
 * the cull radius from the given camera position. The position fed in must
 * be the real follow camera - the EditorApplication fix passes the play
 * camera position instead of the world origin, so nearby objects stay alive
 * as the player walks. These tests lock in the culling mechanism itself
 * (pure CPU math - no GL render calls).
 *
 * The cull is NON-destructive: culled instances are parked in a dormant
 * cache and respawned the moment the camera comes back within the respawn
 * band, so turning around / returning to a previously culled area restores
 * the objects instead of leaving the world empty.
 */

#include <gtest/gtest.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include "../world/SimpleWorldRenderer.h"

namespace {

GLFWwindow* g_cullWin = nullptr;

// Model loading uploads textures, which needs a live GL context. Reuse the
// hidden-window pattern from the other GL tests; skip cleanly (not crash)
// when no context can be created (headless CI without a display server).
bool EnsureGL() {
    static const bool ok = []() {
        if (!glfwInit()) return false;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        g_cullWin = glfwCreateWindow(64, 64, "world-object-culling-test", nullptr, nullptr);
        if (!g_cullWin) { glfwTerminate(); return false; }
        glfwMakeContextCurrent(g_cullWin);
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            glfwDestroyWindow(g_cullWin); g_cullWin = nullptr;
            glfwTerminate(); return false;
        }
        return true;
    }();
    return ok;
}

TEST(WorldObjectCulling, NearObjectsKeptFarObjectsCulled) {
    if (!EnsureGL()) GTEST_SKIP() << "no GL context available";
    SimpleWorldRenderer r;
    const int id = r.loadModel("assets/bot.fbx");
    ASSERT_GE(id, 0) << "bot.fbx must load for culling tests";

    r.addInstance(id, glm::vec3(10.0f, 0.0f, 10.0f), 1.0f);   // ~14m from origin
    r.addInstance(id, glm::vec3(-50.0f, 0.0f, 50.0f), 1.0f);  // ~71m
    r.addInstance(id, glm::vec3(400.0f, 0.0f, 400.0f), 1.0f); // ~566m
    ASSERT_EQ(r.getInstanceCount(), 3u);

    r.cullDistant(glm::vec3(0.0f), 300.0f);
    EXPECT_EQ(r.getInstanceCount(), 2u) << "Objects beyond the cull distance "
                                           "from the camera must be removed";

    r.cullDistant(glm::vec3(0.0f), 30.0f);
    EXPECT_EQ(r.getInstanceCount(), 1u);
}

TEST(WorldObjectCulling, CullingIsRelativeToCameraNotOrigin) {
    if (!EnsureGL()) GTEST_SKIP() << "no GL context available";
    // The same stationary object must survive when the camera is near it and
    // be culled when the camera is far away - even though its distance from
    // the world origin never changes. This is the mechanism the
    // EditorApplication fix relies on for terrain-relative streaming.
    SimpleWorldRenderer r;
    const int id = r.loadModel("assets/bot.fbx");
    ASSERT_GE(id, 0);

    r.addInstance(id, glm::vec3(100.0f, 0.0f, 0.0f), 1.0f);
    ASSERT_EQ(r.getInstanceCount(), 1u);

    // Camera parked 10m from the object: kept.
    r.cullDistant(glm::vec3(90.0f, 0.0f, 0.0f), 50.0f);
    EXPECT_EQ(r.getInstanceCount(), 1u);

    // Camera at the world origin (the pre-fix EditorApplication behavior):
    // the object is 100m away and gets culled even though the player is
    // standing right next to it.
    r.cullDistant(glm::vec3(0.0f), 50.0f);
    EXPECT_EQ(r.getInstanceCount(), 0u);
}

TEST(WorldObjectCulling, CulledObjectsRespawnWhenCameraReturns) {
    if (!EnsureGL()) GTEST_SKIP() << "no GL context available";
    // The whole point of the dormant cache: culling must NOT destroy
    // instances. When the camera turns around / walks back toward a culled
    // object, it must come back - as a cheap cache hit (it is still the same
    // loaded model/matrix, just moved back into the active list).
    SimpleWorldRenderer r;
    const int id = r.loadModel("assets/bot.fbx");
    ASSERT_GE(id, 0);

    r.addInstance(id, glm::vec3(200.0f, 0.0f, 0.0f), 1.0f);
    ASSERT_EQ(r.getInstanceCount(), 1u);

    // Camera at origin: the object is 200m away > 100m cull radius. It must
    // be cached (dormant), not erased.
    r.cullDistant(glm::vec3(0.0f), 100.0f);
    EXPECT_EQ(r.getInstanceCount(), 0u);
    EXPECT_EQ(r.getDormantCount(), 1u) << "culled instance must be cached, not destroyed";

    // Walk back toward it: 60m away is within the default respawn band
    // (85% of 100m), so it respawns as a cache hit.
    r.cullDistant(glm::vec3(140.0f, 0.0f, 0.0f), 100.0f);
    EXPECT_EQ(r.getInstanceCount(), 1u) << "previously culled object must respawn";
    EXPECT_EQ(r.getDormantCount(), 0u);
}

TEST(WorldObjectCulling, HysteresisPreventsBoundaryThrash) {
    if (!EnsureGL()) GTEST_SKIP() << "no GL context available";
    // An object straddling the cull boundary must not flicker in and out as
    // the camera jitters at high speed: it is only cached past maxDist and
    // only respawned once the camera is well inside respawnDist.
    SimpleWorldRenderer r;
    const int id = r.loadModel("assets/bot.fbx");
    ASSERT_GE(id, 0);

    r.addInstance(id, glm::vec3(95.0f, 0.0f, 0.0f), 1.0f);

    // Camera at 200m: object is 105m away > 100m cull radius -> cached.
    r.cullDistant(glm::vec3(200.0f, 0.0f, 0.0f), 100.0f, 80.0f);
    EXPECT_EQ(r.getInstanceCount(), 0u);
    EXPECT_EQ(r.getDormantCount(), 1u);

    // Camera creeps back to 180m (object 85m away): outside the 80m respawn
    // band, so it must stay dormant - no flicker while still far away.
    r.cullDistant(glm::vec3(180.0f, 0.0f, 0.0f), 100.0f, 80.0f);
    EXPECT_EQ(r.getInstanceCount(), 0u);
    EXPECT_EQ(r.getDormantCount(), 1u);

    // Camera at 170m (object 75m away <= 80m): respawns from the cache.
    r.cullDistant(glm::vec3(170.0f, 0.0f, 0.0f), 100.0f, 80.0f);
    EXPECT_EQ(r.getInstanceCount(), 1u);
    EXPECT_EQ(r.getDormantCount(), 0u);
}

} // namespace
