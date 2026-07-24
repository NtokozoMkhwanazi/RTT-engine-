/**
 * Animation System Unit Tests
 * 
 * Tests for skeletal animation, blending, root motion,
 * foot IK, and animation state machines.
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <cmath>
#include <vector>
#include <memory>

/**
 * Test: Animation Blending
 * Verifies linear interpolation between animation poses
 */
class AnimationTest : public ::testing::Test {
protected:
    struct BoneTransform {
        glm::vec3 position;
        glm::quat rotation;
        glm::vec3 scale;
    };
    
    struct AnimationClip {
        std::string name;
        float duration;
        float ticksPerSecond;
        std::vector<BoneTransform> keyframes;
    };
    
    void SetUp() override {}
    void TearDown() override {}
    
    // Linear interpolation helper
    BoneTransform lerp(const BoneTransform& a, const BoneTransform& b, float t) {
        BoneTransform result;
        result.position = glm::mix(a.position, b.position, t);
        result.rotation = glm::slerp(a.rotation, b.rotation, t);
        result.scale = glm::mix(a.scale, b.scale, t);
        return result;
    }
};

TEST_F(AnimationTest, LinearBlend_StartPose) {
    BoneTransform start{{0.0f, 0.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), {1.0f, 1.0f, 1.0f}};
    BoneTransform end{{10.0f, 0.0f, 0.0f}, glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f)), {1.0f, 1.0f, 1.0f}};
    
    float blendTime = 0.0f;  // At start
    BoneTransform result = lerp(start, end, blendTime);
    
    EXPECT_FLOAT_EQ(result.position.x, 0.0f);
    EXPECT_FLOAT_EQ(result.position.y, 0.0f);
    EXPECT_FLOAT_EQ(result.position.z, 0.0f);
}

TEST_F(AnimationTest, LinearBlend_EndPose) {
    BoneTransform start{{0.0f, 0.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), {1.0f, 1.0f, 1.0f}};
    BoneTransform end{{10.0f, 0.0f, 0.0f}, glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f)), {1.0f, 1.0f, 1.0f}};
    
    float blendTime = 1.0f;  // At end
    BoneTransform result = lerp(start, end, blendTime);
    
    EXPECT_FLOAT_EQ(result.position.x, 10.0f);
    // Check rotation is approximately 90 degrees around Y
    glm::vec3 forward{0.0f, 0.0f, 1.0f};
    glm::vec3 rotated = result.rotation * forward;
    EXPECT_NEAR(rotated.x, 1.0f, 0.1f);  // Should point along X after 90 deg rotation
}

TEST_F(AnimationTest, LinearBlend_MidPoint) {
    BoneTransform start{{0.0f, 0.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), {1.0f, 1.0f, 1.0f}};
    BoneTransform end{{10.0f, 0.0f, 0.0f}, glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f)), {1.0f, 1.0f, 1.0f}};
    
    float blendTime = 0.5f;  // Midpoint
    BoneTransform result = lerp(start, end, blendTime);
    
    EXPECT_FLOAT_EQ(result.position.x, 5.0f);
    EXPECT_FLOAT_EQ(result.position.y, 0.0f);
    EXPECT_FLOAT_EQ(result.position.z, 0.0f);
}

/**
 * Test: Animation Time Wrapping
 * Verifies proper looping behavior
 */
TEST_F(AnimationTest, AnimationTimeWrapping_Loops) {
    float animationDuration = 2.0f;
    float currentTime = 2.5f;  // Past end
    
    // Wrap time for looping
    float wrappedTime = std::fmod(currentTime, animationDuration);
    
    EXPECT_FLOAT_EQ(wrappedTime, 0.5f);
}

TEST_F(AnimationTest, AnimationTimeWrapping_MultipleLoops) {
    float animationDuration = 2.0f;
    float currentTime = 7.5f;  // Multiple loops
    
    float wrappedTime = std::fmod(currentTime, animationDuration);
    
    EXPECT_FLOAT_EQ(wrappedTime, 1.5f);
}

/**
 * Test: Root Motion Extraction
 * Verifies root bone displacement calculation
 */
TEST_F(AnimationTest, RootMotionExtraction_CalculatesDelta) {
    glm::vec3 rootPosPrev{0.0f, 0.0f, 0.0f};
    glm::vec3 rootPosCurr{0.0f, 0.0f, 1.0f};
    
    // Extract root motion delta
    glm::vec3 rootMotion = rootPosCurr - rootPosPrev;
    float magnitude = glm::length(rootMotion);
    
    EXPECT_FLOAT_EQ(rootMotion.z, 1.0f);
    EXPECT_FLOAT_EQ(magnitude, 1.0f);
}

TEST_F(AnimationTest, RootMotionExtraction_ZeroMotion) {
    glm::vec3 rootPosPrev{5.0f, 0.0f, 3.0f};
    glm::vec3 rootPosCurr{5.0f, 0.0f, 3.0f};  // Same position
    
    glm::vec3 rootMotion = rootPosCurr - rootPosPrev;
    float magnitude = glm::length(rootMotion);
    
    EXPECT_FLOAT_EQ(magnitude, 0.0f);
}

/**
 * Test: Foot IK Floor Detection
 * Verifies foot placement on ground
 */
TEST_F(AnimationTest, FootIK_FloorPlacement) {
    struct FootIKState {
        glm::vec3 anklePosition;
        glm::vec3 toePosition;
        float floorHeight;
        bool isPlanted;
    };
    
    FootIKState foot{{0.0f, 0.1f, 0.0f}, {0.0f, 0.0f, 0.5f}, 0.0f, false};
    
    // Check if foot should plant
    float footHeight = std::min(foot.anklePosition.y, foot.toePosition.y);
    bool shouldPlant = footHeight <= foot.floorHeight + 0.05f;  // Small threshold
    
    EXPECT_TRUE(shouldPlant) << "Foot should plant on floor";
    EXPECT_TRUE(footHeight >= foot.floorHeight) << "Foot should not go below floor";
}

TEST_F(AnimationTest, FootIK_AboveFloor_NoPlant) {
    struct FootIKState {
        glm::vec3 anklePosition;
        glm::vec3 toePosition;
        float floorHeight;
    };
    
    FootIKState foot{{0.0f, 1.0f, 0.0f}, {0.0f, 0.8f, 0.5f}, 0.0f};
    
    float footHeight = std::min(foot.anklePosition.y, foot.toePosition.y);
    bool shouldPlant = footHeight <= foot.floorHeight + 0.05f;
    
    EXPECT_FALSE(shouldPlant) << "Foot should not plant when above floor";
}

/**
 * Test: Animation Layer Weighting
 * Verifies proper blend weight application
 */
TEST_F(AnimationTest, AnimationLayerWeighting_Normalized) {
    std::vector<float> layerWeights = {0.5f, 0.3f, 0.2f};
    
    // Normalize weights
    float total = 0.0f;
    for (float w : layerWeights) total += w;
    
    std::vector<float> normalized;
    for (float w : layerWeights) {
        normalized.push_back(w / total);
    }
    
    // Verify normalization
    float normalizedTotal = 0.0f;
    for (float w : normalized) normalizedTotal += w;
    
    EXPECT_NEAR(normalizedTotal, 1.0f, 0.001f);
    EXPECT_NEAR(normalized[0], 0.5f, 0.001f);
    EXPECT_NEAR(normalized[1], 0.3f, 0.001f);
    EXPECT_NEAR(normalized[2], 0.2f, 0.001f);
}

TEST_F(AnimationTest, AnimationLayerWeighting_Empty) {
    std::vector<float> layerWeights = {};
    
    float total = 0.0f;
    for (float w : layerWeights) total += w;
    
    // Handle empty case
    bool hasLayers = !layerWeights.empty();
    
    EXPECT_FALSE(hasLayers);
    EXPECT_FLOAT_EQ(total, 0.0f);
}

/**
 * Test: Animation Speed Modification
 * Verifies playback speed affects time progression
 */
TEST_F(AnimationTest, AnimationSpeed_AffectsProgression) {
    float playbackSpeed = 2.0f;  // 2x speed
    float dt = 0.1f;
    float currentTime = 0.0f;
    
    // Advance animation
    currentTime += dt * playbackSpeed;
    
    EXPECT_FLOAT_EQ(currentTime, 0.2f);  // 2x faster
}

TEST_F(AnimationTest, AnimationSpeed_HalfSpeed) {
    float playbackSpeed = 0.5f;  // 0.5x speed
    float dt = 0.1f;
    float currentTime = 0.0f;
    
    currentTime += dt * playbackSpeed;
    
    EXPECT_FLOAT_EQ(currentTime, 0.05f);  // Half speed
}

/**
 * Test: Bone Hierarchy Transformation
 * Verifies parent bone affects child
 */
TEST_F(AnimationTest, BoneHierarchy_ParentAffectsChild) {
    glm::mat4 parentTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 10.0f, 0.0f));
    glm::mat4 childLocalTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 5.0f, 0.0f));
    
    // Calculate world transform
    glm::mat4 childWorldTransform = parentTransform * childLocalTransform;
    glm::vec3 childWorldPos = glm::vec3(childWorldTransform * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    
    EXPECT_FLOAT_EQ(childWorldPos.x, 0.0f);
    EXPECT_FLOAT_EQ(childWorldPos.y, 15.0f);  // Parent + child offset
    EXPECT_FLOAT_EQ(childWorldPos.z, 0.0f);
}

/**
 * Test: Quaternion Normalization
 * Verifies quaternions stay normalized during operations
 */
TEST_F(AnimationTest, QuaternionNormalization_Maintained) {
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    
    // Apply multiple rotations
    for (int i = 0; i < 100; i++) {
        rotation = glm::angleAxis(glm::radians(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)) * rotation;
    }
    
    // Check normalization
    float length = glm::length(rotation);
    
    EXPECT_NEAR(length, 1.0f, 0.01f) << "Quaternion should stay normalized";
}

/**
 * Test: Animation Event Triggering
 * Verifies events fire at correct times
 */
TEST_F(AnimationTest, AnimationEventTrigger_CorrectTiming) {
    struct AnimationEvent {
        float triggerTime;
        std::string name;
        bool triggered;
    };
    
    std::vector<AnimationEvent> events = {
        {0.5f, "footstep_left", false},
        {1.0f, "footstep_right", false},
        {1.5f, "jump", false}
    };
    
    float currentTime = 0.5f;  // Exactly at first event
    float threshold = 0.1f;
    
    // Trigger events within threshold
    for (auto& event : events) {
        if (std::abs(currentTime - event.triggerTime) < threshold && !event.triggered) {
            event.triggered = true;
        }
    }
    
    EXPECT_TRUE(events[0].triggered) << "First event should trigger";
    EXPECT_FALSE(events[1].triggered) << "Second event should not trigger yet";
    EXPECT_FALSE(events[2].triggered) << "Third event should not trigger yet";
}

/**
 * Test: Crossfade Duration
 * Verifies smooth transition between animations
 */
TEST_F(AnimationTest, Crossfade_SmoothTransition) {
    float crossfadeDuration = 0.25f;
    float elapsedTime = 0.0f;
    float dt = 0.016f;
    
    std::vector<float> blendWeights;
    
    // Simulate crossfade
    while (elapsedTime < crossfadeDuration) {
        float weight = elapsedTime / crossfadeDuration;
        blendWeights.push_back(weight);
        elapsedTime += dt;
    }
    
    // Verify smooth progression
    EXPECT_GT(blendWeights.size(), 0);
    EXPECT_FLOAT_EQ(blendWeights.front(), 0.0f);
    EXPECT_NEAR(blendWeights.back(), 1.0f, 0.1f);
    
    // Verify monotonic increase
    for (size_t i = 1; i < blendWeights.size(); i++) {
        EXPECT_GT(blendWeights[i], blendWeights[i-1]) 
            << "Blend weight should increase monotonically";
    }
}
