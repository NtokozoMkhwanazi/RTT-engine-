#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <cstdint>

// ============================================================================
// TRAJECTORY FEATURE CONSTANTS (Unreal-style root-relative future path)
// ============================================================================
//
// Unreal's pose search doesn't just match current speed - it matches WHERE the
// character is going. Each pose stores its own future root path expressed in
// the root's LOCAL space (offsets from the root, rotated by the root yaw). The
// query builds the same local-space path from the predicted trajectory. Two
// poses at the same speed are then ranked by how well their future path
// matches the player's turn/stop - this is what makes clip selection look
// intentional instead of speed-only.
// ============================================================================

constexpr int kTrajectorySteps = 4;         // Future points per pose/query
constexpr float kTrajectoryStepTime = 0.1f; // Seconds between points (0.1..0.4s)

// ============================================================================
// MOTION MATCHING - CORE TYPES
// ============================================================================
// 
// Motion Matching replaces discrete state machines with continuous
// pose searching. Each frame we:
// 1. Query current character motion (speed, direction, etc.)
// 2. Search database for best matching pose
// 3. Blend to that pose
// 4. Track foot plants to prevent sliding
// ============================================================================

/**
 * Motion Features - What we match on
 * 
 * These features describe the character's motion state.
 * We search for poses with similar features.
 */
struct MotionFeatures {
    // Primary locomotion features
    glm::vec3 rootVelocity{0.0f};      // Root bone velocity (world space)
    float speed{0.0f};                  // Movement speed (magnitude of velocity)
    glm::vec2 moveDirection{0.0f, 1.0f}; // Direction character is moving (0-1 normalized)
    
    // Body orientation
    float facingAngle{0.0f};            // Angle root is facing (radians)
    float moveAngle{0.0f};              // Angle between facing and movement
    
    // State flags
    bool isGrounded{true};              // Is character on ground
    bool isCrouching{false};            // Is character crouching
    bool isAirborne{false};             // Is character in air
    
    // Foot state (for foot planting)
    bool leftFootPlanted{false};        // Is left foot planted
    bool rightFootPlanted{false};       // Is right foot planted
    glm::vec3 leftFootPos{0.0f};        // Left foot position (world space)
    glm::vec3 rightFootPos{0.0f};       // Right foot position (world space)
    glm::vec3 leftFootVel{0.0f};        // Left foot velocity
    glm::vec3 rightFootVel{0.0f};       // Right foot velocity

    // Root-relative future trajectory (Unreal-style pose feature). Matches the
    // per-pose Trajectory.localPositions; the query fills these from its
    // predicted future path so the KD-tree can rank by "where am I going".
    glm::vec2 futureLocal[kTrajectorySteps]{glm::vec2(0.0f)};
    int futureCount{0};                 // Valid points (0 = feature disabled)
    
    // Animation metadata
    float animationTime{0.0f};          // Current time in animation
    float animationDuration{0.0f};      // Total animation duration
    int animationIndex{-1};             // Which animation in database
    int frameIndex{-1};                 // Which frame in animation
    
    /**
     * Calculate feature difference (for scoring)
     * Lower score = better match
     */
    float getDifference(const MotionFeatures& other) const;
};

/**
 * Trajectory - Path character will follow
 * 
 * We predict where the character WILL be in the future
 * and match poses that follow that trajectory.
 */
struct Trajectory {
    static constexpr int MAX_POINTS = 8;
    
    glm::vec3 positions[MAX_POINTS];    // Future positions (every 0.1s)
    glm::vec3 velocities[MAX_POINTS];   // Future velocities
    float directions[MAX_POINTS];       // Future movement angles
    int numPoints{0};                   // How many points are valid
    
    // Root-relative future path (Unreal-style feature). localPositions[k] is
    // the root position k*0.1s in the future, expressed as an offset from the
    // current root position rotated into the root's local space (XZ only, y=0).
    glm::vec3 localPositions[MAX_POINTS]{glm::vec3(0.0f)};
    int localNumPoints{0};              // How many local points are valid
    
    /**
     * Get trajectory point at time offset
     * @param timeOffset Time in future (0.1, 0.2, etc.)
     */
    glm::vec3 getPositionAt(float timeOffset) const;
    glm::vec3 getVelocityAt(float timeOffset) const;
    
    /**
     * Calculate trajectory difference
     */
    float getDifference(const Trajectory& other) const;
};

/**
 * Pose Sample - One frame of animation with features
 * 
 * This is what we store in the database.
 * Each pose knows its own motion features.
 */
struct PoseSample {
    // Feature data
    MotionFeatures features;
    Trajectory trajectory;
    
    // Animation reference
    int animationIndex;                 // Which animation
    float timeInSeconds;                // Time in animation
    int frameNumber;                    // Frame number (for debugging)
    
    // Bone transforms (compressed)
    // For now, we just store the time - actual pose comes from Animation
    glm::vec3 rootPosition;             // Root position at this frame
    float rootRotationY;                // Root Y rotation (yaw)
    
    // Foot plant data
    float leftFootHeight;               // Left foot Y position
    float rightFootHeight;              // Right foot Y position
    bool leftFootPlanted;               // Is left foot planted this frame
    bool rightFootPlanted;              // Is right foot planted this frame
    
    // Links to next/previous frames (for blending)
    int nextFrameIndex{-1};             // Next frame in same animation
    int prevFrameIndex{-1};             // Previous frame
};

/**
 * Search Result - Best matching pose
 */
struct SearchResult {
    int poseIndex{-1};                  // Index in database
    float score{0.0f};                  // Match score (lower = better)
    float blendWeight{0.0f};            // How much to blend (0-1)
    
    bool isValid() const { return poseIndex >= 0; }
};

/**
 * Search Results - Multiple candidates for blending
 */
struct SearchResults {
    SearchResult best;                  // Best match
    SearchResult second;                // Second best (for blending)
    int totalSearched{0};               // How many poses we searched
    float searchTimeMs{0.0f};           // How long search took
};

// ============================================================================
// CONFIGURATION
// ============================================================================

struct MotionMatchingConfig {
    // Search configuration
    int maxSearchResults = 10;          // How many candidates to consider
    float searchRadius = 2.0f;          // How far to search in feature space
    bool useTrajectoryMatching = true;  // Match future trajectory
    
    // Blending configuration
    float blendDuration = 0.1f;         // How long to blend between poses (seconds)
    int numBlendPoses = 2;              // How many poses to blend (2 = current + target)

    // State-boundary blends: an airborne <-> grounded switch (takeoff/landing)
    // spans a much larger pose delta than a normal locomotion switch (tucked
    // Fall pose -> full-stance Walk), so the generic 0.1s crossfade pops.
    float landingBlendDuration = 0.3f;  // air -> ground (landing)
    float takeoffBlendDuration = 0.2f;  // ground -> air (takeoff)
    
    // Foot planting configuration
    float footPlantThreshold = 0.05f;   // Velocity below which foot is "planted"
    float footPlantHeightThreshold = 0.1f; // Height variance for planting
    bool enableFootLocking = true;      // Lock feet when planted (IK)

    // Velocity-based lock dissolution parameters (Fix: "Lock Inversion Strangle")
    float footLiftVelocityThreshold = 0.5f;  // Speed above which a planted foot
                                              // must release (forced lift detection).
                                              // Fixes the ghost-lock where feet
                                              // stay pinned while the body strides
                                              // forward, elastically stretching
                                              // the leg past its max reach.
    float footPlantedHeightThreshold = 0.1f; // Max distance below the floor a
                                              // foot may be and still be
                                              // considered "close to ground"
                                              // for (un)locking.
    float footPlantVelocityThreshold = 0.05f; // Speed below which a free foot
                                               // is allowed to (re)plant.
    float footLockReleaseDuration = 0.15f;   // Seconds over which lockWeight
                                              // ramps 1→0 on release, so the
                                              // ankle offset fades instead of
                                              // snapping and the mesh doesn't
                                              // pop when detaching from a
                                              // planted anchor.
    
    // Trajectory prediction
    float trajectoryDuration = 0.5f;    // How far into future to predict (seconds)
    int trajectoryPoints = 5;           // How many trajectory points
    
    // Performance
    bool useSpatialIndex = true;        // Use KD-tree for faster search
    int spatialIndexRebuildFrames = 60; // Rebuild index every N frames

    // Clip-switch persistence bands (see MotionMatcher::SelectPoseWithPersistence).
    // The matcher keeps the current clip until another clip beats it by the
    // 15% distance margin - but drops the margin and switches at once when the
    // query leaves the current clip's nominal band:
    //  - speedBandFactor: band = clip nominal speed * this (with a 1 m/s floor
    //    so a stationary Idle never trips it). Catches gait changes
    //    (run<->walk, walk<->stop, ...) that the margin would otherwise delay.
    //  - directionBandRadians: angular band on the movement direction. When the
    //    query heads more than this far from the clip's nominal move direction
    //    (reversal / backpedal / running away from the clip's heading), switch
    //    immediately instead of waiting for the margin. ~pi/2 (90 deg) keeps
    //    straight-line walking / mild turns from tripping it.
    float speedBandFactor = 0.5f;
    float directionBandRadians = 1.5707963267948966f;  // pi/2 = 90 degrees

    // Advanced - directional flip penalty (feet/footskate guard, the "Metric
    // Space Collapsing" fix). A bestOther candidate whose clip-frame root
    // velocity is geometrically OPPPOSED to the query (cos angle below
    // directionFlipDotThreshold, i.e. more than ~90deg apart) matched only on
    // scalar speed - e.g. a backward RunLookBack clip on a forward-left W+A
    // strafe, which plants the feet on the wrong animation. That candidate's
    // distance is scaled by directionalFlipMultiplier before the 0.85
    // persistence comparison so a geometrically-flipped clip can't hijack
    // selection. Only the OTHER clip's best is penalized; the current clip is
    // never touched, so genuine reversals (Back aligned with a backpedal query)
    // still switch. Idle/zero-velocity poses are exempt. Set to 1.0 to disable
    // (original behavior).
    float directionalFlipMultiplier = 2.0f;
    float directionFlipDotThreshold = 0.0f;  // cos: <0 => >90deg apart

    // Pose-level persistence margin (companion to the 0.85 clip margin above).
    // bestSameIdx is the min-distance pose in the current clip; on a
    // constant-velocity clip (walk/idle) that minimum is shared by MANY
    // near-equidistant frames, so it flips every frame and jitters the matched
    // clip time ~+-half a frame. The foot IK then chases the shifting root and
    // the ankles stretch/release each flicker - visible "feet vibrate / stretch
    // on idle" and "walk vibrate". Hold the current pose unless a same-clip
    // candidate beats it by this fraction (1.0 = never flip pose while the clip
    // is held). Idle/zero-velocity clips are unaffected (no near-equidistant
    // frames to flip). Set to 1.0f to disable.
    float poseHoldMargin = 0.95f;
};

// ============================================================================
// DEBUG VISUALIZATION
// ============================================================================

struct MotionMatchingDebug {
    bool enabled{false};
    
    // Current search info
    int posesSearched{0};
    float searchTimeMs{0.0f};
    SearchResult currentResult;
    
    // Trajectory visualization
    Trajectory predictedTrajectory;
    Trajectory matchedTrajectory;
    
    // Foot plant state
    bool leftFootPlanted{false};
    bool rightFootPlanted{false};
    glm::vec3 leftFootTarget{0.0f};
    glm::vec3 rightFootTarget{0.0f};
    
    // Current pose info
    int currentAnimationIndex{-1};
    float currentAnimationTime{0.0f};
    std::string currentAnimationName;
};
