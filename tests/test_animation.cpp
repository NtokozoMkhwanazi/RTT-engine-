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

#include "animationSystem/Animator.h"  // for the SolveLegIK regression test

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

/**
 * Test: Two-bone knee/leg IK must NOT stretch the leg.
 *
 * SolveLegIK computes a residual ankle translate so the foot reaches the
 * (floor) target. When the target is beyond the leg's (L1+L2) reach, the old
 * code yanked the ankle down UNCLAMPED - the fully-extended leg got dragged
 * past its joint limit, visually STRETCHING the leg and making the foot
 * "plant / twitch" mid-stride. The ankle offset is now clamped to
 * FootIKSettings::maxIKDistance so the leg extends to its max reach instead of
 * elastically stretching. Regression guard for "knees stretch the legs".
 */
TEST_F(AnimationTest, SolveLegIK_DoesNotStretchBeyondMaxDistance) {
    Animator anim(nullptr);
    anim.SetIKWorldScale(1.0f);
    anim.footIKSettings.maxIKDistance = 0.15f;
    anim.footIKSettings.kneeBendWeight = 1.0f;
    anim.footIKSettings.ikKneeClampDeg = 175.0f;

    // Straight leg: hip -> knee -> ankle, 0.5m thigh + 0.5m shin (reach = 1.0m).
    // Identity model matrix => model-space == world-space.
    const int THIGH = 0, SHIN = 1, ANKLE = 2;
    // globalBoneMatrices holds the no-IK animated bone transforms; SolveLegIK
    // samples H/K/A from these (undoing any prior-frame ikRotation), so they
    // must carry the real bone translations — identity matrices yield
    // zero-length legs (L1=L2=0) and a degenerate solve.
    glm::mat4 tHip   = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 tKnee  = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, 0.0f));
    glm::mat4 tAnkle = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f));
    anim.globalBoneMatrices = { tHip, tKnee, tAnkle };
    anim.currBoneWorldPos   = { glm::vec3(0.0f, 1.0f, 0.0f),
                                glm::vec3(0.0f, 0.5f, 0.0f),
                                glm::vec3(0.0f, 0.0f, 0.0f) };

    glm::mat4 model(1.0f);
    glm::mat4 thighRot, shinRot;
    glm::vec3 ankleOff(0.0f), ankleEnd(0.0f), kneeEnd(0.0f);

    // (1) Unreachable target: foot wants to be 0.5m BELOW the ankle - 0.5m
    // past the leg's 1.0m reach. The ankle offset must be clamped to
    // maxIKDistance (NOT 0.5m -> no stretch).
    ASSERT_TRUE(anim.SolveLegIK(THIGH, SHIN, ANKLE, model,
                                glm::vec3(0.0f, -0.5f, 0.0f),
                                thighRot, shinRot, ankleOff, ankleEnd, kneeEnd));
    EXPECT_LE(glm::length(ankleOff), anim.footIKSettings.maxIKDistance + 1e-4f)
        << "unreachable foot must not stretch the leg past maxIKDistance";

    // (2) Reachable target (at the ankle itself): no ankle translate needed.
    ASSERT_TRUE(anim.SolveLegIK(THIGH, SHIN, ANKLE, model,
                                glm::vec3(0.0f, 0.0f, 0.0f),
                                thighRot, shinRot, ankleOff, ankleEnd, kneeEnd));
    EXPECT_LE(glm::length(ankleOff), anim.footIKSettings.maxIKDistance + 1e-4f)
        << "reachable foot must not need ankle translation";

    // (3) Knee never hyperextends/fully locks straight: the two-bone clamp
    // (ikKneeClampDeg = 175) keeps a bent knee, so the interior knee angle is
    // <= ikKneeClampDeg (the leg can't straighten to 180) - i.e. no locked/
    // hyperextended leg on over-reach.
    const glm::vec3 hipW   = glm::vec3(model * glm::vec4(glm::vec3(0.0f, 1.0f, 0.0f), 1.0f));
    const float upperLen  = glm::length(hipW - kneeEnd);
    const float lowerLen  = glm::length(ankleEnd - kneeEnd);
    if (upperLen > 1e-4f && lowerLen > 1e-4f) {
        float cosKnee = glm::dot(hipW - kneeEnd, ankleEnd - kneeEnd) / (upperLen * lowerLen);
        cosKnee = glm::clamp(cosKnee, -1.0f, 1.0f);
        float kneeAngDeg = glm::degrees(acosf(cosKnee));
        EXPECT_LE(kneeAngDeg, 175.0f + 1.0f)
            << "knee must not lock straight past ikKneeClampDeg (hyperextend)";
        EXPECT_GT(kneeAngDeg, 0.0f)
            << "knee angle must be valid (no degenerate/NaN solve)";
    }
}

// =============================================================================
// Foot-IK terrain alignment (todo Part 3, Option A)
// Testable core: ComputeFootTiltQuat maps a +Y-up foot onto a ground normal.
// Flat -> identity (no-op at eval time); a slope -> the align quat that hugs the
// foot to the surface. The actual ikXform injection is gated on foot IK being
// enabled, so the engine/headless path (foot IK off) is unaffected.
// =============================================================================
TEST(AnimatorFootIK, FootTilt_FlatGroundIsIdentity) {
    glm::quat q = ComputeFootTiltQuat(glm::vec3(0.0f, 1.0f, 0.0f));
    EXPECT_NEAR(q.w, 1.0f, 1e-4f);
    EXPECT_NEAR(glm::length(glm::vec3(q.x, q.y, q.z)), 0.0f, 1e-4f);
}

TEST(AnimatorFootIK, FootTilt_AlignsWorldUpToSlopeNormal) {
    // 45 deg ramp rising in +X: surface normal tilts back from vertical toward -X.
    glm::vec3 n = glm::normalize(glm::vec3(-1.0f, 1.0f, 0.0f));
    glm::quat q = ComputeFootTiltQuat(n);
    EXPECT_NEAR(glm::degrees(glm::angle(q)), 45.0f, 1.0f)
        << "tilt angle should equal the slope angle";
    // Rotating world-up by q must reconstruct the slope normal (foot hugs it).
    glm::vec3 up(0.0f, 1.0f, 0.0f);
    glm::vec3 tilted = q * up;            // glm: quat * vec3 rotates vec3
    EXPECT_NEAR(glm::dot(tilted, n), 1.0f, 1e-3f);
    EXPECT_NEAR(glm::length(tilted), 1.0f, 1e-4f);
}

TEST(AnimatorFootIK, FootTilt_DegenerateNormalFallsBackFlat) {
    // A zero-length ground normal must not produce NaN; foot stays flat.
    glm::quat q = ComputeFootTiltQuat(glm::vec3(0.0f, 0.0f, 0.0f));
    EXPECT_NEAR(q.w, 1.0f, 1e-4f);
    EXPECT_TRUE(std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z));
}
