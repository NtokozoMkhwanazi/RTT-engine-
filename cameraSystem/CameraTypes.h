#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <string>

/**
 * Camera State - mirrors animation states for state-aware follow
 */
enum class CameraState {
    IDLE,
    WALK,
    RUN,
    JUMP,
    FALL,
    CROUCH,
    TRANSITIONING,
    CUSTOM
};

inline std::string CameraStateToString(CameraState state) {
    switch (state) {
        case CameraState::IDLE: return "Idle";
        case CameraState::WALK: return "Walk";
        case CameraState::RUN: return "Run";
        case CameraState::JUMP: return "Jump";
        case CameraState::FALL: return "Fall";
        case CameraState::CROUCH: return "Crouch";
        case CameraState::TRANSITIONING: return "Transitioning";
        default: return "Custom";
    }
}

/**
 * Camera Configuration - State-aware smoothing values
 * 
 * Design Principle: Different animation states need different camera behaviors
 * - Idle: Snappy for precise control
 * - Walk: Smooth but responsive
 * - Run: Very smooth to reduce motion blur
 * - Jump/Fall: Balanced for vertical movement
 */
struct CameraConfig {
    // Base configuration
    float distance = 15.0f;         // Camera distance from character
    float height = 5.0f;            // Camera height offset
    float fov = 45.0f;              // Field of view
    
    // Pivot configuration
    float pivotHeight = 2.0f;       // Look-at height (character upper body)
    float pivotSmooth = 4.0f;       // Pivot follow smoothing
    
    // State-aware follow smoothing
    // Higher = snappier, Lower = smoother/more lag
    float idleFollowSmooth = 8.0f;    // Snappy when idle (precise control)
    float walkFollowSmooth = 5.0f;    // Balanced when walking
    float runFollowSmooth = 10.0f;    // Very smooth when running (catches up)
    float jumpFollowSmooth = 6.0f;    // Balanced for jump
    float fallFollowSmooth = 7.0f;    // Smooth for landing
    float crouchFollowSmooth = 4.0f;  // Smooth when crouching
    
    // Transition configuration
    float smoothTransitionRate = 5.0f;  // How fast to transition between smooth values
    
    // Limits
    float minDistance = 5.0f;
    float maxDistance = 30.0f;
    float minPitch = -89.0f;
    float maxPitch = 89.0f;
    
    // Collision avoidance
    bool collisionEnabled = true;
    float collisionRadius = 0.5f;
    float collisionLerp = 0.1f;
    
    /**
     * Get follow smoothing for a given state
     */
    float getFollowSmoothForState(CameraState state) const {
        switch (state) {
            case CameraState::IDLE: return idleFollowSmooth;
            case CameraState::WALK: return walkFollowSmooth;
            case CameraState::RUN: return runFollowSmooth;
            case CameraState::JUMP: return jumpFollowSmooth;
            case CameraState::FALL: return fallFollowSmooth;
            case CameraState::CROUCH: return crouchFollowSmooth;
            default: return 5.0f;
        }
    }
    
    /**
     * Configure for snappy response (action games)
     */
    void setSnappy() {
        idleFollowSmooth = 12.0f;
        walkFollowSmooth = 8.0f;
        runFollowSmooth = 15.0f;
        pivotSmooth = 6.0f;
    }
    
    /**
     * Configure for cinematic smooth (story games)
     */
    void setCinematic() {
        idleFollowSmooth = 4.0f;
        walkFollowSmooth = 3.0f;
        runFollowSmooth = 6.0f;
        pivotSmooth = 2.0f;
    }
    
    /**
     * Configure for balanced (default)
     */
    void setBalanced() {
        idleFollowSmooth = 8.0f;
        walkFollowSmooth = 5.0f;
        runFollowSmooth = 10.0f;
        pivotSmooth = 4.0f;
    }
};

/**
 * Camera Input - character state for camera to follow
 */
struct CameraInput {
    glm::vec3 characterPosition{0.0f};
    glm::vec3 characterVelocity{0.0f};
    float moveMagnitude = 0.0f;
    bool isGrounded = true;
    CameraState animState = CameraState::IDLE;
    
    // Optional: character forward direction
    glm::vec3 characterForward{0.0f, 0.0f, 1.0f};
};

/**
 * Camera Output - result of camera update
 */
struct CameraOutput {
    glm::vec3 position;
    glm::vec3 target;
    glm::mat4 viewMatrix;
    glm::mat4 projectionMatrix;
    float currentFOV;
    CameraState currentState;
    float currentFollowSmooth;
    bool isColliding;
};
