#pragma once
#include "CameraTypes.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <algorithm>
#include <cmath>
#include <functional>

/**
 * Third-Person Camera - State-Aware Follow System
 * 
 * Features:
 * - State-aware smoothing (different behavior for idle/walk/run/jump/fall)
 * - Smooth transitions between camera states
 * - Collision avoidance
 * - Configurable follow behavior
 * 
 * Design Principles:
 * - Modularity: Independent system, easy to test
 * - Scalability: Easy to add new states/behaviors
 * - Performance: Minimal allocations, efficient updates
 */
class ThirdPersonCamera {
public:
    // Public state for debugging/inspection
    CameraConfig config;
    CameraOutput output;
    
    // Current camera transform
    glm::vec3 position;
    glm::vec3 target;
    glm::vec3 pivot;  // Look-at point
    
    // State tracking
    CameraState currentState;
    CameraState previousState;
    
    // Smooth follow value (transitions between state values)
    float currentFollowSmooth;
    float targetFollowSmooth;
    
    // Orbit controls (yaw/pitch)
    float yaw;
    float pitch;
    
    // Collision
    bool isColliding;
    float idealDistance;  // Distance before collision

    // Optional terrain-height callback. When set, the camera is clamped so it
    // never sinks below the floor (Unreal-style ground clamp).
    std::function<float(float, float)> groundHeightFn;
    
    /**
     * Constructor
     */
    ThirdPersonCamera(
        const glm::vec3& startPos = glm::vec3(0.0f, 5.0f, 10.0f),
        const glm::vec3& targetPos = glm::vec3(0.0f, 2.0f, 0.0f),
        const CameraConfig& cfg = CameraConfig()
    ) : config(cfg),
        position(startPos),
        target(targetPos),
        pivot(targetPos),
        currentState(CameraState::IDLE),
        previousState(CameraState::IDLE),
        currentFollowSmooth(cfg.idleFollowSmooth),
        targetFollowSmooth(cfg.idleFollowSmooth),
        yaw(-90.0f),
        pitch(0.0f),
        isColliding(false),
        idealDistance(cfg.distance)
    {
        output.position = position;
        output.target = target;
        output.currentState = currentState;
        output.currentFollowSmooth = currentFollowSmooth;
        output.currentFOV = config.fov;
        output.isColliding = false;
    }
    
    /**
     * Update camera with state-aware follow
     * 
     * @param dt Delta time
     * @param input Character state input
     * @param aspectRatio Aspect ratio for projection matrix
     */
    void update(float dt, const CameraInput& input, float aspectRatio = 16.0f/9.0f) {
        if (dt <= 0.0f) { updateOutput(aspectRatio); return; }

        previousState = currentState;
        currentState = input.animState;

        // Update target follow smooth based on state
        targetFollowSmooth = config.getFollowSmoothForState(currentState);

        // Smoothly transition follow smooth value (frame-rate independent)
        currentFollowSmooth = glm::mix(
            currentFollowSmooth,
            targetFollowSmooth,
            1.0f - std::exp(-config.smoothTransitionRate * dt)
        );

        // Update pivot (look-at point)
        updatePivot(dt, input.characterPosition);

        // Calculate ideal camera position
        glm::vec3 idealPosition = calculateIdealPosition(input);

        // Handle collision avoidance
        if (config.collisionEnabled) {
            idealPosition = handleCollision(input.characterPosition, idealPosition);
        }

        // Unreal-style ground clamp: never let the camera sink below the floor.
        if (groundHeightFn) {
            const float minY = groundHeightFn(idealPosition.x, idealPosition.z) + config.groundClearance;
            if (idealPosition.y < minY) idealPosition.y = minY;
        }

        // Smooth camera follow (state-aware, frame-rate independent exponential).
        const float alpha = 1.0f - std::exp(-currentFollowSmooth * dt);
        position = glm::mix(position, idealPosition, alpha);
        target = pivot;

        // Update output
        updateOutput(aspectRatio);
    }
    
    /**
     * Get view matrix
     */
    glm::mat4 getViewMatrix() const {
        return glm::lookAt(position, target, glm::vec3(0.0f, 1.0f, 0.0f));
    }
    
    /**
     * Get projection matrix
     */
    glm::mat4 getProjectionMatrix(float aspectRatio) const {
        return glm::perspective(
            glm::radians(config.fov), 
            aspectRatio, 
            0.1f, 
            1000.0f
        );
    }
    
    /**
     * Set camera state directly (for manual control)
     */
    void setState(CameraState state) {
        previousState = currentState;
        currentState = state;
        targetFollowSmooth = config.getFollowSmoothForState(state);
    }
    
    /**
     * Set configuration
     */
    void setConfig(const CameraConfig& cfg) {
        config = cfg;
        targetFollowSmooth = config.getFollowSmoothForState(currentState);
    }
    
    /**
     * Instant camera placement (for cutscenes, etc.)
     */
    void setPosition(const glm::vec3& pos) {
        position = pos;
    }
    
    void setTarget(const glm::vec3& tgt) {
        target = tgt;
        pivot = tgt;
    }
    
    /**
     * Get camera forward direction
     */
    glm::vec3 getForward() const {
        return glm::normalize(target - position);
    }
    
    /**
     * Get distance to character
     */
    float getDistanceToCharacter(const glm::vec3& charPos) const {
        return glm::length(position - charPos);
    }
    
    /**
     * Check if camera is in transition
     */
    bool isTransitioning() const {
        return currentState != previousState;
    }
    
    /**
     * Get current state as string (for debugging)
     */
    std::string getStateString() const {
        return CameraStateToString(currentState);
    }
    
private:
    /**
     * Update pivot point (look-at target)
     */
    void updatePivot(float dt, const glm::vec3& charPos) {
        glm::vec3 targetPivot = charPos + glm::vec3(0.0f, config.pivotHeight, 0.0f);
        const float alpha = 1.0f - std::exp(-config.pivotSmooth * dt);
        pivot = glm::mix(pivot, targetPivot, alpha);
    }
    
    /**
     * Calculate ideal camera position (without collision)
     */
    glm::vec3 calculateIdealPosition(const CameraInput& input) {
        // Use yaw and pitch for orbit control. Positive pitch raises the camera
        // ABOVE the character's chest and looks down at it (Unreal-style).
        float yawRad = glm::radians(yaw);
        float pitchRad = glm::radians(pitch);

        // Calculate direction from yaw and pitch (pitch sign: +pitch = above)
        glm::vec3 direction;
        direction.x = std::cos(yawRad) * std::cos(pitchRad);
        direction.y = -std::sin(pitchRad);   // NEGATED: +pitch now = camera above
        direction.z = std::sin(yawRad) * std::cos(pitchRad);
        direction = glm::normalize(direction);

        // Camera orbits the character's chest at `distance`, slightly raised.
        glm::vec3 pivotPos = input.characterPosition
                           + glm::vec3(0.0f, config.pivotHeight, 0.0f);
        glm::vec3 idealPos = pivotPos - direction * config.distance;
        idealPos.y += config.height * 0.35f;  // shoulder-level camera height

        return idealPos;
    }
    
    /**
     * Handle collision avoidance
     */
    glm::vec3 handleCollision(const glm::vec3& charPos, const glm::vec3& idealPos) {
        glm::vec3 direction = idealPos - charPos;
        float distance = glm::length(direction);
        
        if (distance < config.collisionRadius) {
            isColliding = true;
            idealDistance = distance;
            
            // Move camera closer to avoid clipping
            float newDistance = config.collisionRadius;
            glm::vec3 newPos = charPos + glm::normalize(direction) * newDistance;
            
            // Smooth collision transition
            return glm::mix(position, newPos, config.collisionLerp);
        }
        
        isColliding = false;
        idealDistance = config.distance;
        return idealPos;
    }
    
    /**
     * Update output structure
     */
    void updateOutput(float aspectRatio) {
        output.position = position;
        output.target = target;
        output.viewMatrix = getViewMatrix();
        output.projectionMatrix = getProjectionMatrix(aspectRatio);
        output.currentFOV = config.fov;
        output.currentState = currentState;
        output.currentFollowSmooth = currentFollowSmooth;
        output.isColliding = isColliding;
    }
};

/**
 * Camera Controller - High-level camera management
 * 
 * Manages camera modes, transitions, and presets
 */
class CameraController {
public:
    ThirdPersonCamera* camera;
    
    enum class CameraMode {
        THIRD_PERSON,     // Default state-aware follow
        FIRST_PERSON,     // Over-the-shoulder
        ORBIT,           // Manual orbit control
        CINEMATIC        // Fixed cinematic angles
    };
    
    CameraMode currentMode;
    
    CameraController(ThirdPersonCamera* cam) 
        : camera(cam), currentMode(CameraMode::THIRD_PERSON) {}
    
    /**
     * Set camera mode
     */
    void setMode(CameraMode mode) {
        currentMode = mode;
        
        switch (mode) {
            case CameraMode::FIRST_PERSON:
                configureFirstPerson();
                break;
            case CameraMode::ORBIT:
                configureOrbit();
                break;
            case CameraMode::CINEMATIC:
                configureCinematic();
                break;
            default:
                configureThirdPerson();
        }
    }
    
    /**
     * Configure for third-person (default)
     */
    void configureThirdPerson() {
        camera->config.setBalanced();
        camera->config.distance = 4.0f;
        camera->config.height = 1.6f;
        camera->config.pivotHeight = 1.3f;
    }
    
    /**
     * Configure for first-person (over-shoulder)
     */
    void configureFirstPerson() {
        camera->config.setSnappy();
        camera->config.distance = 2.0f;
        camera->config.height = 1.5f;
        camera->config.pivotHeight = 1.6f;
    }
    
    /**
     * Configure for orbit mode
     */
    void configureOrbit() {
        camera->config.distance = 5.5f;
        camera->config.height = 2.0f;
        camera->config.pivotHeight = 1.3f;
        camera->config.pivotSmooth = 6.0f;
    }
    
    /**
     * Configure for cinematic
     */
    void configureCinematic() {
        camera->config.setCinematic();
        camera->config.distance = 7.0f;
        camera->config.height = 2.5f;
        camera->config.pivotHeight = 1.3f;
    }
    
    /**
     * Quick zoom
     */
    void zoom(float delta) {
        camera->config.distance = glm::clamp(
            camera->config.distance + delta,
            camera->config.minDistance,
            camera->config.maxDistance
        );
    }
    
    /**
     * Reset camera to default
     */
    void reset() {
        setMode(CameraMode::THIRD_PERSON);
        camera->yaw = -90.0f;
        camera->pitch = 0.0f;
    }
};
