/**
 * Motion Matching & Animator Integration Tests
 * 
 * Tests for debugging motion matching system and animator integration.
 * Run with: make test
 */

#include <gtest/gtest.h>
#include "../motionMatching/MotionMatcher.h"
#include "../motionMatching/MotionDatabase.h"
#include "../motionMatching/MotionKDTree.h"
#include "../animationSystem/Animator.h"
#include "../animationSystem/Animation.h"
#include "../boneSystem/Skeleton.h"
#include <glm/glm.hpp>
#include <iostream>

// ============================================================================
// TEST FIXTURE
// ============================================================================

class MotionMatchingIntegrationTest : public ::testing::Test {
protected:
    Skeleton* skeleton{nullptr};
    Animator* animator{nullptr};
    MotionMatcher* matcher{nullptr};

    std::shared_ptr<Animation> idleAnim;
    std::shared_ptr<Animation> walkAnim;
    std::shared_ptr<Animation> runAnim;
    
    void SetUp() override {
        // Create minimal skeleton for testing
        skeleton = new Skeleton();

        // Create animator
        animator = new Animator(skeleton);

        // Create motion matcher
        matcher = new MotionMatcher();
        matcher->Initialize(skeleton, animator);

        // Reset animation pointers
        idleAnim = nullptr;
        walkAnim = nullptr;
        runAnim = nullptr;

        std::cout << "\n[MotionMatchingIntegrationTest] SetUp complete\n";
    }
    
    void TearDown() override {
        // Delete in correct order: matcher first (it has shared_ptr to animations)
        delete matcher;
        matcher = nullptr;
        
        // Then animator (no dependencies)
        delete animator;
        animator = nullptr;
        
        // Then skeleton
        delete skeleton;
        skeleton = nullptr;
        
        // Animations are automatically cleaned up by shared_ptr
        idleAnim.reset();
        walkAnim.reset();
        runAnim.reset();

        std::cout << "[MotionMatchingIntegrationTest] TearDown complete\n";
    }
    
    // Create a simple test animation with bone data
    std::shared_ptr<Animation> CreateTestAnimation(const std::string& name, float duration) {
        auto anim = std::make_shared<Animation>(name, duration, 30.0f);

        // Add a root bone animation so MotionDatabase can extract features
        BoneAnimation boneAnim;
        boneAnim.boneName = "root";

        // Add position keys
        boneAnim.positionTimes.push_back(0.0);
        boneAnim.positionValues.push_back(glm::vec3(0.0f));
        boneAnim.positionTimes.push_back(duration);
        boneAnim.positionValues.push_back(glm::vec3(0.0f, 0.0f, 1.0f));

        // Add rotation keys
        boneAnim.rotationTimes.push_back(0.0);
        boneAnim.rotationValues.push_back(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        boneAnim.rotationTimes.push_back(duration);
        boneAnim.rotationValues.push_back(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

        // Add scale keys
        boneAnim.scaleTimes.push_back(0.0);
        boneAnim.scaleValues.push_back(glm::vec3(1.0f));
        boneAnim.scaleTimes.push_back(duration);
        boneAnim.scaleValues.push_back(glm::vec3(1.0f));

        anim->boneAnimations["root"] = boneAnim;
        return anim;
    }
};

// ============================================================================
// ISOLATED TESTS - ANIMATOR
// ============================================================================

/**
 * Test: Animator Play Sets Active Animation
 */
TEST_F(MotionMatchingIntegrationTest, Animator_Play_SetsActiveAnimation) {
    auto testAnim = CreateTestAnimation("Test", 1.0f);

    // Play animation
    animator->Play(testAnim.get());

    // Verify animation is active
    EXPECT_EQ(animator->GetCurrentAnimation(), testAnim.get());
    EXPECT_FLOAT_EQ(animator->GetCurrentTime(), 0.0f);
}

/**
 * Test: Animator SetCurrentTime Updates Time
 */
TEST_F(MotionMatchingIntegrationTest, Animator_SetCurrentTime_UpdatesTime) {
    auto testAnim = CreateTestAnimation("Test", 2.0f);
    animator->Play(testAnim.get());

    // Set time to 0.5s
    animator->SetCurrentTime(0.5f);

    // Verify time was set
    EXPECT_NEAR(animator->GetCurrentTime(), 0.5f, 0.01f);
}

/**
 * Test: Animator SetCurrentTime Updates ActiveAnimations
 *
 * CRITICAL: This is what was broken! SetCurrentTime must update
 * both animatorTime AND activeAnimations[0].time
 */
TEST_F(MotionMatchingIntegrationTest, Animator_SetCurrentTime_UpdatesActiveAnimations) {
    auto testAnim = CreateTestAnimation("Test", 2.0f);
    animator->Play(testAnim.get());

    // Set time to 0.75s
    animator->SetCurrentTime(0.75f);

    // Verify BOTH times are updated
    EXPECT_NEAR(animator->GetCurrentTime(), 0.75f, 0.01f);

    // CRITICAL CHECK: Use public getter to verify active animations time
    EXPECT_GT(animator->GetActiveAnimationLayerCount(), 0);
    EXPECT_NEAR(animator->GetActiveAnimationTime(0), 0.75f, 0.01f);
}

/**
 * Test: Animator Update Advances Time
 */
TEST_F(MotionMatchingIntegrationTest, Animator_Update_AdvancesTime) {
    auto testAnim = CreateTestAnimation("Test", 2.0f);
    animator->Play(testAnim.get());
    animator->SetCurrentTime(0.5f);

    // Update with dt=0.1s
    animator->Update(0.1f);

    // Time should have advanced
    float newTime = animator->GetCurrentTime();
    EXPECT_GT(newTime, 0.5f);
    EXPECT_LE(newTime, 0.65f);  // Should be around 0.6f
}

// ============================================================================
// ISOLATED TESTS - MOTION DATABASE
// ============================================================================

/**
 * Test: MotionDatabase Load Animation
 */
TEST_F(MotionMatchingIntegrationTest, MotionDatabase_LoadAnimation) {
    auto testAnim = CreateTestAnimation("TestWalk", 1.0f);

    matcher->LoadAnimation("Walk", testAnim);

    // Verify animation was loaded
    std::string stats = matcher->GetDatabaseStats();
    EXPECT_NE(stats.find("Walk"), std::string::npos);
}

/**
 * Test: MotionDatabase GetPoses For KD-Tree
 */
TEST_F(MotionMatchingIntegrationTest, MotionDatabase_GetPoses_ForKDTree) {
    auto testAnim = CreateTestAnimation("Test", 1.0f);
    matcher->LoadAnimation("Test", testAnim);

    // Get poses for KD-Tree building
    const auto& poses = matcher->GetDatabase()->GetPoses();

    // Should have some poses (depends on animation length)
    EXPECT_GT(poses.size(), 0);
}

// ============================================================================
// ISOLATED TESTS - KD-TREE
// ============================================================================

/**
 * Test: KD-Tree Build From Poses
 * KD-Tree now copies poses internally for safe lifetime management
 */
TEST_F(MotionMatchingIntegrationTest, KDTree_BuildFromPoses) {
    // Create mock poses
    std::vector<PoseSample> poses(100);
    for (int i = 0; i < 100; i++) {
        poses[i].features.speed = static_cast<float>(i) / 100.0f * 6.0f;
        poses[i].features.rootVelocity = glm::vec3(0, 0, poses[i].features.speed);
        poses[i].features.moveAngle = 0.0f;
        poses[i].features.leftFootPlanted = (i % 2 == 0);
        poses[i].features.rightFootPlanted = (i % 2 == 1);
    }

    MotionKDTree tree;

    // Build tree (KD-Tree copies poses internally)
    tree.Build(poses, 10);

    // Verify tree was built
    EXPECT_TRUE(tree.IsBuilt());
    EXPECT_EQ(tree.GetPoseCount(), 100);

    // Get stats
    MotionKDTree::TreeStats stats = tree.GetStats();
    EXPECT_GT(stats.totalNodes, 0);
    EXPECT_GT(stats.leafNodes, 0);

    std::cout << "KD-Tree stats: " << stats.totalNodes << " nodes, "
              << stats.leafNodes << " leaves, depth=" << stats.maxDepth << "\n";
    // poses can go out of scope safely - tree has its own copy
}

/**
 * Test: KD-Tree Find Nearest
 * KD-Tree now copies poses internally for safe lifetime management
 */
TEST_F(MotionMatchingIntegrationTest, KDTree_FindNearest) {
    // Create mock poses with known speeds
    std::vector<PoseSample> poses(50);
    for (int i = 0; i < 50; i++) {
        poses[i].features.speed = static_cast<float>(i);  // 0, 1, 2, ... 49
        poses[i].features.rootVelocity = glm::vec3(0, 0, poses[i].features.speed);
        poses[i].features.moveAngle = 0.0f;
        poses[i].features.leftFootPlanted = false;
        poses[i].features.rightFootPlanted = false;
    }

    MotionKDTree tree;
    tree.Build(poses, 5);

    // Search for speed = 5.0
    MotionFeatures query;
    query.speed = 5.0f;
    query.rootVelocity = glm::vec3(0, 0, 5.0f);
    query.moveAngle = 0.0f;

    int nearest = tree.FindNearest(query);

    // Should find pose with speed closest to 5.0
    EXPECT_GE(nearest, 0);
    EXPECT_LT(nearest, 50);

    // Verify it's the right pose
    const PoseSample& found = poses[nearest];
    EXPECT_NEAR(found.features.speed, 5.0f, 1.0f);
}

// ============================================================================
// INTEGRATION TESTS - MOTION MATCHER
// ============================================================================

/**
 * Test: MotionMatcher Initialize
 */
TEST_F(MotionMatchingIntegrationTest, MotionMatcher_Initialize) {
    // matcher is already initialized in SetUp
    EXPECT_TRUE(matcher->IsActive());
}

/**
 * Test: MotionMatcher Load And Build Index
 */
TEST_F(MotionMatchingIntegrationTest, MotionMatcher_LoadAndBuildIndex) {
    // Load test animations
    idleAnim = CreateTestAnimation("Idle", 2.0f);
    walkAnim = CreateTestAnimation("Walk", 1.0f);
    runAnim = CreateTestAnimation("Run", 0.8f);

    matcher->LoadAnimation("Idle", idleAnim);
    matcher->LoadAnimation("Walk", walkAnim);
    matcher->LoadAnimation("Run", runAnim);

    // Build search index
    matcher->BuildSearchIndex();

    // Verify index was built
    std::string stats = matcher->GetDatabaseStats();
    EXPECT_NE(stats.find("Idle"), std::string::npos);
    EXPECT_NE(stats.find("Walk"), std::string::npos);
    EXPECT_NE(stats.find("Run"), std::string::npos);
}

/**
 * Test: MotionMatcher Update Changes Animation
 */
TEST_F(MotionMatchingIntegrationTest, MotionMatcher_Update_ChangesAnimation) {
    // Load animations
    idleAnim = CreateTestAnimation("Idle", 2.0f);
    walkAnim = CreateTestAnimation("Walk", 1.0f);

    matcher->LoadAnimation("Idle", idleAnim);
    matcher->LoadAnimation("Walk", walkAnim);
    matcher->BuildSearchIndex();

    // Update with movement. The matcher expects WORLD-space velocity along the
    // character's local forward (-Z at rotation 0); it internally converts to
    // the clips' +Z root frame, so -Z world velocity matches the +Z test clip.
    CharacterState state;
    state.position = glm::vec3(0, 0, 0);
    state.velocity = glm::vec3(0, 0, -2.0f);  // forward, 2 m/s
    state.rotation = 0.0f;
    state.moveDirection = glm::vec2(0, 1);
    state.grounded = true;
    state.crouching = false;

    matcher->Update(0.016f, state);

    // Verify animation changed or time advanced
    Animation* currentAnim = animator->GetCurrentAnimation();
    float currentTime = animator->GetCurrentTime();

    // Should have an animation playing
    EXPECT_NE(currentAnim, nullptr);

    // Time should be set (not zero unless just started)
    std::cout << "Current anim: " << (currentAnim ? currentAnim->name : "null")
              << ", time: " << currentTime << "\n";
}

/**
 * Test: MotionMatcher CharacterState Input
 */
TEST_F(MotionMatchingIntegrationTest, MotionMatcher_CharacterState_Input) {
    idleAnim = CreateTestAnimation("Idle", 2.0f);
    walkAnim = CreateTestAnimation("Walk", 1.0f);
    runAnim = CreateTestAnimation("Run", 0.8f);

    matcher->LoadAnimation("Idle", idleAnim);
    matcher->LoadAnimation("Walk", walkAnim);
    matcher->LoadAnimation("Run", runAnim);
    matcher->BuildSearchIndex();

    // Test different movement speeds
    std::vector<float> speeds = {0.0f, 1.5f, 3.0f, 6.0f};

    for (float speed : speeds) {
        CharacterState state;
        state.velocity = glm::vec3(0, 0, -speed);  // world-space forward = -Z
        state.grounded = true;

        matcher->Update(0.016f, state);

        // Verify animator has valid state
        EXPECT_NE(animator->GetCurrentAnimation(), nullptr);

        std::cout << "Speed=" << speed
                  << " Anim=" << (animator->GetCurrentAnimation() ?
                                  animator->GetCurrentAnimation()->name : "null")
                  << " Time=" << animator->GetCurrentTime() << "\n";
    }
}

// ============================================================================
// UNREAL-STYLE TRAJECTORY + PERSISTENCE TESTS
// ============================================================================

/**
 * Test: MotionDatabase extracts the root-relative future trajectory per pose.
 *
 * The root moves +Z; each pose's Trajectory.localPositions must point +Z in
 * root-local space with offsets growing over the 0.1-0.4s horizon.
 */
TEST_F(MotionMatchingIntegrationTest, MotionDatabase_Trajectory_ExtractedRootRelative) {
    auto anim = CreateTestAnimation("Walk", 2.0f);
    // Move the root a visible distance along +Z so the future path is non-trivial.
    anim->boneAnimations["root"].positionValues[1] = glm::vec3(0.0f, 0.0f, 4.0f);

    matcher->LoadAnimation("Walk", anim);
    const auto& poses = matcher->GetDatabase()->GetPoses();
    ASSERT_GT(poses.size(), 0u);

    const PoseSample& p0 = poses[0];
    EXPECT_EQ(p0.trajectory.localNumPoints, kTrajectorySteps);
    EXPECT_NEAR(p0.rootRotationY, 0.0f, 1e-4f);  // no rotation in the test clip

    // The future path points +Z (the clip's root motion) with growing offsets.
    float prevMag = 0.0f;
    for (int k = 0; k < kTrajectorySteps; ++k) {
        const glm::vec3 lp = p0.trajectory.localPositions[k];
        EXPECT_GT(lp.z, 0.0f) << "trajectory point " << k << " must lead forward";
        const float mag = glm::length(lp);
        EXPECT_GT(mag, prevMag) << "trajectory offsets must grow with the horizon";
        prevMag = mag;
    }
}

/**
 * Test: pose trajectories are stored in raw clip space (unrotated).
 *
 * Even when the clip's root bone rotates 90 degrees about Y, the future path
 * offsets must stay in clip space (+Z here), because the matcher expresses its
 * query path relative to the character heading - the render applies R(heading)
 * on top of clip space, so the query's R(-heading) reproduces clip space by
 * construction. Rotating the DB side by the root yaw would only match when the
 * yaw is exactly 0 and would degrade turn matching otherwise.
 */
TEST_F(MotionMatchingIntegrationTest, MotionDatabase_Trajectory_IgnoresRootYaw) {
    auto anim = CreateTestAnimation("Walk", 2.0f);
    anim->boneAnimations["root"].positionValues[1] = glm::vec3(0.0f, 0.0f, 4.0f);
    // Constant root yaw of 90 degrees about Y (angle/2 = 45 deg).
    const float half = 0.70710678f;
    const glm::quat yaw90(half, 0.0f, half, 0.0f);
    anim->boneAnimations["root"].rotationValues[0] = yaw90;
    anim->boneAnimations["root"].rotationValues[1] = yaw90;

    matcher->LoadAnimation("Walk", anim);
    const auto& poses = matcher->GetDatabase()->GetPoses();
    ASSERT_GT(poses.size(), 0u);

    const PoseSample& p0 = poses[0];
    EXPECT_EQ(p0.trajectory.localNumPoints, kTrajectorySteps);
    // Offsets stay in clip space: +Z forward despite the 90-deg root yaw.
    for (int k = 0; k < kTrajectorySteps; ++k) {
        EXPECT_GT(p0.trajectory.localPositions[k].z, 0.0f)
            << "trajectory must stay in clip space, not root-yaw space";
    }
}

/**
 * Test: the trajectory predictor preserves speed instead of collapsing.
 *
 * Regression: Predict() used to mix the velocity toward a hardcoded
 * "worldInputDir * 6.0f units/s" each step. In the source-unit feature space
 * the matcher searches (walk ~168, run ~500+ units/s), that decayed the
 * predicted path toward a standstill, so every fast pose was mis-ranked by its
 * trajectory feature. The predicted velocity must keep the input magnitude.
 */
TEST_F(MotionMatchingIntegrationTest, TrajectoryPredictor_PreservesSpeed) {
    TrajectoryPredictor predictor;
    MotionMatchingConfig cfg;
    cfg.trajectoryDuration = 0.4f;
    cfg.trajectoryPoints = 5;

    // High speed in source units, moving forward (+Z world at rotation 0).
    const glm::vec3 pos(0.0f);
    const glm::vec3 vel(0.0f, 0.0f, 500.0f);
    Trajectory t = predictor.Predict(pos, vel, 0.0f, glm::vec2(0.0f, 1.0f), cfg);
    ASSERT_GT(t.numPoints, 1);

    const float firstSpeed = glm::length(t.velocities[0]);
    const float lastSpeed = glm::length(t.velocities[t.numPoints - 1]);
    EXPECT_GT(firstSpeed, 450.0f) << "predicted speed must start near the input";
    EXPECT_GT(lastSpeed, 450.0f)
        << "predicted speed must not collapse toward 6 units/s over the horizon";

    // The predicted path must also advance at ~that speed (integration check).
    const float advance = glm::length(t.positions[1] - t.positions[0]);
    EXPECT_GT(advance, 30.0f) << "path must advance at locomotion speed per step";
}

/**
 * Test: KD-tree trajectory features discriminate future direction.
 *
 * Four poses at IDENTICAL speed/velocity - only the root-local future path
 * differs (+Z vs -Z). A +Z query must land on a +Z pose: the trajectory
 * feature is what breaks the tie (Unreal's core idea).
 */
TEST_F(MotionMatchingIntegrationTest, KDTree_Trajectory_DiscriminatesDirection) {
    std::vector<PoseSample> poses(4);
    auto setPath = [&](int idx, float sign) {
        poses[idx].features.speed = 2.0f;
        poses[idx].features.rootVelocity = glm::vec3(0.0f, 0.0f, 2.0f);
        poses[idx].features.moveAngle = 0.0f;
        poses[idx].trajectory.localNumPoints = kTrajectorySteps;
        for (int k = 0; k < kTrajectorySteps; ++k) {
            poses[idx].trajectory.localPositions[k] =
                glm::vec3(0.0f, 0.0f, sign * 0.2f * (k + 1));
        }
    };
    setPath(0, 1.0f);
    setPath(1, 1.0f);
    setPath(2, -1.0f);
    setPath(3, -1.0f);

    MotionKDTree tree;
    tree.Build(poses, 1);

    MotionFeatures query;
    query.speed = 2.0f;
    query.rootVelocity = glm::vec3(0.0f, 0.0f, 2.0f);
    query.moveAngle = 0.0f;
    query.futureCount = kTrajectorySteps;
    for (int k = 0; k < kTrajectorySteps; ++k) {
        query.futureLocal[k] = glm::vec2(0.0f, 0.2f * (k + 1));  // +Z future
    }

    const int nearest = tree.FindNearest(query);
    ASSERT_GE(nearest, 0);
    ASSERT_LT(nearest, 4);
    EXPECT_GT(poses[nearest].trajectory.localPositions[0].z, 0.0f)
        << "query heading +Z must match a +Z pose, not a -Z pose";
}

/**
 * Test: Animator::BlendToAt crossfades to a clip at a specific time.
 *
 * After the call there must be TWO active layers (old fading out, new fading
 * in at the requested time); once the crossfade elapses the new clip is the
 * only one left and the old layer has been removed.
 */
TEST_F(MotionMatchingIntegrationTest, Animator_BlendToAt_CrossfadesAtTime) {
    auto idle = CreateTestAnimation("Idle", 2.0f);
    auto walk = CreateTestAnimation("Walk", 2.0f);  // 2s so the time check can't wrap

    animator->Play(idle.get());
    animator->Update(0.1f);  // let idle advance a little

    // Crossfade to Walk starting at 0.4s over 0.2s.
    animator->BlendToAt(walk.get(), 0.4f, 0.2f);

    // Two layers now: the old clip fading out + Walk fading in.
    EXPECT_GE(animator->GetActiveAnimationLayerCount(), 2);
    // One of the active layers must be the incoming Walk clip at 0.4s.
    bool foundWalkAtTime = false;
    for (int i = 0; i < animator->GetActiveAnimationLayerCount(); ++i) {
        if (std::abs(animator->GetActiveAnimationTime(i) - 0.4f) < 0.01f) {
            foundWalkAtTime = true;
        }
    }
    EXPECT_TRUE(foundWalkAtTime);

    // Mid-crossfade: both layers still present (weights are blending).
    animator->Update(0.05f);
    EXPECT_GE(animator->GetActiveAnimationLayerCount(), 2);

    // Let the crossfade finish and the zero-weight layer get cleaned up.
    for (int i = 0; i < 30; ++i) animator->Update(0.02f);
    EXPECT_EQ(animator->GetActiveAnimationLayerCount(), 1);
    EXPECT_EQ(animator->GetCurrentAnimation(), walk.get());
    // 0.4s start + 0.05s + 0.6s of updates = 1.05s into the 2s clip.
    EXPECT_NEAR(animator->GetCurrentTime(), 1.05f, 0.1f);
}

/**
 * Test: MotionMatcher clip persistence - no flicker at a steady speed.
 *
 * At a steady walk the matcher must stay in the Walk clip for the whole run.
 * This validates the Unreal-style hysteresis: the pose search may find Idle
 * poses nearby, but the current clip is only left when another clip is
 * clearly better.
 */
TEST_F(MotionMatchingIntegrationTest, MotionMatcher_Persistence_NoClipFlicker) {
    idleAnim = CreateTestAnimation("Idle", 2.0f);
    walkAnim = CreateTestAnimation("Walk", 1.0f);
    // Idle root stays put; Walk root moves 2 units over 1s (~2 units/s).
    idleAnim->boneAnimations["root"].positionValues[1] = glm::vec3(0.0f, 0.0f, 0.0f);
    walkAnim->boneAnimations["root"].positionValues[1] = glm::vec3(0.0f, 0.0f, 2.0f);

    matcher->LoadAnimation("Idle", idleAnim);
    matcher->LoadAnimation("Walk", walkAnim);
    matcher->BuildSearchIndex();

    CharacterState state;
    state.position = glm::vec3(0.0f, 0.0f, 0.0f);
    state.velocity = glm::vec3(0.0f, 0.0f, -1.5f);  // steady walk (forward = -Z)
    state.rotation = 0.0f;
    state.moveDirection = glm::vec2(0.0f, 1.0f);
    state.grounded = true;
    state.crouching = false;

    // First frame selects the moving clip.
    matcher->Update(0.016f, state);
    EXPECT_EQ(matcher->GetDebugInfo().currentAnimationName, "Walk");

    // The clip must never flicker back to Idle over many frames.
    for (int i = 0; i < 120; ++i) {
        matcher->Update(0.016f, state);
        EXPECT_EQ(matcher->GetDebugInfo().currentAnimationName, "Walk")
            << "clip flickered on frame " << i;
    }
}

/**
 * Test: MotionMatcher switches clips when speed clearly changes.
 *
 * Walk fast then stop: the matcher must leave Walk and settle in Idle once
 * the query speed drops (the hysteresis margin must not trap it in Walk).
 */
TEST_F(MotionMatchingIntegrationTest, MotionMatcher_SwitchesClip_OnStop) {
    idleAnim = CreateTestAnimation("Idle", 2.0f);
    walkAnim = CreateTestAnimation("Walk", 1.0f);
    idleAnim->boneAnimations["root"].positionValues[1] = glm::vec3(0.0f, 0.0f, 0.0f);
    walkAnim->boneAnimations["root"].positionValues[1] = glm::vec3(0.0f, 0.0f, 2.0f);

    matcher->LoadAnimation("Idle", idleAnim);
    matcher->LoadAnimation("Walk", walkAnim);
    matcher->BuildSearchIndex();

    CharacterState walking;
    walking.velocity = glm::vec3(0.0f, 0.0f, -1.5f);  // world-space forward = -Z
    walking.moveDirection = glm::vec2(0.0f, 1.0f);
    walking.grounded = true;
    for (int i = 0; i < 10; ++i) matcher->Update(0.016f, walking);
    EXPECT_EQ(matcher->GetDebugInfo().currentAnimationName, "Walk");

    CharacterState stopped;
    stopped.velocity = glm::vec3(0.0f);
    stopped.moveDirection = glm::vec2(0.0f, 0.0f);
    stopped.grounded = true;
    for (int i = 0; i < 30; ++i) matcher->Update(0.016f, stopped);
    EXPECT_EQ(matcher->GetDebugInfo().currentAnimationName, "Idle");
}

/**
 * Test: full locomotion cycle idle -> walk -> run -> stop.
 *
 * Drives the matcher through the whole loop the player sees: stand, walk,
 * sprint, stop. Each phase must land on the right clip and STAY there, and
 * each transition must complete (the hysteresis must not trap the matcher in
 * the previous clip, nor flicker during a steady phase).
 */
TEST_F(MotionMatchingIntegrationTest, MotionMatcher_LocomotionCycle_IdleWalkRunStop) {
    idleAnim = CreateTestAnimation("Idle", 2.0f);
    walkAnim = CreateTestAnimation("Walk", 1.0f);
    runAnim = CreateTestAnimation("Run", 1.0f);
    idleAnim->boneAnimations["root"].positionValues[1] = glm::vec3(0.0f, 0.0f, 0.0f);
    walkAnim->boneAnimations["root"].positionValues[1] = glm::vec3(0.0f, 0.0f, 2.0f);
    runAnim->boneAnimations["root"].positionValues[1] = glm::vec3(0.0f, 0.0f, 4.0f);

    matcher->LoadAnimation("Idle", idleAnim);
    matcher->LoadAnimation("Walk", walkAnim);
    matcher->LoadAnimation("Run", runAnim);
    matcher->BuildSearchIndex();

    auto step = [&](float speed, int frames, const char* expect, int settle) {
        CharacterState s;
        s.velocity = glm::vec3(0.0f, 0.0f, -speed);  // world-space forward = -Z
        s.moveDirection = glm::vec2(0.0f, 1.0f);
        s.grounded = true;
        // Phase-in: let the matcher transition and settle into the clip.
        for (int i = 0; i < settle; ++i) matcher->Update(0.016f, s);
        // Steady phase: must hold the expected clip without flicker.
        for (int i = 0; i < frames; ++i) {
            matcher->Update(0.016f, s);
            EXPECT_EQ(matcher->GetDebugInfo().currentAnimationName, expect)
                << "phase '" << expect << "' flickered on frame " << i;
        }
    };

    step(0.0f, 30, "Idle", 5);   // stand -> idle
    step(2.0f, 60, "Walk", 10);   // walk -> walk
    step(4.0f, 60, "Run", 10);    // sprint -> run
    step(0.0f, 40, "Idle", 15);   // stop -> idle
}

// ============================================================================
// AIRBORNE MATCHING TESTS (UE-style movement-state gate + vertical velocity)
// ============================================================================

/**
 * Test: the database tags Jump/Fall clips as airborne and locomotion clips
 * as grounded (the pose-level movement-state gate the matcher filters on).
 */
TEST_F(MotionMatchingIntegrationTest, MotionDatabase_Airborne_TagsClips) {
    auto idle = CreateTestAnimation("Idle", 2.0f);
    auto jump = CreateTestAnimation("Jump", 1.0f);
    jump->boneAnimations["root"].positionValues[1] = glm::vec3(0.0f, 4.0f, 0.0f);  // rises
    auto fall = CreateTestAnimation("Fall", 1.0f);
    fall->boneAnimations["root"].positionValues[1] = glm::vec3(0.0f, -4.0f, 0.0f); // falls

    matcher->LoadAnimation("Idle", idle);
    matcher->LoadAnimation("Jump", jump);
    matcher->LoadAnimation("Fall", fall);

    const auto& poses = matcher->GetDatabase()->GetPoses();
    ASSERT_GT(poses.size(), 0u);

    bool sawAirborne = false;
    bool sawGrounded = false;
    for (const auto& p : poses) {
        if (p.features.isAirborne) sawAirborne = true; else sawGrounded = true;
    }
    EXPECT_TRUE(sawAirborne) << "Jump/Fall poses must be flagged airborne";
    EXPECT_TRUE(sawGrounded) << "Idle poses must stay grounded";
    EXPECT_TRUE(matcher->GetDatabase()->HasAirbornePoses());
}

/**
 * Test: the KD-tree vertical-velocity feature discriminates ascent from
 * descent. Two poses with identical XZ motion - only rootVelocity.y differs.
 * A rising query must land on the rising pose.
 */
TEST_F(MotionMatchingIntegrationTest, KDTree_VerticalVelocity_DiscriminatesAscent) {
    std::vector<PoseSample> poses(4);
    for (int i = 0; i < 4; ++i) {
        poses[i].features.speed = 0.0f;
        poses[i].features.rootVelocity = glm::vec3(0.0f, (i < 2) ? 4.0f : -4.0f, 0.0f);
        poses[i].features.moveAngle = 0.0f;
    }

    MotionKDTree tree;
    tree.Build(poses, 1);

    MotionFeatures query;
    query.speed = 0.0f;
    query.rootVelocity = glm::vec3(0.0f, 3.0f, 0.0f);  // rising
    query.moveAngle = 0.0f;

    const int nearest = tree.FindNearest(query);
    ASSERT_GE(nearest, 0);
    ASSERT_LT(nearest, 4);
    EXPECT_GT(poses[nearest].features.rootVelocity.y, 0.0f)
        << "rising query must match a rising pose, not a falling one";
}

/**
 * Test: an airborne query is gated to airborne poses - it can never land on
 * the grounded Idle clip even though its XZ speed (0) ties with Idle's.
 */
TEST_F(MotionMatchingIntegrationTest, MotionMatcher_Airborne_GatedToAirbornePoses) {
    auto idle = CreateTestAnimation("Idle", 2.0f);
    auto jump = CreateTestAnimation("Jump", 1.0f);
    jump->boneAnimations["root"].positionValues[1] = glm::vec3(0.0f, 4.0f, 0.0f);

    matcher->LoadAnimation("Idle", idle);
    matcher->LoadAnimation("Jump", jump);
    matcher->BuildSearchIndex();

    // Airborne at rest (XZ speed 0 ties with Idle, but Idle must be excluded).
    CharacterState airborne;
    airborne.velocity = glm::vec3(0.0f, 3.0f, 0.0f);
    airborne.moveDirection = glm::vec2(0.0f, 0.0f);
    airborne.grounded = false;
    airborne.jumping = true;

    matcher->Update(0.016f, airborne);
    EXPECT_EQ(matcher->GetDebugInfo().currentAnimationName, "Jump")
        << "airborne query must not match the grounded Idle clip";
}

/**
 * Test: ascent matches the Jump clip, descent matches the Fall clip - the
 * vertical-velocity feature drives the phase inside the airborne set.
 */
TEST_F(MotionMatchingIntegrationTest, MotionMatcher_Airborne_AscentVsDescent) {
    auto idle = CreateTestAnimation("Idle", 2.0f);
    auto jump = CreateTestAnimation("Jump", 1.0f);
    jump->boneAnimations["root"].positionValues[1] = glm::vec3(0.0f, 4.0f, 0.0f);
    auto fall = CreateTestAnimation("Fall", 1.0f);
    fall->boneAnimations["root"].positionValues[1] = glm::vec3(0.0f, -4.0f, 0.0f);

    matcher->LoadAnimation("Idle", idle);
    matcher->LoadAnimation("Jump", jump);
    matcher->LoadAnimation("Fall", fall);
    matcher->BuildSearchIndex();

    // Rising phase -> Jump.
    CharacterState rising;
    rising.velocity = glm::vec3(0.0f, 3.0f, 0.0f);
    rising.moveDirection = glm::vec2(0.0f, 0.0f);
    rising.grounded = false;
    rising.jumping = true;
    for (int i = 0; i < 10; ++i) matcher->Update(0.016f, rising);
    EXPECT_EQ(matcher->GetDebugInfo().currentAnimationName, "Jump");

    // Falling phase -> Fall.
    CharacterState falling;
    falling.velocity = glm::vec3(0.0f, -3.0f, 0.0f);
    falling.moveDirection = glm::vec2(0.0f, 0.0f);
    falling.grounded = false;
    falling.jumping = true;
    for (int i = 0; i < 15; ++i) matcher->Update(0.016f, falling);
    EXPECT_EQ(matcher->GetDebugInfo().currentAnimationName, "Fall")
        << "descending query must switch to the Fall clip";
}

/**
 * Test: landing returns to locomotion - once grounded, the state gate lets
 * the matcher blend back into the Walk clip (crossfade, not a hard cut).
 */
TEST_F(MotionMatchingIntegrationTest, MotionMatcher_Airborne_LandingReturnsToLocomotion) {
    auto idle = CreateTestAnimation("Idle", 2.0f);
    auto walk = CreateTestAnimation("Walk", 1.0f);
    walk->boneAnimations["root"].positionValues[1] = glm::vec3(0.0f, 0.0f, 2.0f);
    auto jump = CreateTestAnimation("Jump", 1.0f);
    jump->boneAnimations["root"].positionValues[1] = glm::vec3(0.0f, 4.0f, 0.0f);

    matcher->LoadAnimation("Idle", idle);
    matcher->LoadAnimation("Walk", walk);
    matcher->LoadAnimation("Jump", jump);
    matcher->BuildSearchIndex();

    // Take off.
    CharacterState rising;
    rising.velocity = glm::vec3(0.0f, 3.0f, 0.0f);
    rising.moveDirection = glm::vec2(0.0f, 1.0f);
    rising.grounded = false;
    rising.jumping = true;
    for (int i = 0; i < 10; ++i) matcher->Update(0.016f, rising);
    EXPECT_EQ(matcher->GetDebugInfo().currentAnimationName, "Jump");

    // Land and walk: the gate flips to grounded and the matcher must blend
    // into Walk (a crossfade leaves 2 active layers on the switch frame).
    CharacterState walking;
    walking.velocity = glm::vec3(0.0f, 0.0f, -1.5f);  // world-space forward = -Z
    walking.moveDirection = glm::vec2(0.0f, 1.0f);
    walking.grounded = true;
    walking.jumping = false;
    matcher->Update(0.016f, walking);
    EXPECT_EQ(matcher->GetDebugInfo().currentAnimationName, "Walk");
    // The clip switch must go through BlendToAt (2+ layers) on the frame it
    // happens - the landing crossfades instead of snapping.
    EXPECT_GE(animator->GetActiveAnimationLayerCount(), 2);

    for (int i = 0; i < 30; ++i) matcher->Update(0.016f, walking);
    EXPECT_EQ(matcher->GetDebugInfo().currentAnimationName, "Walk");
}

// ============================================================================
// DEBUG HELPERS
// ============================================================================

/**
 * Test: Debug Print Animator State
 */
TEST_F(MotionMatchingIntegrationTest, DEBUG_PrintAnimatorState) {
    std::cout << "\n=== ANIMATOR STATE ===\n";
    std::cout << "Current anim: " << (animator->GetCurrentAnimation() ?
                                       animator->GetCurrentAnimation()->name : "null") << "\n";
    std::cout << "Current time: " << animator->GetCurrentTime() << "\n";
    std::cout << "Active animation layers: " << animator->GetActiveAnimationLayerCount() << "\n";

    if (animator->GetActiveAnimationLayerCount() > 0) {
        std::cout << "  [0] time: " << animator->GetActiveAnimationTime(0) << "\n";
    }
}

/**
 * Test: Debug Print MotionMatcher State
 */
TEST_F(MotionMatchingIntegrationTest, DEBUG_PrintMotionMatcherState) {
    std::cout << "\n=== MOTION MATCHER STATE ===\n";
    std::cout << "Active: " << (matcher->IsActive() ? "YES" : "NO") << "\n";
    std::cout << "Database: " << matcher->GetDatabaseStats() << "\n";
}
