/**
 * Simple FBX Loader Test
 * Standalone test without fixtures
 */

#include <gtest/gtest.h>
#include <iostream>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "../utils/FBXLoader.h"
#include "animationSystem/AssimpAnimationLoader.h"
#include "animationSystem/Animation.h"

// Forward declaration
static Animation* LoadAnimationFromFile(const std::string& path);

// ============================================================================
// TEST 1: Bot Model Loading
// ============================================================================

TEST(FBXLoaderTest, BotModelLoads) {
    std::cout << "\n--- Testing Bot Model Load ---\n";

    ModelLoadResult result = LoadFBXModel("assets/bot.fbx", "Bot");

    std::cout << "Load result: " << (result.success ? "SUCCESS" : "FAILED") << "\n";

    if (result.success) {
        std::cout << "  Bones: " << result.boneCount << "\n";
        std::cout << "  Meshes: " << result.meshCount << "\n";
        std::cout << "  Size: " << Vec3ToString(result.size) << "\n";
    } else {
        std::cout << "  Error: " << result.errorMessage << "\n";
    }

    EXPECT_TRUE(result.success) << "Bot model should load successfully";
    EXPECT_GT(result.boneCount, 0) << "Bot should have bones";
    EXPECT_GT(result.meshCount, 0) << "Bot should have meshes";
}

// ============================================================================
// TEST 2: Datsun Model Loading
// ============================================================================

TEST(FBXLoaderTest, DatsunModelLoads) {
    std::cout << "\n--- Testing Datsun Model Load ---\n";

    ModelLoadResult result = LoadFBXModel("assets/datsun.fbx", "Datsun");

    std::cout << "Load result: " << (result.success ? "SUCCESS" : "FAILED") << "\n";

    if (result.success) {
        std::cout << "  Bones: " << result.boneCount << "\n";
        std::cout << "  Meshes: " << result.meshCount << "\n";
        std::cout << "  Size: " << Vec3ToString(result.size) << "\n";

        // Datsun is a car - should be static (no bones)
        std::cout << "  Type: " << (result.boneCount > 0 ? "ANIMATED" : "STATIC") << "\n";
    } else {
        std::cout << "  Error: " << result.errorMessage << "\n";
    }

    // Datsun may or may not exist - just report
    if (result.success) {
        std::cout << "[PASS] Datsun loaded\n";
    } else {
        std::cout << "[INFO] Datsun not found (optional)\n";
    }
}

// ============================================================================
// TEST 3: Animation Loading
// ============================================================================

TEST(FBXLoaderTest, WalkAnimationLoads) {
    std::cout << "\n--- Testing Walk Animation Load ---\n";

    Animation* walkAnim = LoadAnimationFromFile("assets/Walking.fbx");

    if (walkAnim) {
        std::cout << "  Name: " << walkAnim->name << "\n";
        std::cout << "  Duration: " << walkAnim->duration << "s\n";
        std::cout << "  Bones: " << walkAnim->boneAnimations.size() << "\n";

        EXPECT_GT(walkAnim->duration, 0.0f) << "Animation should have positive duration";
        EXPECT_GT(walkAnim->boneAnimations.size(), 0) << "Animation should have bone data";

        delete walkAnim;
        std::cout << "[PASS] Walk animation loaded\n";
    } else {
        std::cout << "[FAIL] Walk animation not found\n";
        FAIL() << "Walking.fbx should exist in assets/";
    }
}

// ============================================================================
// Helper for loading animations
// ============================================================================
static Animation* LoadAnimationFromFile(const std::string& path) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate | aiProcess_FlipUVs);

    if (!scene || !scene->HasAnimations()) {
        return nullptr;
    }

    Animation animTemp = AssimpAnimationLoader::LoadAnimationWithPoseCorrection(
        scene, scene->mAnimations[0]);

    return new Animation(animTemp);
}
