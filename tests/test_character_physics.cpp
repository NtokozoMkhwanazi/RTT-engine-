/**
 * Character-vs-world-object physics tests.
 *
 * WorldObjectManager registers a static box collider per solid placed object
 * (boulder/rock/tree trunk/log/stump). The play-mode character is NOT a body
 * in the world - EditorApplication moves it, then calls
 * PhysicsWorld::resolveCharacterCapsule to push its feet out of any overlap
 * and kill the velocity component heading into the surface, so it slides
 * along / stops at rocks instead of walking through them. getStaticSurfaceHeightAt
 * lets it stand ON boulders (caller takes max(terrain, this)).
 *
 * Lives in its own file: test_physics.cpp defines a self-contained
 * ColliderType struct that would collide with the real enum from Physics.h.
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <limits>

#include "../physicsSystem/Physics.h"

namespace {

// A static world-object body: box collider at `center`, full extents `size`
// (the PhysicsWorld treats RigidBody::scale as full extents, /2 for the
// half-extents AABB). Mirrors WorldObjectManager::registerPhysicsBody.
RigidBody makeStaticBox(const glm::vec3& center, const glm::vec3& size) {
    RigidBody b;
    b.position = center;
    b.scale = size;
    b.mass = 0.0f;
    b.isStatic = true;
    b.colliderType = ColliderType::BOX;
    b.restitution = 0.0f;
    b.friction = 0.8f;
    return b;
}

} // namespace

TEST(CharacterPhysics, Capsule_BlockedByBoulder_PushedOutAndVelocityKilled) {
    PhysicsWorld world;
    // Boulder resting on the floor, occupying x/z [-2, 2], y [0, 4]
    // (center (0,2,0), half extents (2,2,2)).
    world.addBody(makeStaticBox(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(4.0f, 4.0f, 4.0f)));

    // Character capsule (radius 0.4, height 1.8) walking INTO the boulder
    // from the +X side: feet at x=2.2 => the capsule's x range [1.8, 2.6]
    // overlaps the boulder face at x=2.0 by 0.6 > radius.
    glm::vec3 feet(2.2f, 0.0f, 0.0f);
    glm::vec3 vel(-2.0f, 0.0f, 0.0f);   // heading into the boulder

    ASSERT_TRUE(world.resolveCharacterCapsule(feet, 0.4f, 1.8f, vel))
        << "capsule overlapping the boulder must register a hit";

    // Pushed out of the overlap: the capsule's inner edge (feet.x + radius)
    // must clear the boulder face (x=2.0).
    EXPECT_GE(feet.x + 0.4f, 2.0f + 1e-4f)
        << "feet must be pushed out of the boulder, not left inside";

    // The velocity component heading INTO the surface is killed: the
    // character stops at / slides along the rock instead of walking through.
    EXPECT_GE(vel.x, -1e-4f) << "x velocity into the boulder must be removed";
    EXPECT_FLOAT_EQ(vel.y, 0.0f) << "vertical velocity must be untouched";
}

TEST(CharacterPhysics, Capsule_NoOverlap_NotBlocked) {
    PhysicsWorld world;
    world.addBody(makeStaticBox(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(4.0f, 4.0f, 4.0f)));

    // Capsule 4m clear of the boulder face (feet.x = 6.0 > 2.0 + 0.4 + slack).
    glm::vec3 feet(6.0f, 0.0f, 0.0f);
    glm::vec3 vel(-2.0f, 0.0f, 0.0f);
    EXPECT_FALSE(world.resolveCharacterCapsule(feet, 0.4f, 1.8f, vel))
        << "capsule clear of the boulder must not be blocked";
    EXPECT_FLOAT_EQ(feet.x, 6.0f) << "feet must not move";
    EXPECT_FLOAT_EQ(vel.x, -2.0f) << "velocity must not be touched";
}

TEST(CharacterPhysics, Capsule_WalksBesideTreeTrunk_NotBlockedByCanopy) {
    // Trees register a trunk-width collider (WorldObjectManager narrows the
    // box to the trunk so the character walks under/past the foliage). A
    // capsule beside the trunk is free; one walking INTO the trunk column is
    // blocked - the case that used to NaN (capsule segment axis-aligned with
    // the box face made the old per-axis slab code divide by zero, so the
    // collision silently never happened).
    PhysicsWorld world;
    // Trunk column: center (0, 2, 0), half extents (0.3, 2, 0.3).
    world.addBody(makeStaticBox(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.6f, 4.0f, 0.6f)));

    // Beside the trunk: no overlap.
    glm::vec3 feet(5.0f, 0.0f, 0.0f);
    glm::vec3 vel(-2.0f, 0.0f, 0.0f);
    EXPECT_FALSE(world.resolveCharacterCapsule(feet, 0.4f, 1.8f, vel));

    // Into the trunk: blocked.
    feet = glm::vec3(0.5f, 0.0f, 0.0f);
    vel = glm::vec3(-2.0f, 0.0f, 0.0f);
    ASSERT_TRUE(world.resolveCharacterCapsule(feet, 0.4f, 1.8f, vel))
        << "axis-aligned capsule vs trunk must collide (no NaN)";
    EXPECT_GE(feet.x + 0.4f, 0.3f + 1e-4f) << "feet must be pushed out of the trunk";
}

TEST(CharacterPhysics, StaticSurfaceHeight_TopOfBoulder_ReturnsBoulderTop) {
    PhysicsWorld world;
    // Boulder center (0,5,0), half (2,2,2): top face at y=7, footprint
    // x/z [-2, 2].
    world.addBody(makeStaticBox(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(4.0f, 4.0f, 4.0f)));

    // Above the footprint: the boulder top (the caller takes max(terrain,
    // this) so the character stands ON the rock).
    EXPECT_FLOAT_EQ(world.getStaticSurfaceHeightAt(0.0f, 0.0f), 7.0f);
    EXPECT_FLOAT_EQ(world.getStaticSurfaceHeightAt(1.5f, -1.5f), 7.0f);

    // Outside the footprint: no static body covers the point.
    EXPECT_FLOAT_EQ(world.getStaticSurfaceHeightAt(3.0f, 0.0f),
                    -std::numeric_limits<float>::infinity());

    // Overlapping static bodies: the highest one wins.
    world.addBody(makeStaticBox(glm::vec3(0.0f, 9.0f, 0.0f), glm::vec3(1.0f, 1.0f, 1.0f)));
    EXPECT_FLOAT_EQ(world.getStaticSurfaceHeightAt(0.0f, 0.0f), 9.5f);
}
