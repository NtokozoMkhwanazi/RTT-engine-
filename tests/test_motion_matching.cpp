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
    const auto& poses = matcher->GetDatabase().GetPoses();

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

    // Update with movement
    CharacterState state;
    state.position = glm::vec3(0, 0, 0);
    state.velocity = glm::vec3(0, 0, 2.0f);  // Moving at 2 m/s
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
        state.velocity = glm::vec3(0, 0, speed);
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
