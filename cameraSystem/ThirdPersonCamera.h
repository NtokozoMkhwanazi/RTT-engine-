#pragma once
#include "CameraTypes.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <algorithm>
#include <cmath>
#include <functional>
#include "CinematicCamera.h"

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
        yaw(180.0f), // Z-forward: behind the -Z-forward character, looks down -Z (U6)
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

        // Always keep the camera BEHIND the character: when enabled, ease yaw
        // toward the character's heading so the camera orbits around to its
        // back the moment it turns. Without this the camera stays fixed in
        // world space and ends up in front of the character when it walks
        // toward or past the camera.
        if (config.orientToCharacterForward) {
            const glm::vec2 fwd2(input.characterForward.x, input.characterForward.z);
            if (glm::length(fwd2) > 0.001f) {
                const float targetYaw = glm::degrees(std::atan2(fwd2.x, fwd2.y));
                float dy = targetYaw - yaw;
                while (dy > 180.0f) dy -= 360.0f;
                while (dy < -180.0f) dy += 360.0f;
                yaw += dy * std::min(1.0f, 1.0f - std::exp(-config.orientSmoothRate * dt));
            }
        }

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

        // Z-forward convention (U6): yaw=0 looks down +Z. This matches the
        // yaw/atan2 convention used by the editor free-fly, the orbit camera and
        // the cinematic orbit math, so a single yaw value means the same heading
        // everywhere. dir.x = cos(pitch)*sin(yaw), dir.z = cos(pitch)*cos(yaw).
        glm::vec3 direction;
        direction.x = std::cos(pitchRad) * std::sin(yawRad);
        direction.y = -std::sin(pitchRad);   // +pitch raises the camera above
        direction.z = std::cos(pitchRad) * std::cos(yawRad);
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
        
        if (distance < 1e-6f) return idealPos;  // degenerate: camera at character
        
        glm::vec3 dirNorm = direction / distance;
        
        if (distance < config.collisionRadius) {
            // The ideal position would place the camera inside the collision
            // boundary — pull it to the safe radius. Pull slightly closer than
            // the wall face so the camera never clips through geometry.
            isColliding = true;
            float safetyBuffer = config.collisionRadius * 0.15f;
            float newDistance = config.collisionRadius - safetyBuffer;
            idealDistance = newDistance;
            glm::vec3 newPos = charPos + dirNorm * newDistance;
            
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
    // Cinematic timeline tracking (unknown U7 drop-in): an explicit clock
    // so the perpetual-orbit math advances smoothly over time instead of
    // freezing at its entry angle.
    float cinematicTime{0.0f};
    double cinematicOrbitAngle{0.0f};
    float cinematicRotationSpeed{15.0f}; // deg/s (fixed: was {15.0f;} syntax error)

    
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
            cinematicTime = 0.0f;       // Reset timeline on state entry
            cinematicOrbitAngle = 0.0;   // Restart orbit at 0 degrees
                configureCinematic();
                break;
            default:
                configureThirdPerson();
        }
    }
    
    /**
     * Cinematic tick driver (unknown U7 drop-in): advances the explicit
     * cinematic clock and repositions the camera on its perpetual orbit each
     * frame. Engaged only in CINEMATIC mode; a safe no-op in other modes.
     */
    void update(float dt, const CameraInput& input) {
        if (!camera) return;

        if (currentMode == CameraMode::CINEMATIC) {
            cinematicTime += dt;

            // Integrate with the Cinematic namespace math (CinematicCamera.h)
            cinematicOrbitAngle = Cinematic::AdvanceAngle(
                cinematicOrbitAngle, cinematicRotationSpeed, dt);

            // Height profile: ease down from (pivot+2) over a 3s intro.
            float liveHeight = Cinematic::IntroHeight(
                cinematicTime, 3.0f,
                camera->config.height + 2.0f, camera->config.height);

            glm::vec3 lookAtTarget = input.characterPosition
                + glm::vec3(0.0f, camera->config.pivotHeight, 0.0f);
            glm::vec3 nextOrbitPos = lookAtTarget
                + Cinematic::OrbitPosition(cinematicOrbitAngle,
                                          camera->config.distance, liveHeight);

            camera->setPosition(nextOrbitPos);
            camera->setTarget(lookAtTarget);
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
        camera->yaw = 180.0f;     // Z-forward behind-camera default heading (U6)
        camera->pitch = 0.0f;
    }
};
