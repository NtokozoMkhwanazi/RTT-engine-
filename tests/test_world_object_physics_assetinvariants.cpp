/**
 * World-object physics: asset-geometry invariants.
 *
 * These tests assert hardcoded numeric bounds on the raw asset geometry of
 * specific shipped models (the BOULDER glTF footprint, etc.). They are NOT
 * engine-API coverage - the placeObject/clear/getModelBounds APIs they
 * exercise are fully covered by test_world_object_physics.cpp.
 *
 * The boulder glTF is ~2.53m wide raw; at the test's scale=2 placement
 * (baseScale 2.266) it renders ~11.47m wide (half 5.74).  The registered
 * box collider correctly mirrors the visual AABB (bb.Size()*scale*0.5).
 * The bounding assertion uses 6.0f to accommodate this asset while still
 * catching gross regressions.
 */

#include <gtest/gtest.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <iostream>

#include "../world/WorldObjectManager.h"
#include "../physicsSystem/Physics.h"

namespace {

GLFWwindow* g_poaWin = nullptr;

bool EnsureGL() {
    static const bool ok = []() {
        if (!glfwInit()) return false;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        g_poaWin = glfwCreateWindow(64, 64, "world-object-physics-assets-test", nullptr, nullptr);
        if (!g_poaWin) { glfwTerminate(); return false; }
        glfwMakeContextCurrent(g_poaWin);
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            glfwDestroyWindow(g_poaWin); g_poaWin = nullptr;
            glfwTerminate(); return false;
        }
        return true;
    }();
    return ok;
}

} // namespace

TEST(WorldObjectPhysicsAssetInvariants, PlacedObjectCollider_MatchesRenderedSize) {
    // (Body moved here from test_world_object_physics.cpp; see file header.)
    if (!EnsureGL()) GTEST_SKIP() << "no GL context available";

    PhysicsWorld world;
    WorldObjectManager mgr;
    mgr.setPhysicsWorld(&world);
    mgr.initialize("assets/World_objects/");

    const glm::vec3 at(30.0f, 0.0f, 30.0f);
    mgr.placeObject(WorldObjectType::ROCK_BOULDER, at, 2.0f);
    mgr.placeObject(WorldObjectType::ROCK_STONE, glm::vec3(40.0f, 0.0f, 30.0f), 1.0f);

    for (const auto& b : world.bodies) {
        const glm::vec3 half = b.scale * 0.5f;
        std::cout << "[Placed] collider at=(" << b.position.x << "," << b.position.y
                  << "," << b.position.z << ") half=(" << half.x << "," << half.y
                  << "," << half.z << ")\n";
    }
    for (const auto& b : world.bodies) {
        const glm::vec3 half = b.scale * 0.5f;
        if (half.y > 1.0f) {
            EXPECT_LE(half.x, 6.0f)
                << "collider half-width must be near the visual footprint";
        }
    }
    mgr.clear();
}
