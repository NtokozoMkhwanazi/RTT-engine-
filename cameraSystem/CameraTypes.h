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
/**
 * ScreenDeadZone (forCam.txt §1 "Pillar 1: Screen-Space Framing Dead Zone").
 * Inner screen-space anchor box (normalized viewport [0..1], center 15% slack
 * envelope). The camera pivot (look-at) stays put while the tracked anchor
 * projects inside this box, and only resumes chasing the anchor once it
 * breaches a boundary -- eliminating micro-jitter and mechanical edge pops
 * during idle/small-motion tracking. Optional: off by default.
 */
struct ScreenDeadZone {
    float xMin = 0.425f;
    float xMax = 0.575f;
    float yMin = 0.425f;
    float yMax = 0.575f;
    float catchUpSpeed = 8.0f;   // 1/s tracking rate once the anchor breaches
};

struct CameraConfig {
    // Base configuration (Unreal-style third-person tuning for a ~1.8m character)
    float distance = 4.0f;          // Camera distance from character
    float height = 1.6f;            // Camera height offset
    float fov = 60.0f;              // Resting / idle field of view (degrees)

    // Dynamic FOV Expansion (forcam.txt §3 "Lack of Lens Profile FOV Expansion"):
    // the lens profile widens as ground speed rises from idle to sprint,
    // amplifying the sensation of speed and opening peripheral vision to reveal
    // upcoming terrain. `fov` is the resting FOV; fovMax is the fully-open FOV
    // reached at fovMaxSpeed (expansion begins at fovSpeedThreshold).
    float fovMax = 72.0f;            // FOV at/above fovMaxSpeed (degrees)
    float fovSpeedThreshold = 2.0f;  // speed (m/s) where expansion begins
    float fovMaxSpeed = 6.0f;        // speed (m/s) at/above which FOV is fully open
    float fovExpandRate = 8.0f;      // lens-open blend rate (1/s)
    float fovContractRate = 12.0f;  // lens-close blend rate (1/s) - crisp settle

    // Pivot configuration
    float pivotHeight = 1.3f;       // Look-at height (character chest)
    float pivotSmooth = 8.0f;       // Pivot follow smoothing
    
    // State-aware follow smoothing
    // Higher = snappier, Lower = smoother/more lag
    float idleFollowSmooth = 10.0f;   // Snappy when idle (precise control)
    float walkFollowSmooth = 7.0f;    // Balanced when walking
    float runFollowSmooth = 12.0f;    // Responsive when running
    float jumpFollowSmooth = 8.0f;    // Balanced for jump
    float fallFollowSmooth = 9.0f;    // Smooth for landing
    float crouchFollowSmooth = 6.0f;  // Smooth when crouching

    // forcam.txt §3 position-lag coupling: as the lens widens under speed the
    // position follow rate is scaled toward zero (camera trails more) to amplify
    // the sensation of motion. 0 = disabled.
    float positionLagScale = 0.5f;
    
    // Transition configuration
    float smoothTransitionRate = 5.0f;  // How fast to transition between smooth values
    
    // Limits
    float minDistance = 2.0f;
    float maxDistance = 18.0f;
    float minHeight = -5.0f;   // vertical clamp floor (keep the cam above big props / pits)
    float maxHeight = 40.0f;   // vertical clamp ceiling (keep the cam below the skybox)
    float minPitch = -30.0f;   // keep the camera above the floor
    float maxPitch = 55.0f;
    
    // Ground clearance: camera is never pushed below terrain+this margin
    float groundClearance = 0.4f;
    
    // Collision avoidance
    bool collisionEnabled = true;
    float collisionRadius = 0.5f;
    float collisionLerp = 0.1f;       // legacy fixed-rate snap (superseded by spring)
    float collisionBuffer = 0.15f;   // safe-distance buffer inside collisionRadius
    float collisionSpringRate = 12.0f; // critical-damping spring rate (1/s)

    // Auto-orient the camera behind the character's forward direction
    // (Unreal-style). When enabled, yaw eases toward the heading given by
    // CameraInput.characterForward each frame, so if the character turns
    // around or walks toward the camera it orbits around to the character's
    // back instead of ending up in front of it.
    bool orientToCharacterForward = false;
    float orientSmoothRate = 6.0f;   // Yaw swing speed when auto-orienting

    // Velocity-biased look-ahead (forCam.txt §4 "velocity-biased tracking"): the
    // pivot target is nudged forward along the character's velocity so the
    // camera leads the bot by `positionLookAhead` seconds of travel, revealing
    // the landscape ahead instead of framing the exact hip position. A 0.3 s
    // lead is ~0.6 m at walk speed and ~1.8 m at a 6 m/s run — unnoticeable dead
    // zone bias that keeps the bot's path clear of the frame edge.
    float positionLookAhead = 0.3f;

    // forCam.txt §3A-B: seconds for the one-shot "FocusOnEntity" EaseInOut
    // glide that frames a newly-selected entity/prop on a viewport click or
    // outliner row (instead of instantly re-framing through the follow loop).
    float focusGlideDuration = 0.4f;

    // forCam.txt §4 Camera Banking: tilt the viewport on sharp turns (the
    // "turning-rate dependent banking" polish the engine lacked). Roll (deg) =
    // turn-rate (deg/s) * bankScale, clamped to [-maxBankRoll, +maxBankRoll],
    // eased with a critical-damping filter so straight-line tracking stays
    // level (roll -> 0). Zero values keep existing behaviour / tests identical.
    float bankScale   = 0.5f;    // roll (deg) per deg/s of turn rate
    float maxBankRoll = 10.0f;   // hard clamp (deg)
    float bankDamping = 12.0f;   // eased roll rate (1/s)

    // forCam.txt §1 Pillar 1: optional screen-space dead-zone tracking. When
    // enableScreenDeadZone is true, the pivot (look-at) is driven by
    // updateScreenSpaceSlackPivot() instead of updatePivot(): the camera holds
    // its frame while the anchor stays inside the inner screen box and only
    // resumes the chase once the anchor breaches a boundary. OFF by default so
    // existing follow behaviour/tests are byte-identical.
    bool enableScreenDeadZone = false;
    ScreenDeadZone screenDeadZone{};

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
        idleFollowSmooth = 14.0f;
        walkFollowSmooth = 10.0f;
        runFollowSmooth = 16.0f;
        pivotSmooth = 10.0f;
    }
    
    /**
     * Configure for cinematic smooth (story games)
     */
    void setCinematic() {
        idleFollowSmooth = 4.0f;
        walkFollowSmooth = 3.0f;
        runFollowSmooth = 5.0f;
        pivotSmooth = 3.0f;
    }
    
    /**
     * Configure for balanced (default)
     */
    void setBalanced() {
        idleFollowSmooth = 10.0f;
        walkFollowSmooth = 7.0f;
        runFollowSmooth = 12.0f;
        pivotSmooth = 8.0f;
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
    
    // ---- Selection-driven focus (engine milestone) ----
    // The follow camera tracks WHATEVER entity is currently in focus
    // (selected entity, or the playable character as a fallback), not just the
    // playable character. `characterPosition` is reused as the follow target so
    // existing callers/tests are unaffected — they keep feeding the character
    // and get identical behavior. Selection-driven callers instead feed the
    // selected entity's position/forward.
    //
    // targetIsAnimated = true  -> an animated/playable entity: the camera
    //                             orbit-behinds its forward (auto-orient on).
    // targetIsAnimated = false -> a static prop (e.g. a tree): the camera still
    //                             glides to frame it, but does NOT auto-rotate
    //                             behind a (non-existent) forward — it just
    //                             locks yaw and re-frames, so static scenery
    //                             is always kept in view regardless of heading.
    bool targetIsAnimated = false;
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
    float roll;          // forCam.txt §4 camera banking (roll on turn), 0 when steady
    CameraState currentState;
    float currentFollowSmooth;
    bool isColliding;
};
