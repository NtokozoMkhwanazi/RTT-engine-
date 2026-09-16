#pragma once
#include "MotionMatchingTypes.h"
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>

// ============================================================================
// KD-TREE FOR MOTION MATCHING
// ============================================================================
// 
// Spatial data structure for fast nearest-neighbor search.
// Reduces search time from O(n) to O(log n).
// 
// For 1000 poses:
//   Brute force: ~1-2ms
//   KD-Tree:     ~0.1-0.3ms (5-10x faster!)
// ============================================================================

/**
 * KD-Tree Node
 * 
 * Each node represents a point in feature space.
 * Left child has smaller value on split axis.
 * Right child has larger value on split axis.
 */
struct KDTreeNode {
    int poseIndex{-1};              // Index into pose database (-1 for internal nodes)
    int splitAxis{0};               // Which feature dimension we split on
    float splitValue{0.0f};         // Split point value

    // ALL pose indices contained in this leaf (leaf nodes only). The tree
    // used to store a single representative pose (indices[0]) per leaf, which
    // made the kNN search evaluate ~1 pose per visited leaf instead of every
    // pose in it - the returned "nearest" set was arbitrary (whatever pose
    // happened to sort first in each leaf) and clip-biased. That is what made
    // the matcher re-select Idle while moving, leak Jump/Fall poses into
    // grounded queries, and pick CrouchWalk at sprint. Storing the full leaf
    // makes the search exact kNN over all poses.
    std::vector<int> leafIndices;
    
    std::unique_ptr<KDTreeNode> left;   // Left subtree (smaller values)
    std::unique_ptr<KDTreeNode> right;  // Right subtree (larger values)
    
    bool isLeaf() const { return !left && !right; }
};

/**
 * Search Result for KD-Tree
 */
struct KDTSearchResult {
    int poseIndex{-1};
    float score{0.0f};
    float distance{0.0f};       // Distance in feature space
    
    bool operator<(const KDTSearchResult& other) const {
        return distance < other.distance;
    }
};

/**
 * KD-Tree for Motion Matching
 * 
 * Builds a spatial index over pose features for fast search.
 */
class MotionKDTree {
public:
    // Number of dimensions in feature space:
    //   0 speed, 1-2 velocity(XZ), 3 moveAngle, 4-5 foot plants,
    //   6 vertical root velocity (airborne ascent/descent discrimination),
    //   7..7+2*kTrajectorySteps-1 root-local future path (x,z per point)
    static constexpr int NUM_FEATURES = 7 + 2 * kTrajectorySteps;

    // Default near->far trajectory falloff (motion matching weights nearer
    // future points more). Shared by the runtime member initializer and by
    // MotionMatcher::SetSearchWeights so the tuned scale is defined once.
    static constexpr float kDefaultTrajectoryWeights[kTrajectorySteps] =
        { 4.0f, 2.5f, 1.5f, 1.0f };

    MotionKDTree();
    ~MotionKDTree();
    
    // =========================================================================
    // TREE CONSTRUCTION
    // =========================================================================

    /**
     * Build tree from pose database
     *
     * @param poses Vector of all poses to index
     * @param maxLeafSize Maximum poses per leaf (default 10)
     */
    void Build(const std::vector<PoseSample>& poses, int maxLeafSize = 10);

    /**
     * Build tree with SAH optimization
     *
     * @param poses Vector of all poses to index
     * @param maxLeafSize Maximum poses per leaf
     * @param useSAH Enable SAH-based splitting (default true)
     * @param numBins Number of bins for SAH evaluation (default 12)
     */
    void BuildWithSAH(const std::vector<PoseSample>& poses, int maxLeafSize = 10, 
                      bool useSAH = true, int numBins = 12);

    /**
     * Clear tree
     */
    void Clear();
    
    /**
     * Check if tree is built
     */
    bool IsBuilt() const { return root != nullptr; }
    
    /**
     * Get number of poses in tree
     */
    size_t GetPoseCount() const { return poseCount; }
    
    // =========================================================================
    // SEARCHING
    // =========================================================================
    
    /**
     * Find nearest neighbor
     * 
     * @param query Query features
     * @return Best matching pose index (-1 if not found)
     */
    int FindNearest(const MotionFeatures& query) const;
    
    /**
     * Find K nearest neighbors
     * 
     * @param query Query features
     * @param k Number of neighbors to find
     * @return K best matching poses (sorted by distance)
     */
    std::vector<KDTSearchResult> FindKNearest(const MotionFeatures& query, int k) const;
    
    /**
     * Find all poses within radius
     * 
     * @param query Query features
     * @param radius Search radius in feature space
     * @return All poses within radius (sorted by distance)
     */
    std::vector<KDTSearchResult> FindWithinRadius(
        const MotionFeatures& query, 
        float radius) const;

    /**
     * Find the best (nearest) pose belonging to a specific clip (animation),
     * evaluated in the KD tree's own feature-space metric. Used by the matcher
     * persistence to recover the current clip's true best pose when it is
     * absent from the top-k window (e.g. another clip floods the near set with
     * near-identical poses). Returns a sentinel (poseIndex<0) if the tree is
     * not built or the clip has no poses.
     */
    KDTSearchResult FindBestInClip(const MotionFeatures& query, int animIdx) const;
    
    // =========================================================================
    // FEATURE-VIEW / METRIC EXTRACTION (PUBLIC)
    // =========================================================================
    // Exposed so MotionMatcher::SelectPoseWithPersistence can sample the TRUE
    // feature distance of a pose evicted from the Top-K window (FindKNearest
    // returns only the near set; a dense 120fps clip can flood it, evicting
    // the active frame). Same primitives the tree uses internally during
    // search, so the sampled distance is on the identical KD metric.
    std::vector<float> GetFeatureVector(const PoseSample& pose) const;
    std::vector<float> GetFeatureVector(const MotionFeatures& features) const;
    float CalculateDistance(const std::vector<float>& a,
                            const std::vector<float>& b) const;
    
    // =========================================================================
    // DEBUG
    // =========================================================================
    
    // =========================================================================
    // SEARCH WEIGHTS (live-tunable)
    // =========================================================================
    // The tree structure is built on feature VALUES; the weights only shape the
    // distance metric at search time, so they can be changed at runtime with no
    // index rebuild (the Character menu exposes them as sliders).

    void SetSpeedWeight(float w) { weights.speed = w; }
    void SetVelocityWeights(float x, float z) { weights.velocityX = x; weights.velocityZ = z; }
    void SetDirectionWeight(float w) { weights.direction = w; }
    void SetFootPlantWeight(float w) { weights.footPlant = w; }
    void SetVerticalVelocityWeight(float w) { weights.verticalVelocity = w; }
    void SetTrajectoryWeights(const float w[kTrajectorySteps]) {
        for (int i = 0; i < kTrajectorySteps; ++i) trajectoryWeights[i] = w[i];
    }

    float GetSpeedWeight() const { return weights.speed; }
    float GetVelocityXWeight() const { return weights.velocityX; }
    float GetVelocityZWeight() const { return weights.velocityZ; }
    float GetDirectionWeight() const { return weights.direction; }
    float GetFootPlantWeight() const { return weights.footPlant; }
    float GetVerticalVelocityWeight() const { return weights.verticalVelocity; }
    float GetTrajectoryWeight(int point) const {
        return (point >= 0 && point < kTrajectorySteps) ? trajectoryWeights[point] : 0.0f;
    }

    /**
     * Get tree statistics
     */
    struct TreeStats {
        size_t totalNodes{0};
        size_t leafNodes{0};
        size_t internalNodes{0};
        int maxDepth{0};
        float avgLeafSize{0.0f};
    };
    
    TreeStats GetStats() const;
    
    /**
     * Print tree structure (for debugging)
     */
    void PrintStructure(int maxDepth = 3) const;
    
private:
    std::unique_ptr<KDTreeNode> root;
    std::vector<PoseSample> poseData;  // Internal copy of poses (owns the data)
    size_t poseCount{0};

    // SAH Configuration
    static constexpr int SAH_NUM_BINS = 12;      // Number of bins for SAH evaluation
    static constexpr float SAH_TRAVERSAL_COST = 1.0f;    // Cost of traversing a node
    static constexpr float SAH_INTERSECTION_COST = 1.0f; // Cost of intersecting a primitive

    // Tree building
    std::unique_ptr<KDTreeNode> BuildRecursive(
        std::vector<int>& indices,
        int depth,
        int maxLeafSize);

    // SAH-based tree building with binning
    struct BinBounds {
        float minBounds[NUM_FEATURES];  // Min value per feature dimension
        float maxBounds[NUM_FEATURES];  // Max value per feature dimension
    };

    struct SAHSplit {
        int bestAxis{-1};
        int bestBin{-1};
        float bestCost{std::numeric_limits<float>::max()};
    };

    // Compute bounds for all primitives
    BinBounds ComputeBounds(const std::vector<int>& indices) const;

    // Evaluate SAH cost for all bin boundaries
    SAHSplit FindBestSplit(const std::vector<int>& indices, int maxLeafSize) const;

    // Build with SAH optimization
    std::unique_ptr<KDTreeNode> BuildWithSAHRecursive(
        std::vector<int>& indices,
        int depth,
        int maxLeafSize,
        const BinBounds& bounds,
        int numBins);

    // Search helpers
    void SearchRecursive(
        const KDTreeNode* node,
        const std::vector<float>& query,
        float& bestDist,
        int& bestIndex) const;

    void SearchKNearestRecursive(
        const KDTreeNode* node,
        const std::vector<float>& query,
        int k,
        std::vector<KDTSearchResult>& results,
        float& maxDistInResults) const;

    void SearchRadiusRecursive(
        const KDTreeNode* node,
        const std::vector<float>& query,
        float radius,
        std::vector<KDTSearchResult>& results) const;

    // Feature weights (tune search behavior)
    // CRITICAL: speed weight must be HIGH to ensure correct animation selection
    struct FeatureWeights {
        float speed = 15.0f;          // HIGH weight - speed magnitude is MOST important
        float velocityX = 1.0f;       // Lower - direction less important
        float velocityZ = 1.0f;       // Lower - direction less important
        float direction = 0.5f;       // Low - direction can be corrected by blending
        // UE-style motion phase: a full foot-contact mismatch costs (1)^2 * w.
        // Raised from 1.0 so the query's carried foot-plant state (added in
        // MotionMatcher::SearchAndBlend) actually steers the search toward
        // same-gait-phase poses - previously planted poses were PENALIZED by
        // the (1-0)^2 term while the query always read both-feet-unplanted,
        // which is footskating.
        float footPlant = 2.0f;       // Weight for foot plant state
        float verticalVelocity = 2.0f; // Airborne ascent/descent discrimination
    };

    FeatureWeights weights;

    // Weights for the root-local trajectory dims: nearer future points matter
    // more than far ones (Unreal-style). Indexed by trajectory point 0..N-1.
    // Runtime-mutable via SetTrajectoryWeights (defaults match the tuned
    // near->far falloff).
    float trajectoryWeights[kTrajectorySteps] = {
        kDefaultTrajectoryWeights[0], kDefaultTrajectoryWeights[1],
        kDefaultTrajectoryWeights[2], kDefaultTrajectoryWeights[3]
    };
};
