#pragma once
#include "MotionMatchingTypes.h"
#include "MotionKDTreeSIMD.h"
#include "../animationSystem/Animation.h"
#include "../boneSystem/Skeleton.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <array>

// ============================================================================
// CACHE-FRIENDLY MOTION DATA (Struct-of-Arrays)
// ============================================================================
//
// CRITICAL PERFORMANCE FIX:
// Traditional Array-of-Structs (AoS) layout:
//   struct Pose { float speed; vec3 velocity; bool footPlanted; };
//   std::vector<Pose> poses;  // BAD for cache during search
//
// Our Struct-of-Arrays (SoA) layout:
//   struct MotionData {
//     std::vector<float> speeds;
//     std::vector<vec3> velocities;
//     std::vector<bool> footPlanted;
//   };  // GOOD for cache during search
//
// When searching, we access the same field across many poses.
// SoA ensures contiguous memory access = better cache utilization.
// ============================================================================

/**
 * Cache-Friendly Motion Data Storage
 * 
 * This is the SoA (Struct of Arrays) layout for motion matching.
 * Instead of storing all features together per pose (AoS),
 * we store each feature type contiguously (SoA).
 * 
 * Benefits:
 * - SIMD-friendly for parallel comparison
 * - Better cache utilization during search
 * - Reduced memory bandwidth
 */
struct CacheFriendlyMotionData {
    // Primary locomotion features (most frequently accessed)
    std::vector<float> speeds;                    // Movement speed
    std::vector<glm::vec3> rootVelocities;        // Root bone velocity
    std::vector<float> moveAngles;                // Movement direction angle
    
    // Secondary features (accessed less frequently)
    std::vector<float> facingAngles;              // Root facing angle
    std::vector<float> angularVelocities;         // Root yaw rate (rad/s) - ghost-node scoring
    std::vector<bool> isGrounded;                 // Grounded state
    std::vector<bool> isCrouching;                // Crouch state
    
    // Foot state (for foot planting)
    std::vector<bool> leftFootPlanted;            // Left foot planted
    std::vector<bool> rightFootPlanted;           // Right foot planted
    std::vector<glm::vec3> leftFootPositions;     // Left foot position
    std::vector<glm::vec3> rightFootPositions;    // Right foot position
    
    // Animation metadata
    std::vector<float> animationTimes;            // Time in animation
    std::vector<int> animationIndices;            // Which animation
    std::vector<int> frameNumbers;                // Frame number
    
    // Pose links (for blending)
    std::vector<int> nextFrameIndices;            // Next frame in animation
    std::vector<int> prevFrameIndices;            // Previous frame
    
    // Root transform
    std::vector<glm::vec3> rootPositions;         // Root position
    std::vector<float> rootRotationsY;            // Root Y rotation
    
    // Default constructor
    CacheFriendlyMotionData() = default;
    
    // Move constructor and assignment (vectors support move)
    CacheFriendlyMotionData(CacheFriendlyMotionData&&) = default;
    CacheFriendlyMotionData& operator=(CacheFriendlyMotionData&&) = default;
    
    // Copy constructor and assignment (explicit, for safety)
    CacheFriendlyMotionData(const CacheFriendlyMotionData&) = default;
    CacheFriendlyMotionData& operator=(const CacheFriendlyMotionData&) = default;
    
    /**
     * Get number of stored poses
     */
    size_t GetPoseCount() const { return speeds.size(); }
    
    /**
     * Clear all data
     */
    void Clear() {
        speeds.clear();
        rootVelocities.clear();
        moveAngles.clear();
        facingAngles.clear();
        angularVelocities.clear();
        isGrounded.clear();
        isCrouching.clear();
        leftFootPlanted.clear();
        rightFootPlanted.clear();
        leftFootPositions.clear();
        rightFootPositions.clear();
        animationTimes.clear();
        animationIndices.clear();
        frameNumbers.clear();
        nextFrameIndices.clear();
        prevFrameIndices.clear();
        rootPositions.clear();
        rootRotationsY.clear();
    }
    
    /**
     * Reserve memory for N poses (prevents reallocations)
     */
    void Reserve(size_t numPoses) {
        speeds.reserve(numPoses);
        rootVelocities.reserve(numPoses);
        moveAngles.reserve(numPoses);
        facingAngles.reserve(numPoses);
        angularVelocities.reserve(numPoses);
        isGrounded.reserve(numPoses);
        isCrouching.reserve(numPoses);
        leftFootPlanted.reserve(numPoses);
        rightFootPlanted.reserve(numPoses);
        leftFootPositions.reserve(numPoses);
        rightFootPositions.reserve(numPoses);
        animationTimes.reserve(numPoses);
        animationIndices.reserve(numPoses);
        frameNumbers.reserve(numPoses);
        nextFrameIndices.reserve(numPoses);
        prevFrameIndices.reserve(numPoses);
        rootPositions.reserve(numPoses);
        rootRotationsY.reserve(numPoses);
    }
    
    /**
     * Add a pose sample to SoA storage
     */
    void AddPose(const PoseSample& pose) {
        speeds.push_back(pose.features.speed);
        rootVelocities.push_back(pose.features.rootVelocity);
        moveAngles.push_back(pose.features.moveAngle);
        facingAngles.push_back(pose.features.facingAngle);
        angularVelocities.push_back(pose.features.angularVelocity);
        isGrounded.push_back(pose.features.isGrounded);
        isCrouching.push_back(pose.features.isCrouching);
        leftFootPlanted.push_back(pose.leftFootPlanted);
        rightFootPlanted.push_back(pose.rightFootPlanted);
        leftFootPositions.push_back(pose.features.leftFootPos);
        rightFootPositions.push_back(pose.features.rightFootPos);
        animationTimes.push_back(pose.timeInSeconds);
        animationIndices.push_back(pose.animationIndex);
        frameNumbers.push_back(pose.frameNumber);
        nextFrameIndices.push_back(pose.nextFrameIndex);
        prevFrameIndices.push_back(pose.prevFrameIndex);
        rootPositions.push_back(pose.rootPosition);
        rootRotationsY.push_back(pose.rootRotationY);
    }
    
    /**
     * Get feature value for pose index (read-only, cache-friendly)
     */
    float GetSpeed(size_t index) const { return speeds[index]; }
    const glm::vec3& GetVelocity(size_t index) const { return rootVelocities[index]; }
    float GetMoveAngle(size_t index) const { return moveAngles[index]; }
    float GetAngularVelocity(size_t index) const { return angularVelocities[index]; }
    bool IsLeftFootPlanted(size_t index) const { return leftFootPlanted[index]; }
    bool IsRightFootPlanted(size_t index) const { return rightFootPlanted[index]; }
};

// ============================================================================
// MOTION DATABASE (FIXED - Proper shared_ptr ownership)
// ============================================================================

/**
 * Motion Database - Stores animation poses for motion matching
 * 
 * CRITICAL: This class manages complex internal state with both
 * Array-of-Structs (AoS) and Struct-of-Arrays (SoA) data.
 * 
 * Copy operations are DELETED to prevent accidental copying and memory issues.
 * Use std::shared_ptr<MotionDatabase> for sharing.
 */
class MotionDatabase {
public:
    MotionDatabase();
    ~MotionDatabase();
    
    // Delete copy operations to prevent memory issues
    MotionDatabase(const MotionDatabase&) = delete;
    MotionDatabase& operator=(const MotionDatabase&) = delete;
    
    // Allow move operations
    MotionDatabase(MotionDatabase&& other) noexcept;
    MotionDatabase& operator=(MotionDatabase&& other) noexcept;

    // =========================================================================
    // DATABASE CONSTRUCTION
    // =========================================================================

    /**
     * Add animation to database
     *
     * CRITICAL: This takes OWNERSHIP via shared_ptr.
     * The database will manage the animation lifetime.
     *
     * Usage:
     *   auto anim = std::make_shared<Animation>("Walk", duration, fps);
     *   database.AddAnimation("Walk", anim, skeleton);
     *   // anim is now owned by database - don't delete it!
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
    // POSE SEARCHING (Cache-Optimized)
    // =========================================================================

    /**
     * Search for best matching pose
     *
     * Uses cache-friendly SoA layout for fast feature comparison.
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

    /**
     * Cache-friendly search using the flat SoA pose layout.
     *
     * Dispatches to the 8-wide AVX2 kernel (SearchSIMD) when the CPU supports
     * AVX2 + FMA, and to the allocation-free scalar SoA loop otherwise. Both
     * paths compute the identical weighted metric, so results do not depend on
     * which one ran. Use SetForceScalarSearch(true) to pin the scalar reference
     * (the equivalence tests do this).
     *
     * @param query Query features
     * @param trajectory Trajectory to match
     * @param maxCandidates How many candidates to return
     * @param airborneFilter Optional state gate: -1 = any pose, 0 = grounded
     *                       poses only, 1 = airborne poses only (UE-style
     *                       "match within the current movement state").
     * @return Array of candidate indices sorted by score
     */
    std::vector<std::pair<int, float>> SearchCacheOptimized(
        const MotionFeatures& query,
        const Trajectory& trajectory,
        int maxCandidates = 10,
        int airborneFilter = -1) const;

    // ---- AVX2 SIMD-accelerated search ---------------------------------------
    // Builds an aligned SoA snapshot of the database and runs the 8-wide AVX2
    // distance evaluator that mirrors CalculatePoseScoreCacheOptimized. The SoA
    // cache is rebuilt only when the database changes.
    //
    // `airborneFilter` has the same meaning as in SearchCacheOptimized. When
    // AVX2/FMA is unavailable this still returns the correct candidates via the
    // kernel's scalar tail; callers that want the allocation-free scalar loop
    // should call SearchCacheOptimized instead.
    std::vector<std::pair<int, float>> SearchSIMD(
        const MotionFeatures& query,
        const Trajectory& trajectory,
        int maxCandidates = 10,
        int airborneFilter = -1) const;

    /**
     * Pin / query the scalar reference search (e.g. to A/B a CPU without AVX2,
     * or in tests that assert AVX2 results match the scalar ones bit-for-bit in
     * ordering). Process-wide, defaults to false.
     */
    static void SetForceScalarSearch(bool force);
    static bool IsForceScalarSearch();

    /**
     * Enable/disable the root-relative future-path ("trajectory") term in the
     * flat SoA search. Process-wide, defaults to ON.
     *
     * Why it exists: the KD tree scores the Unreal-style future path (its dims
     * 7..7+2*kTrajectorySteps-1), but the flat search historically did not -
     * its trajectory hook went through Trajectory::getDifference, which reads
     * `trajectory.positions/velocities` and is inert for database poses (those
     * are never populated; only `trajectory.localPositions` is). Two search
     * paths scoring different features silently pick different clips when the
     * matcher alternates between them. The term is now applied by BOTH the
     * scalar and AVX2 flat implementations, from the same falloff weights as
     * the tree (kDefaultTrajectoryFalloff). Set to false to restore the old
     * (trajectory-blind) flat metric.
     */
    static void SetFlatSearchUsesTrajectory(bool on);
    static bool FlatSearchUsesTrajectory();

    /**
     * Measure how much faster the AVX2 flat search is than the scalar
     * reference on THIS database, by timing both over the current query load.
     * Returns 0 when AVX2/FMA is unavailable. Intended for the PlayMode
     * diagnostics HUD (it is a real measurement, not a compile-time label).
     *
     * @param iterations Number of full passes over the query set per path.
     */
    double MeasureFlatSearchSpeedup(int iterations = 20) const;

    /**
     * Check if the SIMD database cache needs rebuilding
     */
    void RebuildSIMDCacheIfNeeded() const;

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

    /**
     * Get the REGISTERED name of the animation at index (the name passed to
     * AddAnimation, e.g. "Jump"), not the raw FBX channel name ("mixamo.com"
     * for Mixamo rigs - identical across clips).
     */
    std::string GetAnimationName(size_t index) const {
        if (index >= animations.size()) return std::string();
        return animations[index].name;
    }

    /**
     * Nominal (authored) gait speed of a clip - the mean root-motion speed
     * feature over its poses. Used by the matcher's persistence hysteresis
     * to detect speed-band state changes (run<->walk, walk<->stop) and
     * switch clips immediately instead of waiting for the 15% distance
     * margin. Lazily computed once and cached (so clips added later still
     * work); cache is dropped on Clear().
     */
    float GetClipNominalSpeed(size_t animIndex) const;

    /**
     * Nominal (authored) move direction of a clip - the circular mean of its
     * poses' moveAngle features (the angle in clip/root space the root motion
     * points). Used by the matcher's persistence hysteresis to detect
     * direction-away state changes (reversal / backpedal: the query is moving
     * AWAY from the clip's heading) and switch clips immediately instead of
     * waiting for the 15% distance margin. Computed as a circular mean (the
     * unit vectors are averaged, not the raw angles) so turn clips that sweep
     * across the +/-pi seam don't average out to a wrong heading. Lazily
     * computed once and cached; cache is dropped on Clear().
     */
    float GetClipNominalMoveAngle(size_t animIndex) const;

    /**
     * Does this database contain airborne (Jump/Fall) poses?
     *
     * The matcher gates the pose search by airborne state only when airborne
     * clips exist - a locomotion-only database keeps its current behavior.
     */
    bool HasAirbornePoses() const { 
        for (const auto& p : poses) if (p.features.isAirborne) return true;
        return false;
    }

    /**
     * Get cache-friendly motion data (for optimized search)
     */
    const CacheFriendlyMotionData& GetCacheFriendlyData() const { return motionData; }

    // =========================================================================
    // DATABASE STRUCTURES
    // =========================================================================
    
    /**
     * Animation entry - stores animation and pose range
     */
    struct AnimationEntry {
        std::string name;
        std::shared_ptr<Animation> animation;   // OWNED by database
        size_t startPoseIndex;                  // First pose in this animation
        size_t endPoseIndex;                    // Last pose
    };

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
    // Traditional AoS storage (for compatibility)
    std::vector<PoseSample> poses;

    // Cache-friendly SoA storage (for fast search)
    CacheFriendlyMotionData motionData;

    // Animation references (database OWNS animations via shared_ptr)
    std::vector<AnimationEntry> animations;     // All animations
    std::unordered_map<std::string, size_t> animationByName;  // Name → index

    // Per-animation nominal gait speed cache (see GetClipNominalSpeed);
    // -1.0f = not computed yet. Mutable so it can be filled from a const
    // accessor; size must match animations.size() whenever filled.
    mutable std::vector<float> clipNominalSpeeds_;

    // Per-animation nominal move-angle cache (see GetClipNominalMoveAngle);
    // std::numeric_limits<float>::max() = not computed yet (angles can be any
    // radian value, including -1, so a plain -1 sentinel is unusable). Mutable
    // so it can be filled from a const accessor; size must match
    // animations.size() whenever filled.
    mutable std::vector<float> clipNominalMoveAngles_;

    // ---- AVX2 SIMD search cache (rebuilt when poses change) ----------------
    mutable struct SIMDCache {
        mutable SIMDMotionDatabaseSoA soa;
        mutable bool                  valid = false;
        mutable size_t                poseCount = 0;
    } simdCache_;

    // Feature extraction helpers
    void ExtractPoseFeatures(size_t poseIndex, std::shared_ptr<Animation> anim, float time,
                            const class Skeleton* skeleton);

    /**
     * Scalar reference for SearchCacheOptimized (the exact loop the AVX2 kernel
     * mirrors). Kept as a separate entry point so equivalence tests can compare
     * the two implementations directly.
     */
    std::vector<std::pair<int, float>> SearchCacheOptimizedScalar(
        const MotionFeatures& query,
        const Trajectory& trajectory,
        int maxCandidates,
        int airborneFilter) const;

    /**
     * Calculate pose score (lower = better match)
     */
    float CalculatePoseScore(const PoseSample& pose,
                            const MotionFeatures& query,
                            const Trajectory& trajectory,
                            const MotionMatchingConfig& config) const;

    /**
     * Cache-friendly pose score calculation (SoA-optimized)
     * 
     * Uses contiguous memory access for better cache utilization.
     * Can be SIMD-optimized for parallel comparison.
     */
    float CalculatePoseScoreCacheOptimized(size_t poseIndex,
                                           const MotionFeatures& query,
                                           const Trajectory& trajectory,
                                           const MotionMatchingConfig& config) const;

    /**
     * Root-relative future-path term shared by the flat scalar reference and
     * the AVX2 kernel (which mirrors this expression exactly):
     *
     *   trajectoryWeight · Σ_{k<cmp} w_k·‖pose.localPositions[k] − query.futureLocal[k]‖
     *                      / Σ_{k<cmp} w_k
     *
     * with cmp = min(kTrajectorySteps, query.futureCount), w = trajectoryFalloff_.
     * Returns 0 when the feature is disabled on either side, so a missing
     * future path contributes nothing instead of biasing the score.
     */
    float CalculateTrajectoryTerm(const PoseSample& pose,
                                  const MotionFeatures& query) const;

    // Near->far falloff for the future-path term; mirrors
    // MotionKDTree::kDefaultTrajectoryWeights (both sourced from
    // kDefaultTrajectoryFalloff).
    float trajectoryFalloff_[kTrajectorySteps] = {
        kDefaultTrajectoryFalloff[0], kDefaultTrajectoryFalloff[1],
        kDefaultTrajectoryFalloff[2], kDefaultTrajectoryFalloff[3]
    };

    /**
     * Feature weights (tune matching behavior)
     */
public:
    struct FeatureWeights {
        float velocityWeight = 3.0f;        // How important is velocity match
        float speedWeight = 2.0f;           // How important is speed match
        float directionWeight = 2.0f;       // How important is direction match
        float trajectoryWeight = 1.5f;      // How important is trajectory match
        float footPlantWeight = 1.0f;       // How important is foot plant match
        float crouchWeight = 0.5f;          // How important is crouch state
    };

    FeatureWeights weights;

    /**
     * Get the feature weights used by the pose-search scoring, so external
     * callers (e.g. TransitionClipGenerator::CalculateAdvancedTrajectoryPenalty)
     * can scale the trajectory forward-look-ahead term consistently with the
     * flat / KD-tree search weights.
     */
    const FeatureWeights& GetWeights() const { return weights; }
    // forUI.txt Block 1: live-editable handle to the feature weights so the
    // Details-panel sliders write straight into the matcher's scoring profile.
    FeatureWeights& GetWeightsRef() { return weights; }
};
