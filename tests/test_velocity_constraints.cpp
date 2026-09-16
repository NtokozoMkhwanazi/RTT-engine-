/**
 * Velocity-level constraint solver tests (physics proposal in `todo`).
 *
 * Verifies the velocity-Level Constraint Framework via Jacobians that
 * VelocityConstraints.h provides as the replacement for the legacy
 * position-based constraints in physicsSystem/Constraint.cpp:
 *   - PlaneConstraint (§1): a 1D linear Jacobian that forbids a body from
 *     accelerating into a plane AND preserves angular velocity (JwA = 0),
 *     with split-impulse position correction that resolves penetration
 *     without injecting kinetic energy.
 *   - DistanceConstraint: a ball-and-swing joint that preserves rest length
 *     via the (contiguous) CompressedDistanceConstraint + Jacobian row.
 *
 * These run without a GL context — pure CPU dynamics on RigidBody.
 */
#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

#include "physicsSystem/VelocityConstraints.h"
#include "physicsSystem/Physics.h"   // PhysicsWorld integration smoke test

namespace {
float dot3(const glm::vec3& a, const glm::vec3& b) { return glm::dot(a, b); }

RigidBody makeSphere(const glm::vec3& pos, float mass = 1.0f) {
    return RigidBody(pos, glm::vec3(1.0f), mass, false, ColliderType::SPHERE);
}

// Ground plane at z = 0, normal pointing +Z (into the body side).
// A body is "inside" when n . x + d < 0  =>  penetration C = -(n.x + d) > 0.
constexpr glm::vec3 kPlaneN = glm::vec3(0.0f, 0.0f, 1.0f);
constexpr float     kPlaneD = 0.0f; // planeDistance
} // namespace

TEST(PlaneConstraint, StopsIntoPlaneVelocityAndPreservesAngular) {
    std::vector<RigidBody> bodies;
    bodies.push_back(makeSphere(glm::vec3(0.0f, 0.0f, -0.5f), 1.0f));
    bodies.back().velocity = glm::vec3(0.0f, 0.0f, -5.0f);          // moving INTO plane
    bodies.back().angularVelocity = glm::vec3(0.2f, 0.3f, 0.4f);    // spin must survive

    const glm::vec3 w0 = bodies[0].angularVelocity;
    const float dt = 0.016f;

    vel::ConstraintSolver solver(bodies);
    solver.addPlaneConstraint(0, kPlaneN, kPlaneD, /*restitution=*/0.0f);
    solver.solve(dt, /*velocityIterations=*/8, /*positionIterations=*/8);

    // (1) The body must not be moving into the plane afterwards.
    EXPECT_GE(dot3(kPlaneN, bodies[0].velocity), -1e-4f)
        << "plane constraint must forbid acceleration into the plane; v_rel="
        << dot3(kPlaneN, bodies[0].velocity);

    // (2) Angular velocity is preserved (JwA = 0 -> no torque applied).
    EXPECT_NEAR(bodies[0].angularVelocity.x, w0.x, 1e-5f);
    EXPECT_NEAR(bodies[0].angularVelocity.y, w0.y, 1e-5f);
    EXPECT_NEAR(bodies[0].angularVelocity.z, w0.z, 1e-5f);

    // (3) Split-impulse position correction pushed the body out of the plane.
    EXPECT_GT(bodies[0].position.z, -0.5f);
}

TEST(PlaneConstraint, SplitImpulseResolvesPenetrationWithoutAddingVelocity) {
    std::vector<RigidBody> bodies;
    bodies.push_back(makeSphere(glm::vec3(0.0f, 0.0f, -0.3f), 1.0f));
    bodies.back().velocity = glm::vec3(0.0f);
    bodies.back().angularVelocity = glm::vec3(0.1f, 0.2f, 0.3f);

    const glm::vec3 w0 = bodies[0].angularVelocity;
    const glm::vec3 v0 = bodies[0].velocity;

    vel::ConstraintSolver solver(bodies);
    solver.addPlaneConstraint(0, kPlaneN, kPlaneD, 0.0f);
    solver.solve(0.016f, 8, 8);

    // A body at rest that only needed a position fix must not gain velocity.
    EXPECT_NEAR(bodies[0].velocity.x, v0.x, 1e-9f);
    EXPECT_NEAR(bodies[0].velocity.y, v0.y, 1e-9f);
    EXPECT_NEAR(bodies[0].velocity.z, v0.z, 1e-9f);
    EXPECT_NEAR(bodies[0].angularVelocity.x, w0.x, 1e-9f);
    EXPECT_NEAR(bodies[0].angularVelocity.y, w0.y, 1e-9f);
    EXPECT_NEAR(bodies[0].angularVelocity.z, w0.z, 1e-9f);

    // And the penetration was reduced (body lifted out of the plane).
    EXPECT_GT(bodies[0].position.z, -0.3f);
}

TEST(DistanceConstraint, PreservesRestLength) {
    std::vector<RigidBody> bodies;
    bodies.push_back(makeSphere(glm::vec3(-1.0f, 0.0f, 0.0f), 1.0f));
    bodies.push_back(makeSphere(glm::vec3( 1.0f, 0.0f, 0.0f), 1.0f));
    // Anchors at each body's COM -> r = 0 -> Jw = 0 (linear-only joint).
    const glm::vec3 anchorA = bodies[0].position;
    const glm::vec3 anchorB = bodies[1].position;
    const float restLength = 1.0f; // started 2.0 apart => must contract

    vel::ConstraintSolver solver(bodies);
    solver.addDistanceConstraint(
        vel::ConstraintSolver::DistanceParams{0, 1, restLength, /*stiffness=*/1.0f},
        anchorA, anchorB);
    solver.solve(0.016f, /*velocityIterations=*/8, /*positionIterations=*/64);

    const float dist = glm::length(bodies[1].position - bodies[0].position);
    EXPECT_NEAR(dist, restLength, 0.05f)
        << "distance joint must drive the bodies to rest length; dist=" << dist;

    // No spurious velocity injected (both bodies started at rest, Jw = 0).
    EXPECT_NEAR(glm::length(bodies[0].velocity), 0.0f, 1e-5f);
    EXPECT_NEAR(glm::length(bodies[1].velocity), 0.0f, 1e-5f);
}

TEST(ConstraintSolver, FlatLayoutStaticAsserts) {
    // The contiguous data-layout struct from suggestions.txt §4.
    EXPECT_EQ(sizeof(vel::CompressedDistanceConstraint), 48u);
    // bodyA/bodyB at known offsets so a SIMD gather matches the C++ struct.
    EXPECT_EQ(offsetof(vel::CompressedDistanceConstraint, bodyA), 0u);
    EXPECT_EQ(offsetof(vel::CompressedDistanceConstraint, anchorA), 24u);
    EXPECT_EQ(offsetof(vel::CompressedDistanceConstraint, anchorB), 36u);
}

// =============================================================================
// §2 5-DOF HingeConstraint (block mass matrix J M^-1 J^T, 5x5)
// =============================================================================
//
// A hinge removes 3 translational + 2 rotational DOFs (the two swing axes
// perpendicular to the hinge axis), leaving exactly ONE free rotational DOF —
// spin about the hinge axis. The block solver below couples the 5 Jacobian
// rows of a single joint and solves them simultaneously through the 5x5
// effective-mass matrix, instead of one scalar PGS pass at a time.

TEST(HingeConstraint, FreesAxisSpinAndKillsPerpendicularAngular) {
    std::vector<RigidBody> bodies;
    // A: static ground pinned at the hinge pivot.
    bodies.push_back(RigidBody(glm::vec3(0, 0, 0), glm::vec3(1, 1, 1),
                               1.0f, /*isStatic=*/true, ColliderType::SPHERE));
    // B: free body sitting ON the pivot, with both an allowed (z) spin and
    // forbidden (x,y) perpendicular spin.
    bodies.push_back(makeSphere(glm::vec3(0, 0, 0), 1.0f));
    bodies[1].velocity = glm::vec3(0.0f);
    bodies[1].angularVelocity = glm::vec3(0.3f, 0.1f, 0.5f);

    const float wz0 = bodies[1].angularVelocity.z;
    const glm::vec3 v0 = bodies[1].velocity;

    vel::ConstraintSolver solver(bodies);
    solver.addHingeConstraint(0, 1, glm::vec3(0, 0, 0), glm::vec3(0, 0, 1));
    solver.solveHinges(0.016f, /*velocityIterations=*/8, /*positionIterations=*/1);

    // Translational DOFs pinned (pivot held): linear velocity killed.
    EXPECT_NEAR(bodies[1].velocity.x, v0.x, 1e-5f);
    EXPECT_NEAR(bodies[1].velocity.y, v0.y, 1e-5f);
    EXPECT_NEAR(bodies[1].velocity.z, v0.z, 1e-5f);
    EXPECT_NEAR(glm::length(bodies[1].velocity), 0.0f, 1e-4f);

    // Forbidden perpendicular spin removed; about-axis spin preserved.
    EXPECT_NEAR(bodies[1].angularVelocity.x, 0.0f, 1e-3f);
    EXPECT_NEAR(bodies[1].angularVelocity.y, 0.0f, 1e-3f);
    EXPECT_NEAR(bodies[1].angularVelocity.z, wz0, 1e-4f);

    // And the anchor points stayed coincident (point-to-point constraint).
    const glm::vec3 anchorA = bodies[0].position;            // pivot == A's COM
    const glm::vec3 anchorB = bodies[1].position;            // pivot == B's COM
    EXPECT_NEAR(glm::length(anchorB - anchorA), 0.0f, 1e-5f);
}

TEST(HingeConstraint, ConservesAngularMomentum_AboutAxisOnly) {
    std::vector<RigidBody> bodies;
    bodies.push_back(makeSphere(glm::vec3(-1.0f, 0, 0), 1.0f)); // A
    bodies.push_back(makeSphere(glm::vec3( 1.0f, 0, 0), 1.0f)); // B
    // Shared pivot at the origin; hinge axis = z. Anchors at each COM so r=0.
    bodies[0].angularVelocity = glm::vec3(0.0f, 0.0f,  0.4f);   // spin about axis (free)
    bodies[1].angularVelocity = glm::vec3(0.3f, 0.0f, -0.4f);   //  x: swing (removed), z: spin (free)
    // No initial velocity. Both bodies touch neither (1m apart, r<0.5).

    const glm::vec3 wa0 = bodies[0].angularVelocity;
    const glm::vec3 wb0 = bodies[1].angularVelocity;
    // Total angular momentum ∝ (wa + wb) for equal isotropic spheres.
    const glm::vec3 L0 = wa0 + wb0;            // (0.3, 0, 0)
    const float relZ0 = wb0.z - wa0.z;         // relative spin about axis (-0.8)

    vel::ConstraintSolver solver(bodies);
    solver.addHingeConstraint(0, 1, glm::vec3(0, 0, 0), glm::vec3(0, 0, 1));
    solver.solveHinges(0.016f, /*velocityIterations=*/16, /*positionIterations=*/1);

    const glm::vec3 L1 = bodies[0].angularVelocity + bodies[1].angularVelocity;
    const glm::vec3 rel1 = bodies[1].angularVelocity - bodies[0].angularVelocity;

    // Relative swing (x,y) removed; about-axis relative spin unchanged.
    EXPECT_NEAR(rel1.x, 0.0f, 1e-3f);
    EXPECT_NEAR(rel1.y, 0.0f, 1e-3f);
    EXPECT_NEAR(rel1.z, relZ0, 1e-3f);
    // Total angular momentum conserved (no external torque in the velocity pass).
    EXPECT_NEAR((L1 - L0).x, 0.0f, 1e-3f);
    EXPECT_NEAR((L1 - L0).z, 0.0f, 1e-3f);
}

TEST(HingeConstraint, PivotCoincidenceUnderGravity) {
    // End-to-end through PhysicsWorld::step (opt-in 5-DOF hinge block pass).
    PhysicsWorld world;
    world.gravity = glm::vec3(0.0f);          // isolate from gravity
    world.floor.enabled = false;              // isolate from the ground plane
    world.useVelocityConstraints = true;      // run the vel::ConstraintSolver hinge pass

    // A: static anchor body at the origin.
    BodyHandle ha = world.addBody(RigidBody(glm::vec3(0, 0, 0), glm::vec3(0.15f),
                                            1.0f, /*isStatic=*/true, ColliderType::SPHERE));
    // B: free body 1m away, no overlap (distance 1.0 > 0.15+0.15). The hinge
    // pivot is placed at B's COM (rB == 0) so the about-axis spin (z) is a
    // clean free DOF: the translational rows cannot leak into ωz.
    RigidBody bBody(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.15f),
                    1.0f, /*isStatic=*/false, ColliderType::SPHERE);
    bBody.angularDamping = 1.0f;             // no damping — observe the hinge cleanly
    bBody.linearDamping   = 1.0f;
    bBody.angularVelocity = glm::vec3(0.3f, 0.0f, 0.5f); // perp swing killed, z spin free
    bBody.velocity        = glm::vec3(0.0f);
    BodyHandle hb = world.addBody(bBody);

    // Pivot at B's COM (1,0,0); hinge axis = world z. anchorA on A is offset.
    world.addHingeConstraint(ha, hb, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0, 0, 1));
    world.step(0.016f);

    const RigidBody* A = world.getBody(ha);
    const RigidBody* B = world.getBody(hb);
    ASSERT_NE(A, nullptr);
    ASSERT_NE(B, nullptr);

    // Pivot coincidence: A's anchor (posA + rA) == B's anchor (== B's COM).
    const glm::mat3 RA = glm::mat3_cast(A->rotation);
    const glm::vec3 anchorA = A->position + RA * glm::vec3(1.0f, 0.0f, 0.0f); // pivot on A
    const glm::vec3 anchorB = B->position;                                    // rB == 0
    EXPECT_NEAR(glm::distance(anchorB, anchorA), 0.0f, 1e-4f)
        << "hinge pivot must stay coincident (5-DOF block constraint)";

    // Forbidden perpendicular spin killed by the 2 rotational rows.
    EXPECT_NEAR(B->angularVelocity.x, 0.0f, 1e-3f);
    EXPECT_NEAR(B->angularVelocity.y, 0.0f, 1e-3f);
    // About-axis spin preserved (free DOF).
    EXPECT_NEAR(B->angularVelocity.z, 0.5f, 1e-3f);

    // COM pinned (rB == 0 -> translational rows fix B's COM); B spun about z.
    EXPECT_NEAR(B->velocity.x, 0.0f, 1e-5f);
    EXPECT_NEAR(B->velocity.y, 0.0f, 1e-5f);
    EXPECT_NEAR(B->velocity.z, 0.0f, 1e-5f);
    const float spun = 2.0f * std::acos(std::clamp(B->rotation.w, -1.0f, 1.0f));
    EXPECT_GT(spun, 1e-3f) << "free hinge spin must rotate the body";
}

// ===========================================================================
// Character floor snap via the new velocity-constraint plane solver.
// snapCharacterToGround takes the heightmap-aware surface height (`surfaceY`,
// max of terrain heightmap + static object tops) from the caller so it never
// sinks a character perched on a real heightmap down to the flat physics floor.
// A GROUNDED character is settled ON `surfaceY` (so a spawn/level placement
// that "sits high above the terrain" is clamped onto the ground); an AIRBORNE
// character is only clamped at actual contact (landing), never teleported
// while jumping. This is the shared path used by both editors and test.cpp
// (through WorldManager::resolveCharacterCollision), so the ground clamp is
// identical across the Vulkan and OpenGL RHI backends.
// ===========================================================================
TEST(CharacterFloorSnap, GroundedCharacterIsSettledOntoSurface) {
    PhysicsWorld world;
    glm::vec3 feet(0.0f, 0.1f, 0.0f);  // floating above a y=0 surface
    glm::vec3 vel(0.0f, -3.0f, 0.0f);  // falling into the surface
    const bool grounded = world.snapCharacterToGround(feet, 0.4f, 1.8f, vel,
                                                      /*surfaceY=*/0.0f,
                                                      /*grounded=*/true);
    EXPECT_TRUE(grounded);
    EXPECT_NEAR(feet.y, 0.0f, 0.06f) << "grounded feet must rest on the surface";
    EXPECT_GE(vel.y, 0.0f) << "downward velocity into the ground is killed";
}

TEST(CharacterFloorSnap, SpawnAboveTerrainIsGrounded) {
    // Repro for "the character sits high above the terrain": a grounded bot
    // level-placed (spawn) 55 m above a y=0 surface must be settled onto it.
    PhysicsWorld world;
    glm::vec3 feet(0.0f, 55.0f, 0.0f); // spawned / level-placed well above
    glm::vec3 vel(0.0f, 0.0f, 0.0f);
    const bool grounded = world.snapCharacterToGround(feet, 0.4f, 1.8f, vel,
                                                      /*surfaceY=*/0.0f,
                                                      /*grounded=*/true);
    EXPECT_TRUE(grounded);
    EXPECT_NEAR(feet.y, 0.0f, 0.06f) << "spawn high above terrain must clamp to the surface, not float";
}

TEST(CharacterFloorSnap, IgnoresAirborneCharacter) {
    PhysicsWorld world;
    glm::vec3 feet(0.0f, 6.0f, 0.0f);   // 6 m up - genuinely airborne
    glm::vec3 vel(0.0f, -10.0f, 0.0f);  // actively falling
    const bool grounded = world.snapCharacterToGround(feet, 0.4f, 1.8f, vel,
                                                      /*surfaceY=*/0.0f,
                                                      /*grounded=*/false);
    EXPECT_FALSE(grounded);
    EXPECT_NEAR(feet.y, 6.0f, 1e-6f) << "airborne character must not be teleported to the ground";
    EXPECT_NEAR(vel.y, -10.0f, 1e-6f) << "falling velocity must be untouched until contact";
}

TEST(CharacterFloorSnap, StandsOnStaticPropTop) {
    PhysicsWorld world;
    glm::vec3 feet(0.0f, 2.6f, 0.0f);  // floating just above a y=2.5 crate top
    glm::vec3 vel(0.0f, -1.0f, 0.0f);
    const bool grounded = world.snapCharacterToGround(feet, 0.4f, 1.8f, vel,
                                                      /*surfaceY=*/2.5f,
                                                      /*grounded=*/true);
    EXPECT_TRUE(grounded);
    EXPECT_NEAR(feet.y, 2.5f, 0.06f) << "character must stand on the prop top, not float above it";
}

// ===========================================================================
// Part 1 (velocity motor): add5DOFMotor drives a single DOF toward
// `targetVelocity`, clamped to [-maxForce, +maxForce], solved by solve().
// ===========================================================================
TEST(HingeMotor, DrivesAngularVelocityToTarget) {
    std::vector<RigidBody> bodies;
    // A: static anchor at the origin.
    bodies.push_back(RigidBody(glm::vec3(0, 0, 0), glm::vec3(1.0f), 1.0f,
                               /*isStatic=*/true, ColliderType::SPHERE));
    // B: free body, placed away so no translational coupling.
    bodies.push_back(makeSphere(glm::vec3(1.0f, 0.0f, 0.0f), 1.0f));
    bodies[1].angularVelocity = glm::vec3(0.0f);

    vel::ConstraintSolver solver(bodies);
    vel::HingeBlockMotorParams m;
    m.bodyA = 0;
    m.bodyB = 1;
    m.localAxisA = glm::vec3(0, 0, 1);   // hinge z
    m.targetVelocity = 2.0f;             // rad/s
    m.maxForce = 500.0f;
    m.isAngular = true;
    solver.add5DOFMotor(m);

    solver.solve(0.016f, 8, 1);

    // About-axis spin reaches target velocity; perpendicular spin untouched.
    EXPECT_NEAR(bodies[1].angularVelocity.z, 2.0f, 0.05f);
    EXPECT_NEAR(bodies[1].angularVelocity.x, 0.0f, 1e-4f);
    EXPECT_NEAR(bodies[1].angularVelocity.y, 0.0f, 1e-4f);
    // A velocity motor injects no linear velocity.
    EXPECT_NEAR(bodies[1].velocity.x, 0.0f, 1e-4f);
    EXPECT_NEAR(bodies[1].velocity.y, 0.0f, 1e-4f);
    EXPECT_NEAR(bodies[1].velocity.z, 0.0f, 1e-4f);
}

// ===========================================================================
// Part 2 (static pipeline): Static5DOFHingeBlock — a compile-time, fully
// inlined positional projection that locks a single translational DOF
// (motion along `lockAxis` is removed; the two perpendicular axes stay free).
// ===========================================================================
TEST(Static5DOFHingeBlock, LocksMotionAlongAxis) {
    vel_static::StaticPhysicsWorld sw;
    vel_static::StaticRigidBody aBody, bBody;
    bBody.position = glm::vec3(2.0f, 0.0f, 0.0f);
    sw.bodies.push_back(aBody);
    sw.bodies.push_back(bBody);

    vel_static::Static5DOFHingeBlock hinge(0, 1, glm::vec3(1, 0, 0), /*stiff=*/0.8f);
    vel_static::CompileTimeConstraintPipeline<vel_static::Static5DOFHingeBlock> pipe(hinge);
    pipe.Solve(sw, 0.016f, 8);

    // Separation along the locked x-axis is driven to ~0 over 8 iterations.
    const glm::vec3 sep = sw.bodies[1].position - sw.bodies[0].position;
    EXPECT_NEAR(std::fabs(sep.x), 0.0f, 1e-3f);
    // Perpendicular (y/z) separation is unconstrained — stays at 0.
    EXPECT_NEAR(sep.y, 0.0f, 1e-6f);
    EXPECT_NEAR(sep.z, 0.0f, 1e-6f);
}
