/**
 * BoneMatrixBuffer + MM gait-phase regression tests (GL-free)
 *
 * 1. ShouldSkipUpload: the pure upload decision that used to live inline in
 *    BoneMatrixBuffer::Update(). Its short-circuit
 *    ( !forceUpdate && !needsUpdate && same count ) is the root cause of the
 *    "skeleton animates but the mesh is frozen in Idle" bug - after the first
 *    upload, needsUpdate=false and every later Update() skipped, so the GPU
 *    kept frame-1's pose forever. The callers now pass forceUpdate=true; these
 *    tests pin the decision so a future "optimization" can't reintroduce it.
 *
 * 2. KD-tree foot-plant feature: the motion-matching query carries the
 *    CURRENT pose's foot-contact state (UE-style gait phase). A query with a
 *    planted left foot must prefer poses in the same phase - before the fix
 *    the query read both-feet-unplanted, so every planted pose was penalized
 *    (footskating).
 *
 * Run with: make test
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <vector>

#include "../animationSystem/BoneMatrixBuffer.h"
#include "../animationSystem/AnimationStateMachine.h"
#include "../motionMatching/MotionMatchingTypes.h"
#include "../motionMatching/MotionKDTree.h"

// ============================================================================
// BoneMatrixBuffer::ShouldSkipUpload - the frozen-mesh regression
// ============================================================================

TEST(BoneMatrixBufferShouldSkipUpload, ForceUpdateAlwaysUploads) {
    // forceUpdate=true (what Model::Draw / Animator::UpdateBoneBuffer pass)
    // must NEVER skip - the animated bone matrices change every frame.
    EXPECT_FALSE(BoneMatrixBuffer::ShouldSkipUpload(true, false, 65, 65));
    EXPECT_FALSE(BoneMatrixBuffer::ShouldSkipUpload(true, true, 65, 65));
    EXPECT_FALSE(BoneMatrixBuffer::ShouldSkipUpload(true, false, 60, 65));
    EXPECT_FALSE(BoneMatrixBuffer::ShouldSkipUpload(true, false, 65, 60));
}

TEST(BoneMatrixBufferShouldSkipUpload, DirtyBufferNeverSkips) {
    // needsUpdate=true (first upload, or after a rebuild) must upload even
    // without the force flag.
    EXPECT_FALSE(BoneMatrixBuffer::ShouldSkipUpload(false, true, 65, 65));
    EXPECT_FALSE(BoneMatrixBuffer::ShouldSkipUpload(false, true, 60, 65));
}

TEST(BoneMatrixBufferShouldSkipUpload, CountChangeNeverSkips) {
    // A different bone count is always a real change.
    EXPECT_FALSE(BoneMatrixBuffer::ShouldSkipUpload(false, false, 60, 65));
    EXPECT_FALSE(BoneMatrixBuffer::ShouldSkipUpload(false, false, 70, 65));
}

TEST(BoneMatrixBufferShouldSkipUpload, IdenticalStaticBufferSkipsWithoutForce) {
    // The ONLY case where skipping is correct: nothing forced, not dirty, and
    // the count matches. This is the exact short-circuit that became a bug
    // because animated callers did not pass forceUpdate=true - the test below
    // documents why that is insufficient for animation.
    EXPECT_TRUE(BoneMatrixBuffer::ShouldSkipUpload(false, false, 65, 65));
}

// ============================================================================
// KD-tree foot-plant feature - the MM gait-phase regression
// ============================================================================

/**
 * Build a tiny pose set where foot-plant state is the ONLY distinguishing
 * feature (identical speed/velocity/trajectory). A query carrying a planted
 * left foot must prefer the same-phase pose over the both-unplanted one.
 */
TEST(MMGaitPhase, KDTreePrefersPoseWithMatchingFootPlant) {
    std::vector<PoseSample> poses(2);
    for (int i = 0; i < 2; ++i) {
        poses[i].features.speed = 2.0f;
        poses[i].features.rootVelocity = glm::vec3(0.0f, 0.0f, 2.0f);
        poses[i].features.moveAngle = 0.0f;
        // Trajectory identical for both (straight +Z) so it can't break ties.
        poses[i].trajectory.localNumPoints = kTrajectorySteps;
        for (int k = 0; k < kTrajectorySteps; ++k) {
            poses[i].trajectory.localPositions[k] =
                glm::vec3(0.0f, 0.0f, 0.2f * (k + 1));
        }
    }
    // Pose 0: left foot planted (stance phase). Pose 1: both feet unplanted.
    poses[0].features.leftFootPlanted = true;
    poses[0].features.rightFootPlanted = false;
    poses[1].features.leftFootPlanted = false;
    poses[1].features.rightFootPlanted = false;

    MotionKDTree tree;
    tree.Build(poses, 1);

    // Query in the same gait phase: left foot planted (the matcher now carries
    // the current pose's contact state into the query).
    MotionFeatures query;
    query.speed = 2.0f;
    query.rootVelocity = glm::vec3(0.0f, 0.0f, 2.0f);
    query.moveAngle = 0.0f;
    query.futureCount = kTrajectorySteps;
    for (int k = 0; k < kTrajectorySteps; ++k) {
        query.futureLocal[k] = glm::vec2(0.0f, 0.2f * (k + 1));
    }
    query.leftFootPlanted = true;
    query.rightFootPlanted = false;

    const int nearest = tree.FindNearest(query);
    ASSERT_GE(nearest, 0);
    ASSERT_LT(nearest, 2);
    EXPECT_TRUE(poses[nearest].features.leftFootPlanted)
        << "a planted query must prefer the same-phase (planted) pose - "
           "otherwise the search always prefers mid-swing poses (footskating)";
}

/**
 * Inverse case: an all-unplanted query must prefer the unplanted pose.
 */
TEST(MMGaitPhase, KDTreePrefersPoseWithMatchingUnplanted) {
    std::vector<PoseSample> poses(2);
    for (int i = 0; i < 2; ++i) {
        poses[i].features.speed = 2.0f;
        poses[i].features.rootVelocity = glm::vec3(0.0f, 0.0f, 2.0f);
        poses[i].features.moveAngle = 0.0f;
    }
    poses[0].features.leftFootPlanted = true;
    poses[0].features.rightFootPlanted = true;
    poses[1].features.leftFootPlanted = false;
    poses[1].features.rightFootPlanted = false;

    MotionKDTree tree;
    tree.Build(poses, 1);

    MotionFeatures query;
    query.speed = 2.0f;
    query.rootVelocity = glm::vec3(0.0f, 0.0f, 2.0f);
    query.moveAngle = 0.0f;
    // Both feet unplanted (swing phase).
    query.leftFootPlanted = false;
    query.rightFootPlanted = false;

    const int nearest = tree.FindNearest(query);
    ASSERT_GE(nearest, 0);
    ASSERT_LT(nearest, 2);
    EXPECT_FALSE(poses[nearest].features.leftFootPlanted)
        << "an unplanted query must prefer the same-phase (swing) pose";
}

// ============================================================================
// FSM blend-band alignment (constructor must match header tuning)
// ============================================================================

/**
 * The AnimationStateMachine constructor used to hard-code different blend
 * bands (0.2/0.55, rate 15) than the header declared (0.3/0.6, rate 30) - the
 * getters read the members, so the header was a lie and setBlendBands() tuning
 * behaved differently than documented. Pin the alignment.
 */
TEST(FSMBlendBands, ConstructorMatchesHeaderTuning) {
    Animator* nullAnimator = nullptr;  // FSM never dereferences it in these getters
    AnimationStateMachine fsm(nullAnimator);

    EXPECT_NEAR(fsm.getIdleToWalkThreshold(), 0.3f, 1e-4f);
    EXPECT_NEAR(fsm.getWalkToRunThreshold(), 0.6f, 1e-4f);
    EXPECT_NEAR(fsm.getMaxWalkSpeed(), 2.0f, 1e-4f);
    EXPECT_NEAR(fsm.getMaxRunSpeed(), 6.0f, 1e-4f);
}

/**
 * setBlendBands must actually change the band thresholds (the user-facing
 * tuning knob for the locomotion FSM).
 */
TEST(FSMBlendBands, SetBlendBandsTakesEffect) {
    Animator* nullAnimator = nullptr;
    AnimationStateMachine fsm(nullAnimator);

    fsm.setBlendBands(0.15f, 0.5f);
    EXPECT_NEAR(fsm.getIdleToWalkThreshold(), 0.15f, 1e-4f);
    EXPECT_NEAR(fsm.getWalkToRunThreshold(), 0.5f, 1e-4f);

    fsm.setBlendBands(0.3f, 0.6f);  // restore default
    EXPECT_NEAR(fsm.getIdleToWalkThreshold(), 0.3f, 1e-4f);
    EXPECT_NEAR(fsm.getWalkToRunThreshold(), 0.6f, 1e-4f);
}
