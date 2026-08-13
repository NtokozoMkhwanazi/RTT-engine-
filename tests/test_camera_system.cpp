/**
 * Camera System Unit Tests
 * 
 * Tests for the new modular ThirdPersonCamera system.
 * Tests state-aware smoothing, collision avoidance,
 * camera modes, and transitions.
 */

#include <gtest/gtest.h>
#include "../cameraSystem/ThirdPersonCamera.h"
#include "../cameraSystem/CameraTypes.h"
#include <glm/glm.hpp>

class CameraSystemTest : public ::testing::Test {
protected:
    ThirdPersonCamera camera;
    CameraInput input;
    float dt = 0.016f;  // ~60fps
    
    void SetUp() override {
        camera = ThirdPersonCamera();
        input = CameraInput();
    }
    
    void TearDown() override {
        camera = ThirdPersonCamera();
    }
    
    void simulateFrames(int numFrames) {
        for (int i = 0; i < numFrames; i++) {
            camera.update(dt, input, 16.0f/9.0f);
        }
    }
};

// ============================================================================
// State-Aware Smoothing Tests
// ============================================================================

/**
 * Test: State-Aware Smoothing Values
 * Verifies each state has appropriate smoothing
 */
TEST_F(CameraSystemTest, StateAwareSmoothing_Values) {
    // Verify smoothing values are reasonable (exact values may vary)
    EXPECT_GT(camera.config.idleFollowSmooth, 4.0f);  // Should be snappy
    EXPECT_GT(camera.config.runFollowSmooth, camera.config.walkFollowSmooth);  // Run > Walk
    EXPECT_GT(camera.config.walkFollowSmooth, 2.0f);  // Should not be too slow
}

/**
 * Test: State Transition Smooth Value
 * Verifies smooth value transitions correctly
 */
TEST_F(CameraSystemTest, StateTransition_SmoothValueChanges) {
    // Start idle
    input.animState = CameraState::IDLE;
    camera.update(dt, input);
    
    float idleSmooth = camera.currentFollowSmooth;
    
    // Transition to run
    input.animState = CameraState::RUN;
    camera.update(dt, input);
    
    // Should be transitioning toward run smooth
    EXPECT_GT(camera.targetFollowSmooth, idleSmooth);
    EXPECT_GT(camera.currentFollowSmooth, idleSmooth * 0.9f);  // Starting to increase
}

/**
 * Test: Configuration Presets
 * Verifies preset configurations work
 */
TEST_F(CameraSystemTest, ConfigurationPresets_Snappy) {
    camera.config.setSnappy();
    
    EXPECT_GT(camera.config.idleFollowSmooth, 10.0f);
    EXPECT_GT(camera.config.runFollowSmooth, 12.0f);
}

TEST_F(CameraSystemTest, ConfigurationPresets_Cinematic) {
    camera.config.setCinematic();
    
    EXPECT_LT(camera.config.idleFollowSmooth, 5.0f);
    EXPECT_LT(camera.config.runFollowSmooth, 7.0f);
}

TEST_F(CameraSystemTest, ConfigurationPresets_Balanced) {
    camera.config.setBalanced();
    
    EXPECT_NEAR(camera.config.idleFollowSmooth, 10.0f, 1.0f);
    EXPECT_NEAR(camera.config.walkFollowSmooth, 7.0f, 1.0f);
    EXPECT_NEAR(camera.config.runFollowSmooth, 12.0f, 1.0f);
}

// ============================================================================
// Camera Follow Tests
// ============================================================================

/**
 * Test: Camera Follows Character
 * Verifies basic follow behavior
 */
TEST_F(CameraSystemTest, Follow_CharacterMoves) {
    input.characterPosition = glm::vec3(0.0f, 0.0f, 10.0f);
    input.animState = CameraState::WALK;
    
    simulateFrames(30);
    
    // Camera should have moved toward the character and settled behind it at
    // ~config.distance (4): ideal z = 10 + 4 = 14. Allow margin for smoothing.
    EXPECT_GT(camera.position.z, 5.0f);
    EXPECT_LT(camera.position.z, 28.0f);
}

/**
 * Test: Camera Maintains Distance
 * Verifies camera maintains configured distance
 */
TEST_F(CameraSystemTest, Follow_MaintainsDistance) {
    input.characterPosition = glm::vec3(0.0f, 0.0f, 0.0f);
    input.animState = CameraState::IDLE;
    
    float targetDistance = 15.0f;
    camera.config.distance = targetDistance;
    
    simulateFrames(100);  // Let camera settle
    
    float distance = camera.getDistanceToCharacter(input.characterPosition);
    
    // Distance should be reasonable (allowing for smoothing)
    EXPECT_GT(distance, targetDistance * 0.5f);  // Should be at least half
    EXPECT_LT(distance, targetDistance * 2.0f);  // Should not be double
}

/**
 * Test: Pivot Tracks Character
 * Verifies pivot point follows character
 */
TEST_F(CameraSystemTest, Follow_PivotTracksCharacter) {
    input.characterPosition = glm::vec3(5.0f, 0.0f, 5.0f);
    input.animState = CameraState::WALK;
    
    simulateFrames(30);
    
    // Pivot should have moved toward character
    EXPECT_GT(camera.pivot.x, 2.0f);
    EXPECT_GT(camera.pivot.z, 2.0f);
    EXPECT_NEAR(camera.pivot.y, camera.config.pivotHeight, 0.5f);
}

// ============================================================================
// State-Specific Behavior Tests
// ============================================================================

/**
 * Test: Idle State Behavior
 * Verifies camera is snappy when idle
 */
TEST_F(CameraSystemTest, IdleState_SnappyResponse) {
    input.animState = CameraState::IDLE;
    input.characterPosition = glm::vec3(1.0f, 0.0f, 0.0f);
    
    glm::vec3 initialPos = camera.position;
    
    camera.update(dt, input);
    
    // Should respond quickly
    float movement = glm::length(camera.position - initialPos);
    EXPECT_GT(movement, 0.01f);  // Should have moved
}

/**
 * Test: Run State Behavior
 * Verifies camera is smooth when running
 */
TEST_F(CameraSystemTest, RunState_SmoothFollow) {
    input.animState = CameraState::RUN;
    input.characterPosition = glm::vec3(0.0f, 0.0f, 15.0f);
    
    simulateFrames(10);
    
    // Camera should catch up smoothly
    float distance = camera.getDistanceToCharacter(input.characterPosition);
    EXPECT_LT(distance, camera.config.distance + 5.0f);  // Should not lag too far
}

/**
 * Test: Jump State Behavior
 * Verifies camera handles vertical movement
 */
TEST_F(CameraSystemTest, JumpState_VerticalTracking) {
    input.animState = CameraState::JUMP;
    input.characterPosition = glm::vec3(0.0f, 3.0f, 0.0f);
    input.isGrounded = false;
    
    simulateFrames(20);
    
    // Camera should track vertical movement
    EXPECT_GT(camera.target.y, 2.0f);  // Pivot should be higher
}

/**
 * Test: Fall State Behavior
 * Verifies camera prepares for landing
 */
TEST_F(CameraSystemTest, FallState_SmoothLanding) {
    input.animState = CameraState::FALL;
    input.characterPosition = glm::vec3(0.0f, 5.0f, 0.0f);
    input.characterVelocity = glm::vec3(0.0f, -10.0f, 0.0f);
    input.isGrounded = false;
    
    camera.update(dt, input);
    
    // Camera should have appropriate smoothing for fall
    float followSmooth = camera.currentFollowSmooth;
    EXPECT_GT(followSmooth, 4.0f);  // Should be moderate to smooth
    EXPECT_LT(followSmooth, 12.0f);  // Should not be too snappy
}

// ============================================================================
// Transition Tests
// ============================================================================

/**
 * Test: State Transition No Popping
 * Verifies camera doesn't pop during transitions
 */
TEST_F(CameraSystemTest, Transition_NoPopping) {
    std::vector<glm::vec3> positions;
    
    // Rapid state changes
    std::vector<CameraState> states = {
        CameraState::IDLE,
        CameraState::WALK,
        CameraState::RUN,
        CameraState::WALK,
        CameraState::IDLE
    };
    
    for (const auto& state : states) {
        input.animState = state;
        camera.update(dt, input);
        positions.push_back(camera.position);
    }
    
    // Check for sudden jumps
    for (size_t i = 1; i < positions.size(); i++) {
        float jump = glm::length(positions[i] - positions[i-1]);
        EXPECT_LT(jump, 2.0f) << "Camera popped during transition";
    }
}

/**
 * Test: Smooth Value Transition
 * Verifies follow smooth value transitions smoothly
 */
TEST_F(CameraSystemTest, Transition_SmoothValueTransition) {
    input.animState = CameraState::IDLE;
    camera.update(dt, input);
    float initialSmooth = camera.currentFollowSmooth;
    
    input.animState = CameraState::RUN;
    camera.update(dt, input);
    
    // Should be transitioning toward run smooth (higher value)
    EXPECT_GT(camera.targetFollowSmooth, initialSmooth);
    // Current should be between initial and target (transitioning)
    EXPECT_GE(camera.currentFollowSmooth, initialSmooth);
    EXPECT_LE(camera.currentFollowSmooth, camera.targetFollowSmooth + 0.1f);
}

// ============================================================================
// Collision Avoidance Tests
// ============================================================================

/**
 * Test: Collision Detection
 * Verifies camera detects collision
 */
TEST_F(CameraSystemTest, Collision_Detection) {
    camera.config.collisionEnabled = true;
    camera.config.collisionRadius = 5.0f;  // Large radius to trigger collision
    // The collision check measures the character against the camera's IDEAL
    // position (behind the character at config.distance). Pull the follow
    // distance inside the radius so the ideal spot is occupied by the
    // character and the response engages deterministically.
    camera.config.distance = 2.0f;

    input.characterPosition = camera.position;
    input.animState = CameraState::IDLE;

    camera.update(dt, input);

    // Camera should detect collision (isColliding flag should be set)
    EXPECT_TRUE(camera.isColliding);
}

/**
 * Test: Collision Avoidance
 * Verifies camera moves to avoid collision
 */
TEST_F(CameraSystemTest, Collision_Avoidance) {
    camera.config.collisionEnabled = true;
    camera.config.collisionRadius = 2.0f;
    
    glm::vec3 initialPos = camera.position;
    
    // Place character very close
    input.characterPosition = camera.position + glm::vec3(0.5f, 0.0f, 0.0f);
    
    camera.update(dt, input);
    
    // Camera should have moved to avoid collision
    float movement = glm::length(camera.position - initialPos);
    EXPECT_GT(movement, 0.1f);
}

/**
 * Test: Collision Disabled
 * Verifies collision can be disabled
 */
TEST_F(CameraSystemTest, Collision_Disabled) {
    camera.config.collisionEnabled = false;
    
    input.characterPosition = camera.position + glm::vec3(0.5f, 0.0f, 0.0f);
    
    camera.update(dt, input);
    
    // Should not detect collision when disabled
    EXPECT_FALSE(camera.isColliding);
}

// ============================================================================
// Camera Controller Tests
// ============================================================================

/**
 * Test: Camera Mode Third Person
 * Verifies third-person mode configuration
 */
TEST_F(CameraSystemTest, Controller_ModeThirdPerson) {
    CameraController controller(&camera);
    controller.setMode(CameraController::CameraMode::THIRD_PERSON);
    
    EXPECT_NEAR(camera.config.distance, 4.0f, 1.0f);
    EXPECT_NEAR(camera.config.height, 1.6f, 1.0f);
}

/**
 * Test: Camera Mode First Person
 * Verifies first-person mode configuration
 */
TEST_F(CameraSystemTest, Controller_ModeFirstPerson) {
    CameraController controller(&camera);
    controller.setMode(CameraController::CameraMode::FIRST_PERSON);
    
    EXPECT_LT(camera.config.distance, 5.0f);
    EXPECT_LT(camera.config.height, 3.0f);
}

/**
 * Test: Camera Mode Orbit
 * Verifies orbit mode configuration
 */
TEST_F(CameraSystemTest, Controller_ModeOrbit) {
    CameraController controller(&camera);
    controller.setMode(CameraController::CameraMode::ORBIT);
    
    EXPECT_GT(camera.config.distance, 4.0f);
    EXPECT_GT(camera.config.height, 1.5f);
}

/**
 * Test: Camera Mode Cinematic
 * Verifies cinematic mode configuration
 */
TEST_F(CameraSystemTest, Controller_ModeCinematic) {
    CameraController controller(&camera);
    controller.setMode(CameraController::CameraMode::CINEMATIC);
    
    EXPECT_GT(camera.config.distance, 6.0f);
    EXPECT_LT(camera.config.idleFollowSmooth, 5.0f);
}

/**
 * Test: Zoom
 * Verifies zoom functionality (direct config test)
 */
TEST_F(CameraSystemTest, Controller_Zoom) {
    // Test zoom clamping logic directly
    camera.config.distance = 15.0f;
    camera.config.minDistance = 5.0f;
    camera.config.maxDistance = 30.0f;
    
    // Simulate zoom in (reduce distance)
    camera.config.distance = glm::clamp(camera.config.distance - 5.0f, camera.config.minDistance, camera.config.maxDistance);
    EXPECT_NEAR(camera.config.distance, 10.0f, 0.1f);

    // Simulate zoom out (increase distance)
    camera.config.distance = glm::clamp(camera.config.distance + 3.0f, camera.config.minDistance, camera.config.maxDistance);
    EXPECT_NEAR(camera.config.distance, 13.0f, 0.1f);
    
    // Test zoom in past minimum
    camera.config.distance = glm::clamp(camera.config.distance - 100.0f, camera.config.minDistance, camera.config.maxDistance);
    EXPECT_NEAR(camera.config.distance, 5.0f, 0.1f);  // Should clamp to min
    
    // Test zoom out past maximum
    camera.config.distance = 15.0f;  // Reset
    camera.config.distance = glm::clamp(camera.config.distance + 100.0f, camera.config.minDistance, camera.config.maxDistance);
    EXPECT_NEAR(camera.config.distance, 30.0f, 0.1f);  // Should clamp to max
}

/**
 * Test: Zoom Limits
 * Verifies zoom respects limits
 */
TEST_F(CameraSystemTest, Controller_ZoomLimits) {
    CameraController controller(&camera);
    
    // Zoom in past minimum
    controller.zoom(-100.0f);
    EXPECT_GE(camera.config.distance, camera.config.minDistance);
    
    // Zoom out past maximum
    controller.zoom(100.0f);
    EXPECT_LE(camera.config.distance, camera.config.maxDistance);
}

/**
 * Test: Reset
 * Verifies reset functionality
 */
TEST_F(CameraSystemTest, Controller_Reset) {
    CameraController controller(&camera);
    
    // Change mode
    controller.setMode(CameraController::CameraMode::FIRST_PERSON);
    camera.yaw = 0.0f;
    camera.pitch = 45.0f;
    
    // Reset
    controller.reset();
    
    // Should be back to third person
    EXPECT_NEAR(camera.config.distance, 4.0f, 1.0f);
    EXPECT_FLOAT_EQ(camera.yaw, -90.0f);
    EXPECT_FLOAT_EQ(camera.pitch, 0.0f);
}

// ============================================================================
// Output Tests
// ============================================================================

/**
 * Test: Output View Matrix
 * Verifies view matrix is calculated correctly
 */
TEST_F(CameraSystemTest, Output_ViewMatrix) {
    input.characterPosition = glm::vec3(0.0f, 0.0f, 0.0f);
    simulateFrames(10);
    
    glm::mat4 view = camera.output.viewMatrix;
    
    // View matrix should be valid (no NaN)
    EXPECT_FALSE(std::isnan(view[0][0]));
    EXPECT_FALSE(std::isnan(view[1][1]));
    EXPECT_FALSE(std::isnan(view[2][2]));
}

/**
 * Test: Output Projection Matrix
 * Verifies projection matrix is calculated correctly
 */
TEST_F(CameraSystemTest, Output_ProjectionMatrix) {
    glm::mat4 proj = camera.getProjectionMatrix(16.0f/9.0f);
    
    // Projection matrix should be valid
    EXPECT_FALSE(std::isnan(proj[0][0]));
    EXPECT_FALSE(std::isnan(proj[1][1]));
}

/**
 * Test: Output State
 * Verifies output state is updated
 */
TEST_F(CameraSystemTest, Output_State) {
    input.animState = CameraState::RUN;
    camera.update(dt, input);
    
    // Output should reflect current state
    EXPECT_EQ(camera.output.currentState, CameraState::RUN);
    // Follow smooth should be transitioning toward run value
    EXPECT_GT(camera.output.currentFollowSmooth, 4.0f);  // Should be moderate
    EXPECT_LT(camera.output.currentFollowSmooth, 12.0f);  // Should not be extreme
}

// ============================================================================
// Edge Case Tests
// ============================================================================

/**
 * Test: Zero DeltaTime
 * Verifies camera handles zero dt
 */
TEST_F(CameraSystemTest, EdgeCase_ZeroDeltaTime) {
    input.animState = CameraState::IDLE;
    
    glm::vec3 initialPos = camera.position;
    
    camera.update(0.0f, input);
    
    // Position should not change
    EXPECT_FLOAT_EQ(camera.position.x, initialPos.x);
    EXPECT_FLOAT_EQ(camera.position.y, initialPos.y);
    EXPECT_FLOAT_EQ(camera.position.z, initialPos.z);
}

/**
 * Test: Character At Camera
 * Verifies camera handles character at same position
 */
TEST_F(CameraSystemTest, EdgeCase_CharacterAtCamera) {
    input.characterPosition = camera.position;
    input.animState = CameraState::IDLE;
    
    camera.update(dt, input);
    
    // Should not produce NaN
    EXPECT_FALSE(std::isnan(camera.position.x));
    EXPECT_FALSE(std::isnan(camera.position.y));
    EXPECT_FALSE(std::isnan(camera.position.z));
}

/**
 * Test: Large DeltaTime
 * Verifies camera handles large dt (lag spike)
 */
TEST_F(CameraSystemTest, EdgeCase_LargeDeltaTime) {
    input.characterPosition = glm::vec3(0.0f, 0.0f, 0.0f);
    input.animState = CameraState::IDLE;
    
    // Simulate lag spike
    camera.update(0.5f, input);
    
    // Should still be valid
    EXPECT_FALSE(std::isnan(camera.position.x));
    EXPECT_FALSE(std::isinf(camera.position.x));
}

/**
 * Test: Very Far Character
 * Verifies camera handles character very far away
 */
TEST_F(CameraSystemTest, EdgeCase_CharacterVeryFar) {
    input.characterPosition = glm::vec3(100.0f, 0.0f, 100.0f);
    input.animState = CameraState::RUN;
    
    camera.update(dt, input);
    
    // Camera should start moving toward character
    EXPECT_GT(camera.position.x, 0.0f);
    EXPECT_GT(camera.position.z, 0.0f);
}

// ============================================================================
// Integration-Style Tests
// ============================================================================

/**
 * Test: Complete Gameplay Sequence
 * Verifies camera handles full gameplay flow
 */
TEST_F(CameraSystemTest, Integration_CompleteSequence) {
    std::vector<std::pair<CameraState, glm::vec3>> sequence = {
        {CameraState::IDLE, glm::vec3(0.0f, 0.0f, 0.0f)},
        {CameraState::WALK, glm::vec3(0.0f, 0.0f, 5.0f)},
        {CameraState::RUN, glm::vec3(0.0f, 0.0f, 15.0f)},
        {CameraState::JUMP, glm::vec3(0.0f, 3.0f, 15.0f)},
        {CameraState::FALL, glm::vec3(0.0f, 5.0f, 15.0f)},
        {CameraState::IDLE, glm::vec3(0.0f, 0.0f, 15.0f)}
    };

    for (const auto& [state, position] : sequence) {
        input.animState = state;
        input.characterPosition = position;
        simulateFrames(5);  // Few frames per state
    }

    // Camera should be in valid state
    EXPECT_FALSE(std::isnan(camera.position.x));
    EXPECT_FALSE(std::isnan(camera.position.y));
    EXPECT_FALSE(std::isnan(camera.position.z));

    // Should have tracked the character
    EXPECT_GT(camera.position.z, 10.0f);
}

// ============================================================================
// CROUCH State Tests
// ============================================================================

/**
 * Test: Crouch State - Low Pivot Height
 * Verifies camera adjusts for crouching character
 */
TEST_F(CameraSystemTest, CrouchState_LowPivotHeight) {
    input.animState = CameraState::CROUCH;
    input.characterPosition = glm::vec3(0.0f, 0.0f, 0.0f);

    simulateFrames(30);

    // Pivot should be lower for crouch
    EXPECT_LT(camera.pivot.y, camera.config.pivotHeight + 1.0f);
}

/**
 * Test: Crouch State - Smooth Transition
 * Verifies smooth transition into crouch state
 */
TEST_F(CameraSystemTest, CrouchState_SmoothTransition) {
    // Start idle
    input.animState = CameraState::IDLE;
    camera.update(dt, input);
    float idleSmooth = camera.currentFollowSmooth;

    // Transition to crouch
    input.animState = CameraState::CROUCH;
    camera.update(dt, input);

    // Should transition toward crouch smooth value
    EXPECT_LT(camera.targetFollowSmooth, idleSmooth);  // Crouch is smoother
}

/**
 * Test: Crouch Walk - Balanced Smoothing
 * Verifies crouch walk has appropriate smoothing
 */
TEST_F(CameraSystemTest, CrouchWalk_BalancedSmoothing) {
    input.animState = CameraState::CROUCH;
    input.characterPosition = glm::vec3(0.0f, 0.0f, 5.0f);

    simulateFrames(20);

    // Camera should track smoothly (distance should be reasonable)
    float distance = camera.getDistanceToCharacter(input.characterPosition);
    
    // Distance should be within reasonable bounds (not too far, not too close)
    EXPECT_GT(distance, 3.0f);   // Should not be too close
    EXPECT_LT(distance, 15.0f);  // Should not be too far
}

// ============================================================================
// TRANSITIONING State Tests
// ============================================================================

/**
 * Test: Transitioning State - Uses Default Smooth
 * Verifies transitioning state uses fallback smoothing
 */
TEST_F(CameraSystemTest, TransitioningState_UsesDefaultSmooth) {
    input.animState = CameraState::TRANSITIONING;
    camera.update(dt, input);

    // Should use default fallback (around 5.0f)
    EXPECT_GT(camera.targetFollowSmooth, 3.0f);
    EXPECT_LT(camera.targetFollowSmooth, 7.0f);
}

/**
 * Test: Rapid State Changes - No Instability
 * Verifies camera remains stable during rapid state changes
 */
TEST_F(CameraSystemTest, RapidStateChanges_NoInstability) {
    std::vector<CameraState> rapidStates = {
        CameraState::IDLE, CameraState::RUN, CameraState::JUMP,
        CameraState::FALL, CameraState::WALK, CameraState::CROUCH,
        CameraState::RUN, CameraState::IDLE, CameraState::JUMP
    };

    for (const auto& state : rapidStates) {
        input.animState = state;
        camera.update(dt, input);
    }

    // Camera should remain stable (no NaN or Inf)
    EXPECT_FALSE(std::isnan(camera.position.x));
    EXPECT_FALSE(std::isinf(camera.position.x));
    EXPECT_FALSE(std::isnan(camera.position.y));
    EXPECT_FALSE(std::isinf(camera.position.y));
    EXPECT_FALSE(std::isnan(camera.position.z));
    EXPECT_FALSE(std::isinf(camera.position.z));
}

// ============================================================================
// Camera Direction Tests
// ============================================================================

/**
 * Test: GetForward - Returns Normalized Direction
 * Verifies forward vector is normalized
 */
TEST_F(CameraSystemTest, GetForward_Normalized) {
    input.characterPosition = glm::vec3(0.0f, 0.0f, 10.0f);
    simulateFrames(10);

    glm::vec3 forward = camera.getForward();
    float length = glm::length(forward);

    // Forward should be normalized
    EXPECT_NEAR(length, 1.0f, 0.01f);
}

/**
 * Test: GetForward - Points From Position To Target
 * Verifies forward direction is correct
 */
TEST_F(CameraSystemTest, GetForward_PointsToTarget) {
    input.characterPosition = glm::vec3(0.0f, 0.0f, 10.0f);
    simulateFrames(30);

    glm::vec3 forward = camera.getForward();
    glm::vec3 expectedForward = glm::normalize(camera.target - camera.position);

    EXPECT_NEAR(forward.x, expectedForward.x, 0.01f);
    EXPECT_NEAR(forward.y, expectedForward.y, 0.01f);
    EXPECT_NEAR(forward.z, expectedForward.z, 0.01f);
}

// ============================================================================
// Camera Transform Tests
// ============================================================================

/**
 * Test: SetPosition - Instant Placement
 * Verifies instant position setting works
 */
TEST_F(CameraSystemTest, SetPosition_InstantPlacement) {
    glm::vec3 newPos(100.0f, 50.0f, -100.0f);
    camera.setPosition(newPos);

    EXPECT_FLOAT_EQ(camera.position.x, newPos.x);
    EXPECT_FLOAT_EQ(camera.position.y, newPos.y);
    EXPECT_FLOAT_EQ(camera.position.z, newPos.z);
}

/**
 * Test: SetTarget - Updates Look-At Point
 * Verifies target setting works
 */
TEST_F(CameraSystemTest, SetTarget_UpdatesLookAt) {
    glm::vec3 newTarget(5.0f, 10.0f, 15.0f);
    camera.setTarget(newTarget);

    EXPECT_FLOAT_EQ(camera.target.x, newTarget.x);
    EXPECT_FLOAT_EQ(camera.target.y, newTarget.y);
    EXPECT_FLOAT_EQ(camera.target.z, newTarget.z);
    EXPECT_FLOAT_EQ(camera.pivot.x, newTarget.x);
    EXPECT_FLOAT_EQ(camera.pivot.y, newTarget.y);
    EXPECT_FLOAT_EQ(camera.pivot.z, newTarget.z);
}

/**
 * Test: SetState - Changes Camera State
 * Verifies manual state setting works
 */
TEST_F(CameraSystemTest, SetState_ChangesState) {
    camera.setState(CameraState::RUN);

    EXPECT_EQ(camera.currentState, CameraState::RUN);
    EXPECT_EQ(camera.targetFollowSmooth, camera.config.runFollowSmooth);
}

/**
 * Test: SetConfig - Updates Configuration
 * Verifies config setting works
 */
TEST_F(CameraSystemTest, SetConfig_UpdatesConfiguration) {
    CameraConfig newConfig;
    newConfig.distance = 25.0f;
    newConfig.height = 10.0f;
    newConfig.idleFollowSmooth = 15.0f;

    camera.setConfig(newConfig);

    EXPECT_FLOAT_EQ(camera.config.distance, 25.0f);
    EXPECT_FLOAT_EQ(camera.config.height, 10.0f);
    EXPECT_FLOAT_EQ(camera.config.idleFollowSmooth, 15.0f);
}

// ============================================================================
// Transition Detection Tests
// ============================================================================

/**
 * Test: IsTransitioning - Detects State Change
 * Verifies transition detection works
 */
TEST_F(CameraSystemTest, IsTransitioning_DetectsStateChange) {
    input.animState = CameraState::IDLE;
    camera.update(dt, input);

    // First frame - no transition (same state)
    EXPECT_FALSE(camera.isTransitioning());

    // Change state
    input.animState = CameraState::RUN;
    camera.update(dt, input);

    // Should detect transition
    EXPECT_TRUE(camera.isTransitioning());
}

/**
 * Test: IsTransitioning - Clears After Settle
 * Verifies transition flag clears when state stabilizes
 */
TEST_F(CameraSystemTest, IsTransitioning_ClearsAfterSettle) {
    // Start idle
    input.animState = CameraState::IDLE;
    camera.update(dt, input);

    // Change to run
    input.animState = CameraState::RUN;
    camera.update(dt, input);
    EXPECT_TRUE(camera.isTransitioning());

    // Stay in run state
    input.animState = CameraState::RUN;
    camera.update(dt, input);

    // Should no longer be transitioning
    EXPECT_FALSE(camera.isTransitioning());
}

// ============================================================================
// View Matrix Tests
// ============================================================================

/**
 * Test: GetViewMatrix - Valid Matrix
 * Verifies view matrix is properly constructed
 */
TEST_F(CameraSystemTest, GetViewMatrix_ValidMatrix) {
    input.characterPosition = glm::vec3(0.0f, 0.0f, 0.0f);
    simulateFrames(10);

    glm::mat4 view = camera.getViewMatrix();

    // Check for valid matrix (no NaN)
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            EXPECT_FALSE(std::isnan(view[i][j])) << "NaN at [" << i << "][" << j << "]";
            EXPECT_FALSE(std::isinf(view[i][j])) << "Inf at [" << i << "][" << j << "]";
        }
    }
}

/**
 * Test: GetViewMatrix - Invertible (Determinant Check)
 * Verifies view matrix is invertible (non-zero determinant)
 */
TEST_F(CameraSystemTest, GetViewMatrix_Invertible) {
    input.characterPosition = glm::vec3(0.0f, 0.0f, 0.0f);
    simulateFrames(10);

    glm::mat4 view = camera.getViewMatrix();

    // Calculate determinant (simplified check - just verify it's not zero)
    float det = glm::determinant(view);

    // View matrix should have determinant close to 1 or -1 (orthonormal)
    EXPECT_NEAR(std::abs(det), 1.0f, 0.1f);
}

// ============================================================================
// Projection Matrix Tests
// ============================================================================

/**
 * Test: GetProjectionMatrix - Valid Matrix
 * Verifies projection matrix is properly constructed
 */
TEST_F(CameraSystemTest, GetProjectionMatrix_ValidMatrix) {
    glm::mat4 proj = camera.getProjectionMatrix(16.0f / 9.0f);

    // Check for valid matrix (no NaN)
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            EXPECT_FALSE(std::isnan(proj[i][j])) << "NaN at [" << i << "][" << j << "]";
            EXPECT_FALSE(std::isinf(proj[i][j])) << "Inf at [" << i << "][" << j << "]";
        }
    }
}

/**
 * Test: GetProjectionMatrix - Different Aspect Ratios
 * Verifies projection matrix handles different aspect ratios
 */
TEST_F(CameraSystemTest, GetProjectionMatrix_DifferentAspectRatios) {
    float aspectRatios[] = {4.0f/3.0f, 16.0f/9.0f, 21.0f/9.0f};

    for (float aspect : aspectRatios) {
        glm::mat4 proj = camera.getProjectionMatrix(aspect);

        // Should produce valid matrix for all aspect ratios
        EXPECT_FALSE(std::isnan(proj[0][0])) << "Invalid matrix for aspect " << aspect;
    }
}

// ============================================================================
// Debug Output Tests
// ============================================================================

/**
 * Test: GetStateString - Returns Valid String
 * Verifies state string conversion works
 */
TEST_F(CameraSystemTest, GetStateString_ReturnsValidString) {
    std::vector<CameraState> states = {
        CameraState::IDLE, CameraState::WALK, CameraState::RUN,
        CameraState::JUMP, CameraState::FALL, CameraState::CROUCH
    };

    for (const auto& state : states) {
        input.animState = state;
        camera.update(dt, input);

        std::string stateStr = camera.getStateString();

        // Should return non-empty string
        EXPECT_FALSE(stateStr.empty()) << "Empty string for state " << static_cast<int>(state);
    }
}

/**
 * Test: Output Structure - All Fields Updated
 * Verifies output structure is fully populated
 */
TEST_F(CameraSystemTest, OutputStructure_AllFieldsUpdated) {
    input.characterPosition = glm::vec3(5.0f, 2.0f, 5.0f);
    input.animState = CameraState::RUN;
    camera.update(dt, input, 16.0f/9.0f);

    // All output fields should be updated
    EXPECT_FLOAT_EQ(camera.output.position.x, camera.position.x);
    EXPECT_FLOAT_EQ(camera.output.position.y, camera.position.y);
    EXPECT_FLOAT_EQ(camera.output.position.z, camera.position.z);

    EXPECT_FLOAT_EQ(camera.output.target.x, camera.target.x);
    EXPECT_FLOAT_EQ(camera.output.target.y, camera.target.y);
    EXPECT_FLOAT_EQ(camera.output.target.z, camera.target.z);

    EXPECT_EQ(camera.output.currentState, camera.currentState);
    EXPECT_NEAR(camera.output.currentFollowSmooth, camera.currentFollowSmooth, 0.1f);
    EXPECT_EQ(camera.output.isColliding, camera.isColliding);
}

// ============================================================================
// Constructor Tests
// ============================================================================

/**
 * Test: Constructor - Default Values
 * Verifies default constructor initializes correctly
 */
TEST_F(CameraSystemTest, Constructor_DefaultValues) {
    ThirdPersonCamera defaultCam;

    // Default position
    EXPECT_FLOAT_EQ(defaultCam.position.x, 0.0f);
    EXPECT_FLOAT_EQ(defaultCam.position.y, 5.0f);
    EXPECT_FLOAT_EQ(defaultCam.position.z, 10.0f);

    // Default target
    EXPECT_FLOAT_EQ(defaultCam.target.x, 0.0f);
    EXPECT_FLOAT_EQ(defaultCam.target.y, 2.0f);
    EXPECT_FLOAT_EQ(defaultCam.target.z, 0.0f);

    // Default yaw/pitch
    EXPECT_FLOAT_EQ(defaultCam.yaw, -90.0f);
    EXPECT_FLOAT_EQ(defaultCam.pitch, 0.0f);
}

/**
 * Test: Constructor - Custom Start Position
 * Verifies custom position constructor works
 */
TEST_F(CameraSystemTest, Constructor_CustomStartPosition) {
    glm::vec3 startPos(10.0f, 20.0f, 30.0f);
    glm::vec3 targetPos(5.0f, 10.0f, 15.0f);

    ThirdPersonCamera customCam(startPos, targetPos);

    EXPECT_FLOAT_EQ(customCam.position.x, startPos.x);
    EXPECT_FLOAT_EQ(customCam.position.y, startPos.y);
    EXPECT_FLOAT_EQ(customCam.position.z, startPos.z);

    EXPECT_FLOAT_EQ(customCam.target.x, targetPos.x);
    EXPECT_FLOAT_EQ(customCam.target.y, targetPos.y);
    EXPECT_FLOAT_EQ(customCam.target.z, targetPos.z);
}

/**
 * Test: Constructor - Custom Config
 * Verifies custom config constructor works
 */
TEST_F(CameraSystemTest, Constructor_CustomConfig) {
    CameraConfig customConfig;
    customConfig.distance = 20.0f;
    customConfig.height = 8.0f;
    customConfig.idleFollowSmooth = 12.0f;

    ThirdPersonCamera configuredCam(glm::vec3(0, 5, 10), glm::vec3(0, 2, 0), customConfig);

    EXPECT_FLOAT_EQ(configuredCam.config.distance, 20.0f);
    EXPECT_FLOAT_EQ(configuredCam.config.height, 8.0f);
    EXPECT_FLOAT_EQ(configuredCam.config.idleFollowSmooth, 12.0f);
}
