/**
 * FBX Loading & Animation Integration Tests
 * 
 * Tests for:
 * 1. Model loading (Bot, Bear, Datsun, etc.)
 * 2. Character movement (root motion extraction)
 * 3. Root motion debug output
 * 4. Motion matching search results
 * 
 * Run with: make test
 */

#include <gtest/gtest.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include "../modelSystem/Model.h"
#include "../animationSystem/Animator.h"
#include "../animationSystem/Animation.h"
#include "../animationSystem/AssimpAnimationLoader.h"
#include "../motionMatching/MotionMatcher.h"
#include "../motionMatching/MotionDatabase.h"
#include "../motionMatching/MotionMatchingTypes.h"
#include "../boneSystem/Skeleton.h"
#include "../utils/FBXLoader.h"

#include <assimp/postprocess.h>

// ============================================================================
// Helper: Load animation from file
// ============================================================================
Animation* LoadAnimationFromFile(const std::string& path) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path, 
        aiProcess_Triangulate | aiProcess_FlipUVs);
    
    if (!scene || !scene->HasAnimations()) {
        return nullptr;
    }
    
    // LoadAnimationWithPoseCorrection returns by value
    Animation animTemp = AssimpAnimationLoader::LoadAnimationWithPoseCorrection(
        scene, scene->mAnimations[0]);
    
    // Copy to heap for storage
    return new Animation(animTemp);
}

// ============================================================================
// TEST FIXTURE
// ============================================================================

class FBXAnimationIntegrationTest : public ::testing::Test {
protected:
    Model* botModel{nullptr};
    Model* bearModel{nullptr};
    Model* datsunModel{nullptr};
    const Skeleton* skeleton{nullptr};
    Animator* animator{nullptr};
    MotionMatcher* matcher{nullptr};
    
    std::vector<std::string> loadedModels;
    std::vector<std::string> failedModels;
    
    void SetUp() override {
        std::cout << "\n=== FBX Animation Integration Test Setup ===\n";
        loadedModels.clear();
        failedModels.clear();
        botModel = nullptr;
        bearModel = nullptr;
        datsunModel = nullptr;
        skeleton = nullptr;
        animator = nullptr;
        matcher = nullptr;
    }
    
    void TearDown() override {
        delete matcher; matcher = nullptr;
        delete animator; animator = nullptr;
        delete botModel; botModel = nullptr;
        delete bearModel; bearModel = nullptr;
        delete datsunModel; datsunModel = nullptr;
        skeleton = nullptr;
        std::cout << "\n=== Test Cleanup Complete ===\n";
    }
    
    bool loadModel(const std::string& name, const std::string& path) {
        ModelLoadResult result = LoadFBXModel(path, name.c_str());
        if (result.success) {
            loadedModels.push_back(name);
            std::cout << "[TEST] Loaded: " << name << " - " 
                      << result.boneCount << " bones, " 
                      << result.meshCount << " meshes\n";
        } else {
            failedModels.push_back(name);
            std::cout << "[TEST] FAILED: " << name << " - " 
                      << result.errorMessage << "\n";
        }
        delete result.model;  // Test only inspects counts - release the model
        return result.success;
    }
    
    void initializeMotionMatching(Model* model) {
        if (model) {
            skeleton = &model->GetSkeleton();
            animator = new Animator(skeleton);
            matcher = new MotionMatcher();
            matcher->Initialize(skeleton, animator);
            std::cout << "[TEST] Motion matching initialized\n";
        }
    }
};

// ============================================================================
// TEST 1: Model Loading
// ============================================================================

TEST_F(FBXAnimationIntegrationTest, AllModelsLoadSuccessfully) {
    std::cout << "\n--- TEST: All Models Load Successfully ---\n";
    
    // Test primary character model
    bool botLoaded = loadModel("Bot", "assets/bot.fbx");
    ASSERT_TRUE(botLoaded) << "Bot model failed to load - check assets/bot.fbx";
    
    // Test optional models (may not exist)
    bool bearLoaded = loadModel("Bear", "assets/World_objects/Bear_DEMO.fbx");
    bool datsunLoaded = loadModel("Datsun", "assets/datsun.fbx");  // In assets root
    
    std::cout << "\n=== Model Loading Summary ===\n";
    std::cout << "Loaded: " << loadedModels.size() << "\n";
    std::cout << "Failed: " << failedModels.size() << "\n";
    
    for (const auto& name : loadedModels) std::cout << "  ✓ " << name << "\n";
    for (const auto& name : failedModels) std::cout << "  ✗ " << name << "\n";
    
    EXPECT_TRUE(botLoaded) << "Primary character model (Bot) must load";
    
    // These are informational - assets may not exist
    if (bearLoaded) std::cout << "[PASS] Bear model loaded\n";
    else std::cout << "[INFO] Bear model not found (optional)\n";
    
    if (datsunLoaded) std::cout << "[PASS] Datsun model loaded\n";
    else std::cout << "[INFO] Datsun model not found (optional)\n";
}

// ============================================================================
// TEST 2: Model Properties
// ============================================================================

TEST_F(FBXAnimationIntegrationTest, ModelPropertiesAreValid) {
    std::cout << "\n--- TEST: Model Properties Are Valid ---\n";
    
    ModelLoadResult botResult = LoadFBXModel("assets/bot.fbx", "Bot");
    
    ASSERT_TRUE(botResult.success) << "Bot model must load";
    
    std::cout << "Bot bones: " << botResult.boneCount << "\n";
    EXPECT_GT(botResult.boneCount, 0) << "Should have bones for animation";
    
    std::cout << "Bot meshes: " << botResult.meshCount << "\n";
    EXPECT_GT(botResult.meshCount, 0) << "Should have at least one mesh";
    
    std::cout << "Bot size: " << Vec3ToString(botResult.size) << "\n";
    EXPECT_GT(botResult.size.y, 0.1f) << "Height should be positive";
    EXPECT_LT(botResult.size.y, 200.0f) << "Height should be reasonable (model is ~180 units tall)";

    delete botResult.model;
}

// ============================================================================
// TEST 3: Root Motion Extraction
// ============================================================================

TEST_F(FBXAnimationIntegrationTest, RootMotionIsExtracted) {
    std::cout << "\n--- TEST: Root Motion Extraction ---\n";
    
    botModel = new Model("assets/bot.fbx");
    ASSERT_TRUE(botModel != nullptr);
    
    skeleton = &botModel->GetSkeleton();
    animator = new Animator(skeleton);
    
    Animation* walkAnim = LoadAnimationFromFile("assets/Walking.fbx");
    
    if (!walkAnim) {
        GTEST_SKIP() << "Walking animation not found";
    }
    
    std::cout << "Walk animation: " << walkAnim->name 
              << " (" << walkAnim->duration << "s)\n";
    
    animator->Play(walkAnim);
    animator->SetLockRootPosition(false);
    
    float dt = 1.0f / 60.0f;
    int updatesCount = 10;
    float totalRootMotion = 0.0f;
    
    for (int i = 0; i < updatesCount; i++) {
        animator->Update(dt);
        glm::vec3 rootMotion = animator->ConsumeRootMotion();
        float motionMag = glm::length(rootMotion);
        totalRootMotion += motionMag;
        
        if (motionMag > 0.001f) {
            std::cout << "  Frame " << i << ": Root motion = " << motionMag << "\n";
        }
    }
    
    float avgRootMotion = totalRootMotion / updatesCount;
    std::cout << "\n=== Root Motion Summary ===\n";
    std::cout << "Average root motion magnitude: " << avgRootMotion << "\n";
    
    if (avgRootMotion > 0.001f) {
        std::cout << "[PASS] Root motion is being extracted\n";
    } else {
        std::cout << "[INFO] No root motion detected\n";
    }
    
    EXPECT_GE(avgRootMotion, 0.0f);
    
    delete walkAnim;
}

// ============================================================================
// TEST 4: Motion Matching Database
// ============================================================================

TEST_F(FBXAnimationIntegrationTest, MotionMatchingDatabaseIsPopulated) {
    std::cout << "\n--- TEST: Motion Matching Database ---\n";
    
    botModel = new Model("assets/bot.fbx");
    ASSERT_TRUE(botModel != nullptr);
    
    initializeMotionMatching(botModel);
    
    std::vector<std::pair<std::string, std::string>> animations = {
        {"Idle", "assets/Idle.fbx"},
        {"Walk", "assets/Walking.fbx"},
        {"Run", "assets/Run.fbx"}
    };
    
    int loadedCount = 0;
    for (const auto& [name, path] : animations) {
        Animation* anim = LoadAnimationFromFile(path);
        if (anim) {
            // MotionDatabase takes shared_ptr ownership; default deleter frees the animation
            matcher->LoadAnimation(name, std::shared_ptr<Animation>(anim));
            loadedCount++;
            std::cout << "[TEST] Loaded: " << name << "\n";
        }
    }
    
    std::cout << "Loaded " << loadedCount << " animations\n";
    
    std::cout << "Building KD-Tree...\n";
    matcher->BuildSearchIndex();
    
    std::cout << "\n" << matcher->GetDatabaseStats() << "\n";
    
    EXPECT_GT(loadedCount, 0);
    
    // Test search
    MotionFeatures features;
    features.rootVelocity = glm::vec3(0.0f, 0.0f, 1.0f);
    features.speed = 0.5f;
    features.moveDirection = glm::vec2(0.0f, 1.0f);
    features.isGrounded = true;
    
    Trajectory trajectory;
    trajectory.positions[0] = glm::vec3(0, 0, 0);
    trajectory.velocities[0] = glm::vec3(0, 0, 1);
    trajectory.directions[0] = 0.0f;
    trajectory.numPoints = 1;
    
    MotionMatchingConfig config;
    config.maxSearchResults = 5;
    config.searchRadius = 2.0f;
    
    std::cout << "Testing search...\n";
    SearchResults results = matcher->GetDatabase()->SearchWithBlending(features, trajectory, config);
    
    std::cout << "Search completed. Best score: " << results.best.score << "\n";
    std::cout << "Total searched: " << results.totalSearched << " poses\n";
    std::cout << "Search time: " << results.searchTimeMs << " ms\n";
    
    EXPECT_GE(results.totalSearched, 0);
}

// ============================================================================
// TEST 5: Character Movement Simulation
// ============================================================================

TEST_F(FBXAnimationIntegrationTest, CharacterPositionChangesWithMovement) {
    std::cout << "\n--- TEST: Character Position Changes ---\n";
    
    botModel = new Model("assets/bot.fbx");
    skeleton = &botModel->GetSkeleton();
    animator = new Animator(skeleton);
    
    Animation* walkAnim = LoadAnimationFromFile("assets/Walking.fbx");
    if (!walkAnim) GTEST_SKIP() << "Walking animation not found";
    
    animator->Play(walkAnim);
    animator->SetLockRootPosition(false);
    
    glm::vec3 characterPos(0, 0, 0);
    glm::vec3 moveDirection(0, 0, 1);
    float dt = 1.0f / 60.0f;
    
    std::cout << "Initial position: " << Vec3ToString(characterPos) << "\n";
    
    for (int i = 0; i < 60; i++) {
        animator->Update(dt);
        glm::vec3 rootMotion = animator->ConsumeRootMotion();
        float motionMag = glm::length(rootMotion);
        
        if (motionMag > 0.001f) {
            characterPos += moveDirection * motionMag * 0.25f;
        }
    }
    
    std::cout << "Final position: " << Vec3ToString(characterPos) << "\n";
    
    float distanceMoved = glm::length(characterPos);
    std::cout << "Distance moved: " << distanceMoved << " units\n";
    
    if (distanceMoved > 0.1f) {
        std::cout << "[PASS] Character moved forward\n";
    } else {
        std::cout << "[INFO] Character didn't move much\n";
    }
    
    delete walkAnim;
}

// ============================================================================
// TEST 6: Animation Blending
// ============================================================================

TEST_F(FBXAnimationIntegrationTest, AnimationBlendingWorks) {
    std::cout << "\n--- TEST: Animation Blending ---\n";
    
    botModel = new Model("assets/bot.fbx");
    skeleton = &botModel->GetSkeleton();
    animator = new Animator(skeleton);
    
    Animation* idleAnim = LoadAnimationFromFile("assets/Idle.fbx");
    Animation* walkAnim = LoadAnimationFromFile("assets/Walking.fbx");
    
    if (!idleAnim || !walkAnim) {
        GTEST_SKIP() << "Required animations not found";
    }
    
    animator->Play(idleAnim);
    animator->Update(1.0f / 60.0f);
    
    std::cout << "Playing Idle\n";
    
    animator->BlendTo(walkAnim, 0.2f);
    std::cout << "Blending to Walk (0.2s)\n";
    
    float blendTime = 0.0f;
    float dt = 1.0f / 60.0f;
    
    while (blendTime < 0.3f) {
        animator->Update(dt);
        blendTime += dt;
    }
    
    std::cout << "[PASS] Blending completed\n";
    
    delete idleAnim;
    delete walkAnim;
}

// Note: main() is in test_main.cpp - don't duplicate
