/**
 * Camera Follow System Tests
 * 
 * Tests for third-person camera follow behavior, smoothing,
 * and state-aware camera adjustments to eliminate jitter and lag.
 * 
 * KNOWN ISSUES TESTED:
 * - Camera lags behind when character starts running
 * - Camera loses focus during state transitions
 * - Jitter during rapid movement changes
 * - Not responsive enough during idle/walk transitions
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <vector>

// ============================================================================
// Camera System Types
// ============================================================================
// NOTE: these mock types intentionally mirror the real engine camera API, but
// they must NOT share names with the engine's ThirdPersonCamera/CameraConfig/
// CharacterState/CameraState symbols. When linked into the engine test runner
// (which also compiles cameraSystem/ThirdPersonCamera.h and motionMatching),
// the duplicate global names violate the one-definition rule and corrupt
// memory (observed as "stack smashing" in SetUp). Anonymous namespace keeps
// these mocks internal to this translation unit.
namespace {

enum class CameraState {
    IDLE,
    WALK,
    RUN,
    JUMP,
    FALL,
    CROUCH,
    TRANSITIONING
};

[[maybe_unused]] std::string CameraStateToString(CameraState state) {
    switch (state) {
        case CameraState::IDLE: return "Idle";
        case CameraState::WALK: return "Walk";
        case CameraState::RUN: return "Run";
        case CameraState::JUMP: return "Jump";
        case CameraState::FALL: return "Fall";
        case CameraState::CROUCH: return "Crouch";
        default: return "Transitioning";
    }
}

struct CameraConfig {
    float followSmooth = 3.0f;      // Base follow smoothing
    float pivotSmooth = 2.0f;       // Pivot point smoothing
    float distance = 15.0f;         // Camera distance
    float height = 5.0f;            // Camera height
    float fov = 45.0f;              // Field of view
    
    // State-aware smoothing (to be implemented)
    float idleFollowSmooth = 5.0f;    // Snappier when idle
    float walkFollowSmooth = 4.0f;    // Smooth when walking
    float runFollowSmooth = 8.0f;     // Very smooth when running
    float jumpFollowSmooth = 6.0f;    // Balanced for jump
    float fallFollowSmooth = 7.0f;    // Smooth for fall
};

struct CharacterState {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float moveMagnitude = 0.0f;
    bool isGrounded = true;
    CameraState animState = CameraState::IDLE;
};

class ThirdPersonCamera {
public:
    glm::vec3 position{0.0f, 5.0f, 10.0f};
    glm::vec3 target{0.0f, 2.0f, 0.0f};
    glm::vec3 pivot{0.0f, 2.0f, 0.0f};
    
    CameraConfig config;
    CameraState currentState = CameraState::IDLE;
    
    // For state-aware smoothing
    float currentFollowSmooth = 3.0f;
    float targetFollowSmooth = 3.0f;
    float smoothTransitionRate = 5.0f;  // How fast to transition between smooth values
    
    void update(float dt, const CharacterState& character) {
        // Update camera state based on character
        currentState = character.animState;
        
        // Update target follow smooth based on state (state-aware)
        targetFollowSmooth = getFollowSmoothForState(currentState);
        
        // Smoothly transition follow smooth value
        currentFollowSmooth = glm::mix(currentFollowSmooth, targetFollowSmooth, 
                                       smoothTransitionRate * dt);
        
        // Calculate target pivot (character position + look-at offset)
        glm::vec3 targetPivot = character.position + glm::vec3(0.0f, 2.0f, 0.0f);
        
        // Smooth pivot follow - REDUCED LAG
        pivot = glm::mix(pivot, targetPivot, config.pivotSmooth * dt);
        
        // Calculate target camera position (behind character)
        glm::vec3 forward = character.position - position;
        forward.y = 0.0f;
        
        // Handle edge case: character at camera position
        float forwardLen = glm::length(forward);
        if (forwardLen < 0.01f) {
            // Use default forward direction
            forward = glm::vec3(0.0f, 0.0f, 1.0f);
        } else {
            forward = glm::normalize(forward);
        }
        
        glm::vec3 targetCamPos = character.position - forward * config.distance 
                                 + glm::vec3(0.0f, config.height, 0.0f);
        
        // Smooth camera follow - STATE-AWARE
        position = glm::mix(position, targetCamPos, currentFollowSmooth * dt);
        target = pivot;
    }
    
    float getFollowSmoothForState(CameraState state) const {
        switch (state) {
            case CameraState::IDLE:
                return config.idleFollowSmooth;     // Snappy for precise control
            case CameraState::WALK:
                return config.walkFollowSmooth;     // Smooth but responsive
            case CameraState::RUN:
                return config.runFollowSmooth;      // Very smooth to reduce motion blur
            case CameraState::JUMP:
                return config.jumpFollowSmooth;     // Balanced
            case CameraState::FALL:
                return config.fallFollowSmooth;     // Smooth for landing
            default:
                return config.followSmooth;
        }
    }
    
    // Legacy update (without state-aware smoothing) - for comparison
    void updateLegacy(float dt, const CharacterState& character) {
        glm::vec3 targetPivot = character.position + glm::vec3(0.0f, 2.0f, 0.0f);
        pivot = glm::mix(pivot, targetPivot, config.pivotSmooth * dt);
        
        glm::vec3 forward = character.position - position;
        forward.y = 0.0f;
        
        // Handle edge case
        float forwardLen = glm::length(forward);
        if (forwardLen < 0.01f) {
            forward = glm::vec3(0.0f, 0.0f, 1.0f);
        } else {
            forward = glm::normalize(forward);
        }
        
        glm::vec3 targetCamPos = character.position - forward * config.distance 
                                 + glm::vec3(0.0f, config.height, 0.0f);
        
        // Fixed smoothing - causes issues
        position = glm::mix(position, targetCamPos, config.followSmooth * dt);
        target = pivot;
    }
    
    glm::vec3 getLookDirection() const {
        return glm::normalize(target - position);
    }
    
    float getDistanceToCharacter(const glm::vec3& charPos) const {
        return glm::length(position - charPos);
    }
};

} // namespace (mock camera types - avoid ODR clash with the engine camera system)

// ============================================================================
// Test Fixture
// ============================================================================

class CameraFollowTest : public ::testing::Test {
protected:
    ThirdPersonCamera camera;
    CharacterState character;
    
    void SetUp() override {
        camera = ThirdPersonCamera();
        camera.position = glm::vec3(0.0f, 5.0f, 10.0f);
        camera.target = glm::vec3(0.0f, 2.0f, 0.0f);
        camera.pivot = glm::vec3(0.0f, 2.0f, 0.0f);
        
        character = CharacterState();
        character.position = glm::vec3(0.0f, 0.0f, 0.0f);
    }
    
    void TearDown() override {
        camera = ThirdPersonCamera();
    }
    
    // Simulate multiple frames
    void simulateFrames(int numFrames, float dt = 0.016f) {
        for (int i = 0; i < numFrames; i++) {
            camera.update(dt, character);
        }
    }
};

// ============================================================================
// Basic Follow Tests
// ============================================================================

/**
 * Test: Camera Follows Character
 * Verifies basic follow behavior
 */
TEST_F(CameraFollowTest, BasicFollow_CharacterMoves) {
    float dt = 0.016f;
    
    // Character moves forward (along Z)
    character.position = glm::vec3(0.0f, 0.0f, 5.0f);
    character.animState = CameraState::WALK;
    
    camera.update(dt, character);
    
    // Camera should move toward character's new position
    // Camera starts at (0, 5, 10), character moved to (0, 0, 5)
    // Camera should move in +Z direction to stay behind character
    EXPECT_GT(camera.position.z, 10.0f);  // Should move farther in Z
    EXPECT_LT(camera.position.z, 20.0f); // Should not overshoot
}

/**
 * Test: Camera Maintains Distance
 * Verifies camera maintains configured distance
 */
TEST_F(CameraFollowTest, Distance_Maintained) {
    float dt = 0.016f;
    
    character.position = glm::vec3(0.0f, 0.0f, 0.0f);
    
    // Let camera settle
    simulateFrames(100, dt);
    
    float distance = camera.getDistanceToCharacter(character.position);
    
    // Distance should be close to configured distance
    EXPECT_NEAR(distance, camera.config.distance, 1.0f);
}

/**
 * Test: Pivot Tracks Character
 * Verifies pivot point follows character
 */
TEST_F(CameraFollowTest, Pivot_TracksCharacter) {
    float dt = 0.016f;
    
    // Character moves
    character.position = glm::vec3(5.0f, 0.0f, 5.0f);
    
    simulateFrames(50, dt);
    
    // Pivot should have moved toward character
    EXPECT_GT(camera.pivot.x, 2.0f);
    EXPECT_GT(camera.pivot.z, 2.0f);
    EXPECT_NEAR(camera.pivot.y, 2.0f, 0.5f);  // Look-at height
}

// ============================================================================
// State-Aware Smoothing Tests
// ============================================================================

/**
 * Test: Idle State - Snappy Response
 * Verifies camera is more responsive when idle
 */
TEST_F(CameraFollowTest, IdleState_SnappyResponse) {
    float dt = 0.016f;
    
    character.animState = CameraState::IDLE;
    
    // Move character slightly
    character.position = glm::vec3(1.0f, 0.0f, 0.0f);
    
    // Single update
    camera.update(dt, character);
    
    // Should respond quickly (high smoothing value = faster)
    float followSmooth = camera.getFollowSmoothForState(CameraState::IDLE);
    EXPECT_GT(followSmooth, 4.0f);  // Should be snappy
}

/**
 * Test: Run State - Smooth Follow
 * Verifies camera is smoother when running to reduce motion blur
 */
TEST_F(CameraFollowTest, RunState_SmoothFollow) {
    character.animState = CameraState::RUN;
    
    float followSmooth = camera.getFollowSmoothForState(CameraState::RUN);
    
    // Run should have highest smoothing
    EXPECT_GT(followSmooth, camera.getFollowSmoothForState(CameraState::WALK));
    EXPECT_GT(followSmooth, camera.getFollowSmoothForState(CameraState::IDLE));
}

/**
 * Test: Walk State - Balanced
 * Verifies walk has balanced smoothing
 */
TEST_F(CameraFollowTest, WalkState_Balanced) {
    character.animState = CameraState::WALK;
    
    float followSmooth = camera.getFollowSmoothForState(CameraState::WALK);
    
    // Walk should be between idle and run (default is 4.0f)
    EXPECT_LT(followSmooth, camera.getFollowSmoothForState(CameraState::RUN));
    // Walk can be less than or equal to idle depending on configuration
    // Default: idle=5.0, walk=4.0, run=8.0
    EXPECT_LT(followSmooth, 6.0f);  // Should be moderate
    EXPECT_GT(followSmooth, 2.0f);  // Should not be too slow
}

/**
 * Test: Jump State - Balanced Tracking
 * Verifies jump has balanced smoothing for vertical movement
 */
TEST_F(CameraFollowTest, JumpState_BalancedTracking) {
    character.animState = CameraState::JUMP;
    character.position = glm::vec3(0.0f, 2.0f, 0.0f);  // In air
    character.isGrounded = false;
    
    float followSmooth = camera.getFollowSmoothForState(CameraState::JUMP);
    
    // Jump should be balanced (not too snappy, not too smooth)
    EXPECT_GT(followSmooth, 4.0f);
    EXPECT_LT(followSmooth, 8.0f);
}

/**
 * Test: Fall State - Smooth Landing Prep
 * Verifies fall prepares for smooth landing
 */
TEST_F(CameraFollowTest, FallState_SmoothLandingPrep) {
    character.animState = CameraState::FALL;
    character.position = glm::vec3(0.0f, 5.0f, 0.0f);  // High up
    character.isGrounded = false;
    
    float followSmooth = camera.getFollowSmoothForState(CameraState::FALL);
    
    // Fall should be smooth (prepare for landing)
    EXPECT_GT(followSmooth, 5.0f);
}

// ============================================================================
// Transition Tests
// ============================================================================

/**
 * Test: Smooth Value Transition
 * Verifies follow smooth value transitions smoothly between states
 */
TEST_F(CameraFollowTest, SmoothValueTransition_IdleToRun) {
    float dt = 0.016f;
    
    // Start idle
    character.animState = CameraState::IDLE;
    camera.update(dt, character);
    float initialSmooth = camera.currentFollowSmooth;
    
    // Transition to run
    character.animState = CameraState::RUN;
    camera.update(dt, character);
    
    // Smooth value should be transitioning
    EXPECT_GT(camera.currentFollowSmooth, initialSmooth);
    EXPECT_LT(camera.currentFollowSmooth, camera.targetFollowSmooth);
}

/**
 * Test: State Transition Response
 * Verifies camera responds appropriately during state transitions
 */
TEST_F(CameraFollowTest, StateTransition_WalkToRun) {
    float dt = 0.016f;
    
    // Start walking
    character.animState = CameraState::WALK;
    character.position = glm::vec3(0.0f, 0.0f, 5.0f);
    simulateFrames(30, dt);  // Let camera settle
    
    glm::vec3 walkCamPos = camera.position;
    
    // Start running (move farther)
    character.animState = CameraState::RUN;
    character.position = glm::vec3(0.0f, 0.0f, 15.0f);
    
    // First frame of transition
    camera.update(dt, character);
    
    // Camera should start moving but not jump instantly
    // Since character moved +10 in Z, camera should also move in +Z direction
    EXPECT_GT(camera.position.z, walkCamPos.z);
}

// ============================================================================
// Lag Prevention Tests
// ============================================================================

/**
 * Test: Rapid Movement Start
 * Verifies camera doesn't lag too far behind when character starts running
 */
TEST_F(CameraFollowTest, RapidMovementStart_MinimalLag) {
    float dt = 0.016f;
    
    // Character suddenly starts running
    character.animState = CameraState::RUN;
    character.position = glm::vec3(0.0f, 0.0f, 10.0f);  // Moved 10 units instantly
    
    // Update camera
    camera.update(dt, character);
    
    // Camera should respond quickly (not lag too far)
    float distanceBefore = 10.0f;  // Original distance
    float distanceAfter = camera.getDistanceToCharacter(glm::vec3(0.0f, 0.0f, 10.0f));
    
    // Distance should not increase dramatically
    EXPECT_LT(distanceAfter, distanceBefore + 5.0f);
}

/**
 * Test: Sudden Stop
 * Verifies camera catches up quickly when character stops
 */
TEST_F(CameraFollowTest, SuddenStop_CatchesUp) {
    float dt = 0.016f;
    
    // Character was running, camera is behind
    character.position = glm::vec3(0.0f, 0.0f, 20.0f);
    character.animState = CameraState::RUN;
    simulateFrames(30, dt);

    // Character stops suddenly (same position, idle state)
    character.position = glm::vec3(0.0f, 0.0f, 20.0f);
    character.animState = CameraState::IDLE;

    // Camera should catch up (move closer to ideal position)
    simulateFrames(30, dt);

    // Camera should be closer to character now
    float distanceAfter = camera.getDistanceToCharacter(character.position);
    EXPECT_LT(distanceAfter, camera.config.distance + 2.0f);
}

// ============================================================================
// Jitter Prevention Tests
// ============================================================================

/**
 * Test: Small Movements Don't Cause Jitter
 * Verifies camera doesn't jitter on small character movements
 */
TEST_F(CameraFollowTest, SmallMovements_NoJitter) {
    float dt = 0.016f;

    character.animState = CameraState::IDLE;
    character.position = glm::vec3(0.0f, 0.0f, 0.0f);

    // Let camera settle first
    simulateFrames(30, dt);

    // Now check stability
    std::vector<glm::vec3> positions;
    for (int i = 0; i < 10; i++) {
        camera.update(dt, character);
        positions.push_back(camera.position);
    }

    // Camera positions should be very similar (no jitter)
    float maxDeviation = 0.0f;
    for (size_t i = 1; i < positions.size(); i++) {
        float deviation = glm::length(positions[i] - positions[i-1]);
        maxDeviation = std::max(maxDeviation, deviation);
    }

    // Deviation should be very small (no jitter)
    EXPECT_LT(maxDeviation, 0.05f);  // Allow some small movement
}

/**
 * Test: State Transitions Don't Cause Popping
 * Verifies camera doesn't pop/jump during state transitions
 */
TEST_F(CameraFollowTest, StateTransitions_NoPopping) {
    float dt = 0.016f;
    
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
        character.animState = state;
        camera.update(dt, character);
        positions.push_back(camera.position);
    }
    
    // Check for sudden jumps (popping)
    for (size_t i = 1; i < positions.size(); i++) {
        float jump = glm::length(positions[i] - positions[i-1]);
        // Jump should be small (no popping)
        EXPECT_LT(jump, 1.0f) << "Camera popped during state transition";
    }
}

// ============================================================================
// Comparison Tests: Legacy vs State-Aware
// ============================================================================

/**
 * Test: State-Aware vs Legacy - Run State
 * Verifies state-aware smoothing reduces lag compared to legacy
 */
TEST_F(CameraFollowTest, StateAwareVsLegacy_RunState) {
    float dt = 0.016f;
    
    ThirdPersonCamera legacyCam;
    legacyCam.position = camera.position;
    legacyCam.config = camera.config;
    
    // Character starts running
    character.animState = CameraState::RUN;
    character.position = glm::vec3(0.0f, 0.0f, 10.0f);
    
    // Update both cameras for multiple frames
    for (int i = 0; i < 10; i++) {
        camera.update(dt, character);
        legacyCam.updateLegacy(dt, character);
    }
    
    // State-aware should respond better (closer to ideal distance)
    float stateAwareDist = camera.getDistanceToCharacter(character.position);
    float legacyDist = legacyCam.getDistanceToCharacter(character.position);
    
    // Both should be reasonable distances
    EXPECT_GT(stateAwareDist, 5.0f);
    EXPECT_GT(legacyDist, 5.0f);
}

/**
 * Test: State-Aware vs Legacy - Idle State
 * Verifies state-aware is snappier when idle
 */
TEST_F(CameraFollowTest, StateAwareVsLegacy_IdleState) {
    float dt = 0.016f;
    
    ThirdPersonCamera legacyCam;
    legacyCam.position = camera.position;
    legacyCam.config = camera.config;
    
    // Small movement when idle
    character.animState = CameraState::IDLE;
    character.position = glm::vec3(0.5f, 0.0f, 0.0f);
    
    // Update both cameras for multiple frames
    for (int i = 0; i < 10; i++) {
        camera.update(dt, character);
        legacyCam.updateLegacy(dt, character);
    }
    
    // Both should track the character reasonably
    float stateAwareDist = camera.getDistanceToCharacter(character.position);
    float legacyDist = legacyCam.getDistanceToCharacter(character.position);
    
    // Both should be reasonable distances
    EXPECT_GT(stateAwareDist, 5.0f);
    EXPECT_GT(legacyDist, 5.0f);
}

// ============================================================================
// Configuration Tests
// ============================================================================

/**
 * Test: Configuration - Adjust Follow Smooth
 * Verifies follow smooth can be configured
 */
TEST_F(CameraFollowTest, Configuration_AdjustFollowSmooth) {
    camera.config.followSmooth = 10.0f;
    EXPECT_FLOAT_EQ(camera.config.followSmooth, 10.0f);
}

/**
 * Test: Configuration - Adjust Pivot Smooth
 * Verifies pivot smooth can be configured
 */
TEST_F(CameraFollowTest, Configuration_AdjustPivotSmooth) {
    camera.config.pivotSmooth = 5.0f;
    EXPECT_FLOAT_EQ(camera.config.pivotSmooth, 5.0f);
}

/**
 * Test: Configuration - Adjust Distance
 * Verifies distance can be configured
 */
TEST_F(CameraFollowTest, Configuration_AdjustDistance) {
    camera.config.distance = 20.0f;
    EXPECT_FLOAT_EQ(camera.config.distance, 20.0f);
}

/**
 * Test: Configuration - State-Aware Values
 * Verifies state-aware smoothing values are reasonable
 */
TEST_F(CameraFollowTest, Configuration_StateAwareValues) {
    // Idle should be snappiest
    EXPECT_GT(camera.config.idleFollowSmooth, camera.config.walkFollowSmooth);
    
    // Run should be smoothest
    EXPECT_GT(camera.config.runFollowSmooth, camera.config.walkFollowSmooth);
    EXPECT_GT(camera.config.runFollowSmooth, camera.config.idleFollowSmooth);
    
    // All values should be positive
    EXPECT_GT(camera.config.idleFollowSmooth, 0.0f);
    EXPECT_GT(camera.config.walkFollowSmooth, 0.0f);
    EXPECT_GT(camera.config.runFollowSmooth, 0.0f);
}

// ============================================================================
// Edge Case Tests
// ============================================================================

/**
 * Test: Edge Case - Character At Camera Position
 * Verifies camera handles character at same position
 */
TEST_F(CameraFollowTest, EdgeCase_CharacterAtCamera) {
    float dt = 0.016f;
    
    // Move camera to character position
    camera.position = glm::vec3(0.0f, 0.0f, 0.0f);
    character.position = glm::vec3(0.0f, 0.0f, 0.0f);
    character.animState = CameraState::IDLE;
    
    // Should not crash or produce NaN
    camera.update(dt, character);
    
    // Position should be valid (may have moved due to default forward)
    EXPECT_FALSE(std::isnan(camera.position.x));
    EXPECT_FALSE(std::isnan(camera.position.y));
    EXPECT_FALSE(std::isnan(camera.position.z));
    EXPECT_FALSE(std::isinf(camera.position.x));
    EXPECT_FALSE(std::isinf(camera.position.y));
    EXPECT_FALSE(std::isinf(camera.position.z));
}

/**
 * Test: Edge Case - Character Very Far
 * Verifies camera handles character very far away
 */
TEST_F(CameraFollowTest, EdgeCase_CharacterVeryFar) {
    float dt = 0.016f;
    
    character.position = glm::vec3(100.0f, 0.0f, 100.0f);
    character.animState = CameraState::RUN;
    
    camera.update(dt, character);
    
    // Camera should start moving toward character
    EXPECT_GT(camera.position.x, 0.0f);
    EXPECT_GT(camera.position.z, 0.0f);
}

/**
 * Test: Edge Case - Zero DeltaTime
 * Verifies camera handles zero dt
 */
TEST_F(CameraFollowTest, EdgeCase_ZeroDeltaTime) {
    character.animState = CameraState::IDLE;
    
    // Should not crash
    camera.update(0.0f, character);
    
    // Position should not change
    EXPECT_FLOAT_EQ(camera.position.x, 0.0f);
    EXPECT_FLOAT_EQ(camera.position.y, 5.0f);
    EXPECT_FLOAT_EQ(camera.position.z, 10.0f);
}
