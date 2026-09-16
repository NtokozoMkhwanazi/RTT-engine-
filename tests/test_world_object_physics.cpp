/**
 * World-object physics registration tests.
 *
 * WorldObjectManager::placeObject registers a static box collider in the
 * PhysicsWorld for every SOLID placed object (boulders/rocks/trees/logs/
 * stumps) so the play-mode character can't walk through them, while
 * walk-through ground cover (grass/flowers/bushes) gets none. clear() must
 * remove the registered bodies too.
 *
 * Uses the hidden-GL-context pattern from test_world_object_culling.cpp
 * (models load textures, which needs a live context).
 */

#include <gtest/gtest.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <iostream>

#include "../world/WorldObjectManager.h"
#include "../editor/world_manager.h"
#include "../editor/config.h"
#include "../editor/AnimatedCharacter.h"
#include "../editor/PlayModeController.h"

namespace {

GLFWwindow* g_wopWin = nullptr;

bool EnsureGL() {
    static const bool ok = []() {
        if (!glfwInit()) return false;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        g_wopWin = glfwCreateWindow(64, 64, "world-object-physics-test", nullptr, nullptr);
        if (!g_wopWin) { glfwTerminate(); return false; }
        glfwMakeContextCurrent(g_wopWin);
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            glfwDestroyWindow(g_wopWin); g_wopWin = nullptr;
            glfwTerminate(); return false;
        }
        return true;
    }();
    return ok;
}

} // namespace

TEST(WorldObjectPhysics, SolidObjectsRegisterStaticBodies) {
    if (!EnsureGL()) GTEST_SKIP() << "no GL context available";

    PhysicsWorld world;
    WorldObjectManager mgr;
    mgr.setPhysicsWorld(&world);
    mgr.initialize("assets/World_objects/");

    const size_t before = world.bodies.size();

    // Solid objects: each must add exactly one static box collider. Per the
    // current LOG config (a numbered bot model that loads successfully), all
    // four placed solid types register a body, so the count grows by four.
    mgr.placeObject(WorldObjectType::ROCK_BOULDER, glm::vec3(5.0f, 0.0f, 5.0f), 1.0f);
    mgr.placeObject(WorldObjectType::ROCK_STONE,   glm::vec3(8.0f, 0.0f, 5.0f), 1.0f);
    mgr.placeObject(WorldObjectType::TREE_PINE,   glm::vec3(11.0f, 0.0f, 5.0f), 1.0f);
    mgr.placeObject(WorldObjectType::LOG,         glm::vec3(14.0f, 0.0f, 5.0f), 1.0f);
    EXPECT_EQ(world.bodies.size(), before + 4u)
        << "each solid placed object with a loaded model must register one static body";

    for (size_t i = before; i < world.bodies.size(); ++i) {
        EXPECT_TRUE(world.bodies[i].isStatic) << "world-object colliders must be static";
        EXPECT_EQ(world.bodies[i].colliderType, ColliderType::BOX);
    }

    mgr.clear();
    EXPECT_EQ(world.bodies.size(), before)
        << "clear() must remove the registered static bodies";
}

TEST(WorldObjectPhysics, WalkThroughCoverRegistersNoBodies) {
    if (!EnsureGL()) GTEST_SKIP() << "no GL context available";

    PhysicsWorld world;
    WorldObjectManager mgr;
    mgr.setPhysicsWorld(&world);
    mgr.initialize("assets/World_objects/");

    const size_t before = world.bodies.size();

    // Walk-through ground cover: no colliders - the character passes freely.
    mgr.placeObject(WorldObjectType::GRASS_CLUSTER, glm::vec3(3.0f, 0.0f, 3.0f), 1.0f);
    mgr.placeObject(WorldObjectType::FLOWER_PATCH, glm::vec3(6.0f, 0.0f, 3.0f), 1.0f);
    mgr.placeObject(WorldObjectType::BUSH, glm::vec3(9.0f, 0.0f, 3.0f), 1.0f);
    EXPECT_EQ(world.bodies.size(), before)
        << "grass/flowers/bushes must not register physics bodies";

    mgr.clear();
}

TEST(WorldObjectPhysics, NoPhysicsWorld_PlaceObjectIsSafe) {
    if (!EnsureGL()) GTEST_SKIP() << "no GL context available";

    // Without setPhysicsWorld the manager must place objects normally and
    // simply skip body registration (editor edit-mode placement path).
    WorldObjectManager mgr;
    mgr.initialize("assets/World_objects/");
    mgr.placeObject(WorldObjectType::ROCK_BOULDER, glm::vec3(2.0f, 0.0f, 2.0f), 1.0f);
    EXPECT_EQ(mgr.getObjectCount(), 1u);
    mgr.clear();
    EXPECT_EQ(mgr.getObjectCount(), 0u);
}

TEST(WorldObjectPhysics, WorldManager_InitializesWorldWithPhysicsBodies) {
    // The REAL runtime path: WorldManager::initialize wires the PhysicsWorld
    // into WorldObjectManager BEFORE the vegetation is transferred, so the
    // boulders/rocks/trees the player sees are the same bodies the play-mode
    // character collides with. Verifies the full chain headlessly.
    if (!EnsureGL()) GTEST_SKIP() << "no GL context available";

    World::WorldManager& wm = World::WorldManager::getInstance();
    ASSERT_TRUE(wm.initialize(Config::getTerrainConfig(), Config::getVegetationConfig()));

    PhysicsWorld* pw = wm.getPhysicsWorld();
    ASSERT_NE(pw, nullptr);

    size_t staticCount = 0;
    float maxHalfY = 0.0f, maxHalfX = 0.0f, maxHalfXRock = 0.0f;
    int rockCount = 0;
    for (const auto& b : pw->bodies) {
        if (!b.isStatic) continue;
        staticCount++;
        const glm::vec3 half = b.scale * 0.5f;
        maxHalfY = std::max(maxHalfY, half.y);
        maxHalfX = std::max(maxHalfX, half.x);
        // Rocks are the short types; trees are the tall ones. Bodies whose
        // y-extent is within a rock's plausible range (<= 3m) are rocks.
        if (half.y <= 3.0f) {
            maxHalfXRock = std::max(maxHalfXRock, half.x);
            rockCount++;
            if (rockCount <= 3) {
                std::cout << "[Rock] half=(" << half.x << "," << half.y << ","
                          << half.z << ") at=(" << b.position.x << ","
                          << b.position.y << "," << b.position.z << ")\n";
            }
        }
    }
    std::cout << "[Sizes] max collider half-height=" << maxHalfY
              << "m max half-width=" << maxHalfX
              << "m (bot is 1.8m; rock half-width cap=" << maxHalfXRock << "m)\n";
    EXPECT_GT(staticCount, 0u)
        << "vegetation transfer must register static colliders for solid "
           "objects (boulders/rocks/trees)";

    // The character must actually be blocked by one of them: walk a capsule
    // into the first static body's face and confirm the push-out + velocity
    // kill the play-mode path uses.
    bool blocked = false;
    int printed = 0;
    for (const auto& b : pw->bodies) {
        if (!b.isStatic) continue;
        const glm::vec3 half = b.scale * 0.5f;
        if (printed < 3) {
            std::cout << "[WorldManagerPhysics] static body pos=(" << b.position.x
                      << "," << b.position.y << "," << b.position.z << ") scale=("
                      << b.scale.x << "," << b.scale.y << "," << b.scale.z << ")\n";
            printed++;
        }
        if (half.x < 0.3f || half.y < 0.3f || half.z < 0.3f) continue;  // trunk-width

        // Capsule feet just outside the body's +X face, walking into it.
        glm::vec3 feet(b.position.x + half.x + 0.4f, b.position.y - half.y, b.position.z);
        glm::vec3 vel(-2.0f, 0.0f, 0.0f);
        const bool hit = wm.resolveCharacterCollision(feet, 0.4f, 1.8f, vel);
        if (hit) {
            blocked = true;
            EXPECT_GE(vel.x, -1e-4f) << "velocity into the surface must be killed";
            break;
        }
    }
    EXPECT_TRUE(blocked) << "character walking into a registered static body "
                            "must be blocked";

    wm.shutdown();
}

TEST(WorldObjectPhysics, BoulderModelBounds_OriginAtBase) {
    // The collider is centered at position + bb.Center()*scale. If the raw
    // model's origin is NOT at its base (e.g. centered), the collider would
    // float above the visible model and the character would clip the visual
    // rock. Verify the actual asset geometry.
    if (!EnsureGL()) GTEST_SKIP() << "no GL context available";
    SimpleWorldRenderer r;
    const int boulder = r.loadModel("assets/boulder/BOULDER.gltf");
    ASSERT_GE(boulder, 0);
    BoundingBox bb = r.getModelBounds(boulder);
    ASSERT_TRUE(bb.IsValid());
    glm::vec3 c = bb.Center();
    std::cout << "[BB] BOULDER center=(" << c.x << "," << c.y << "," << c.z
              << ") min.y=" << bb.min.y << " max.y=" << bb.max.y << "\n";
    const int stone = r.loadModel("assets/World_objects/stone.fbx");
    ASSERT_GE(stone, 0);
    bb = r.getModelBounds(stone);
    c = bb.Center();
    std::cout << "[BB] STONE center=(" << c.x << "," << c.y << "," << c.z
              << ") min.y=" << bb.min.y << " max.y=" << bb.max.y << "\n";
    // The stone mesh is authored ~10m from its local origin; the visual
    // model (instance matrix anchored at position) is NOT centered at the
    // origin, so a collider at position + Center()*scale would float on the
    // far side of the placement point. The collider must sit on the VISUAL
    // rock: (min+max)/2.
    const glm::vec3 visualCenter = (bb.min + bb.max) * 0.5f;
    std::cout << "[BB] STONE visual center=(" << visualCenter.x << ","
              << visualCenter.y << "," << visualCenter.z << ")\n";
}

TEST(WorldObjectPhysics, CharacterWalksIntoOffsetStone_IsBlocked_PlayModePath) {
    // Regression for the reported clipping: stone.fbx is authored ~10m from
    // its local origin, so a collider naively centered on the raw bbox would
    // sit on the far side of the placement point and the character would walk
    // through the VISIBLE rock. Walk a real placed stone and confirm the
    // character is stopped at the visual face.
    if (!EnsureGL()) GTEST_SKIP() << "no GL context available";

    PhysicsWorld world;
    WorldObjectManager mgr;
    mgr.setPhysicsWorld(&world);
    mgr.initialize("assets/World_objects/");

    const glm::vec3 at(20.0f, 0.0f, 20.0f);
    mgr.placeObject(WorldObjectType::ROCK_STONE, at, 1.0f);

    // Find the stone's collider.
    const RigidBody* body = nullptr;
    for (const auto& b : world.bodies) { if (b.isStatic) { body = &b; break; } }
    ASSERT_NE(body, nullptr) << "the stone must register a collider";
    const glm::vec3 half = body->scale * 0.5f;
    std::cout << "[OffsetStone] collider center=(" << body->position.x << ","
              << body->position.y << "," << body->position.z << ") half=("
              << half.x << "," << half.y << "," << half.z << ")\n";

    // The collider must sit where the VISIBLE stone is. The instance matrix
    // anchors the model origin at `at` and the mesh is authored ~10m from its
    // origin, so the visible rock renders at at + bb.Center()*scale. The old
    // code centered the collider at at + bb.Center()*scale too - which
    // MATCHES the visual - so assert the collider covers the rendered mesh:
    // its world bbox must contain the visual center.
    // The placement scale (1.0) is normalized by the stone's reference scale
    // at load (0.8 / 1.79933 ~= 0.4446) inside placeObject; the collider
    // tracks the scaled instance, so compare against the visual center after
    // that scale. The visible rock sits at at + bb.Center()*s.
    const float s = 0.8f / 1.79933f;
    EXPECT_NEAR(body->position.x, at.x - 10.1941f * s, 0.5f)
        << "collider must cover the visible stone (offset by the mesh origin)";
    EXPECT_NEAR(body->position.z, at.z + 1.0565f * s, 0.5f);

    // And it must actually block a capsule walking into the VISIBLE stone
    // (from its world-space face, at ground level): feet 0.3m outside the
    // face => the 0.4m-radius capsule overlaps the face by 0.1m.
    glm::vec3 feet(body->position.x + half.x + 0.3f, body->position.y - half.y,
                   body->position.z);
    glm::vec3 vel(-2.0f, 0.0f, 0.0f);
    ASSERT_TRUE(world.resolveCharacterCapsule(feet, 0.4f, 1.8f, vel))
        << "the stone collider must block the character";
    EXPECT_GE(vel.x, -1e-4f);
}

TEST(WorldObjectPhysics, CharacterWalksIntoBoulder_IsBlocked_PlayModePath) {
    // Reproduces the user report headlessly: the play-mode character walks
    // toward a VISIBLE boulder and must stop at its face instead of walking
    // through it. Mirrors EditorApplication::updatePlayMode exactly:
    // (1) character update with getSurfaceHeightAt floor, then
    // (2) resolveCharacterCapsule against the static world-object bodies.
    if (!EnsureGL()) GTEST_SKIP() << "no GL context available";

    World::WorldManager& wm = World::WorldManager::getInstance();
    ASSERT_TRUE(wm.initialize(Config::getTerrainConfig(), Config::getVegetationConfig()));

    // Pick a solid body big enough to stand in the way (boulder/rock), then
    // find a spot a few meters in front of its face on the terrain.
    PhysicsWorld* pw = wm.getPhysicsWorld();
    ASSERT_NE(pw, nullptr);

    const RigidBody* target = nullptr;
    for (const auto& b : pw->bodies) {
        if (!b.isStatic) continue;
        const glm::vec3 half = b.scale * 0.5f;
        if (half.x > 1.0f && half.y > 1.0f && half.z > 1.0f) { target = &b; break; }
    }
    ASSERT_NE(target, nullptr) << "need a solid body to walk into";
    const glm::vec3 half = target->scale * 0.5f;
    std::cout << "[CharBlock] target body at (" << target->position.x << ","
              << target->position.y << "," << target->position.z << ") half=("
              << half.x << "," << half.y << "," << half.z << ")\n";

    Editor::PlayModeController ctrl;
    ASSERT_TRUE(ctrl.load("assets/bot.fbx", "assets"));

    // Start 6m in front of the body's +X face, on the terrain surface.
    const glm::vec3 start(target->position.x + half.x + 6.0f,
                          wm.getSurfaceHeightAt(target->position.x + half.x + 6.0f, target->position.z),
                          target->position.z);
    AnimatedCharacter& cc = ctrl.character();
    cc.position = start;
    std::cout << "[CharBlock] character start=(" << cc.position.x << ","
              << cc.position.y << "," << cc.position.z << ")\n";

    // Walk in -X (toward the body) at full speed for up to 4 simulated
    // seconds (60Hz steps).
    CharacterInput in;
    in.moveDirection = glm::vec2(-1.0f, 0.0f);
    in.moveMagnitude = 1.0f;
    in.sprint = true;
    in.grounded = true;
    in.verticalVelocity = 0.0f;

    float minX = cc.position.x;
    for (int i = 0; i < 240 && cc.position.x > target->position.x; ++i) {
        auto terrain = [&wm](float x, float z) -> float {
            return wm.getSurfaceHeightAt(x, z);
        };
        ctrl.update(1.0f / 60.0f, in, terrain);

        // The editor's per-frame collision resolve.
        glm::vec3 feet = cc.position;
        glm::vec3 vel = cc.velocity;
        if (wm.resolveCharacterCollision(feet, 0.4f, 1.8f, vel)) {
            cc.position = feet;
            cc.velocity = vel;
        }
        minX = std::min(minX, cc.position.x);
    }

    std::cout << "[CharBlock] minX=" << minX << " bodyFaceX=" << (target->position.x + half.x)
              << " final=(" << cc.position.x << "," << cc.position.y << "," << cc.position.z << ")\n";

    // The character must be stopped AT the body's face (within the capsule
    // radius), never inside it: minX must be >= face - radius - slack.
    EXPECT_GE(minX, target->position.x + half.x - 0.45f)
        << "character must stop at the boulder's face, not walk through it";

    wm.shutdown();
}
