#pragma once
#include "MotionMatchingTypes.h"
#include "../animationSystem/Animation.h"
#include "../boneSystem/Skeleton.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>

// ============================================================================
// MOTION DATABASE
// ============================================================================
//
// Stores all animation frames as searchable pose samples.
// Each frame knows its motion features (speed, direction, foot state, etc.)
//
// When we search, we find frames with similar features to current state.
// ============================================================================

class MotionDatabase {
public:
    MotionDatabase();
    ~MotionDatabase();

    // =========================================================================
    // DATABASE CONSTRUCTION
    // =========================================================================

    /**
     * Add animation to database
     *
     * This extracts ALL frames from the animation and stores them
     * as searchable pose samples with their motion features.
     *
     * @param name Animation name (e.g., "Walk", "Run")
     * @param anim The animation to add (database takes ownership)
     * @param skeleton Skeleton for bone indices (foot bones, etc.)
     */
    void AddAnimation(const std::string& name, std::shared_ptr<Animation> anim,
                      const class Skeleton* skeleton);

    /**
     * Clear all animations from database
     */
    void Clear();

    /**
     * Get number of pose samples in database
     */
    size_t GetPoseCount() const { return poses.size(); }

    /**
     * Get number of animations in database
     */
    size_t GetAnimationCount() const { return animations.size(); }
    
    // =========================================================================
    // POSE SEARCHING
    // =========================================================================
    
    /**
     * Search for best matching pose
     * 
     * This is the CORE of motion matching. We search all poses
     * and find the one that best matches the query features.
     * 
     * @param query Current motion state (speed, direction, etc.)
     * @param trajectory Predicted future trajectory
     * @return Best matching pose index
     */
    SearchResult Search(const MotionFeatures& query, 
                       const Trajectory& trajectory) const;
    
    /**
     * Search with blending candidates
     * 
     * Returns multiple poses for smooth blending.
     * 
     * @param query Current motion state
     * @param trajectory Predicted trajectory
     * @param config Search configuration
     * @return Multiple search results for blending
     */
    SearchResults SearchWithBlending(const MotionFeatures& query,
                                     const Trajectory& trajectory,
                                     const MotionMatchingConfig& config) const;
    
    // =========================================================================
    // POSE ACCESS
    // =========================================================================
    
    /**
     * Get all poses (for KD-Tree building)
     */
    const std::vector<PoseSample>& GetPoses() const { return poses; }
    
    /**
     * Get pose sample by index
     */
    const PoseSample& GetPose(size_t index) const { return poses[index]; }
    
    /**
     * Get animation by index
     */
    std::shared_ptr<Animation> GetAnimation(size_t index) const;

    /**
     * Get animation by name
     */
    std::shared_ptr<Animation> GetAnimation(const std::string& name) const;

    // =========================================================================
    // DEBUG
    // =========================================================================

    /**
     * Print database statistics
     */
    void PrintStats() const;

    /**
     * Get debug info string
     */
    std::string GetDebugInfo() const;

private:
    // Pose storage
    std::vector<PoseSample> poses;              // All pose samples

    // Animation references (database owns animations via shared_ptr)
    struct AnimationEntry {
        std::string name;
        std::shared_ptr<Animation> animation;   // Owned by database
        size_t startPoseIndex;                  // First pose in this animation
        size_t endPoseIndex;                    // Last pose
    };
    std::vector<AnimationEntry> animations;     // All animations
    std::unordered_map<std::string, size_t> animationByName;  // Name → index

    // Feature extraction helpers
    void ExtractPoseFeatures(size_t poseIndex, std::shared_ptr<Animation> anim, float time,
                            const class Skeleton* skeleton);
    
    /**
     * Calculate pose score (lower = better match)
     */
    float CalculatePoseScore(const PoseSample& pose,
                            const MotionFeatures& query,
                            const Trajectory& trajectory,
                            const MotionMatchingConfig& config) const;
    
    /**
     * Feature weights (tune matching behavior)
     */
    struct FeatureWeights {
        float velocityWeight = 3.0f;        // How important is velocity match
        float directionWeight = 2.0f;       // How important is direction match
        float trajectoryWeight = 1.5f;      // How important is trajectory match
        float footPlantWeight = 1.0f;       // How important is foot plant match
        float crouchWeight = 0.5f;          // How important is crouch state
    };
    
    FeatureWeights weights;
};
