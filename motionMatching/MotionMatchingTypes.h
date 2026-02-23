#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <cstdint>

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
    
    // Foot planting configuration
    float footPlantThreshold = 0.05f;   // Velocity below which foot is "planted"
    float footPlantHeightThreshold = 0.1f; // Height variance for planting
    bool enableFootLocking = true;      // Lock feet when planted (IK)
    
    // Trajectory prediction
    float trajectoryDuration = 0.5f;    // How far into future to predict (seconds)
    int trajectoryPoints = 5;           // How many trajectory points
    
    // Performance
    bool useSpatialIndex = true;        // Use KD-tree for faster search
    int spatialIndexRebuildFrames = 60; // Rebuild index every N frames
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
