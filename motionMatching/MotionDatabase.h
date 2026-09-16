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
     * Cache-friendly search using SoA layout
     * 
     * This is OPTIMIZED for SIMD and cache efficiency.
     * Accesses contiguous memory for each feature comparison.
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
    // Builds an aligned SoA snapshot of the database and runs an 8-wide
    // AVX2 distance evaluator.  Falls back gracefully if the CPU lacks AVX2.
    // The SoA cache is rebuilt only when the database changes (dirty flag).
    std::vector<std::pair<int, float>> SearchSIMD(
        const MotionFeatures& query,
        const Trajectory& trajectory,
        int maxCandidates = 10,
        int airborneFilter = -1) const;

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
     * Get the pose range [startPoseIndex, endPoseIndex] for the animation
     * at the given index. Used by MotionMatcher to derive a time-aligned
     * pose index inside an animation clip (instead of re-running the KD-tree
     * on every frame during rest).
     */
    void GetPoseRange(size_t index, size_t& outStart, size_t& outEnd) const {
        if (index >= animations.size()) {
            outStart = 0;
            outEnd = 0;
            return;
        }
        outStart = animations[index].startPoseIndex;
        outEnd = animations[index].endPoseIndex;
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

    // FIX (v11 Section 3): AVX2-accelerated trajectory feature extraction.
    // Vectorizes the future-path look-ahead (4 sample points in a single
    // _mm256_sub_ps) instead of looping scalar. Caller must guard with
    // cpuSupportsAVX2() — falls back to ExtractPoseFeatures otherwise.
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((target("avx2,fma")))
#endif
    void ExtractPoseFeaturesSIMD(size_t poseIndex, std::shared_ptr<Animation> anim,
                                 float time, float rootPos_x, float rootPos_z);

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
     * Feature weights (tune matching behavior)
     */
    struct FeatureWeights {
        float velocityWeight = 3.0f;        // How important is velocity match
        float speedWeight = 2.0f;           // How important is speed match
        float directionWeight = 2.0f;       // How important is direction match
        float trajectoryWeight = 1.5f;      // How important is trajectory match
        float footPlantWeight = 1.0f;       // How important is foot plant match
        float crouchWeight = 0.5f;          // How important is crouch state
    };

    FeatureWeights weights;
};
