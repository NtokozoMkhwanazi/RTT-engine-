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
    
    // =========================================================================
    // DEBUG
    // =========================================================================
    
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

    // Feature extraction (converts PoseSample to feature vector)
    std::vector<float> GetFeatureVector(const PoseSample& pose) const;
    std::vector<float> GetFeatureVector(const MotionFeatures& features) const;

    // Tree building
    std::unique_ptr<KDTreeNode> BuildRecursive(
        std::vector<int>& indices,
        int depth,
        int maxLeafSize);

    // SAH-based tree building with binning
    struct BinBounds {
        float minBounds[5];  // Min value per feature dimension
        float maxBounds[5];  // Max value per feature dimension
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

    // Distance calculation
    float CalculateDistance(
        const std::vector<float>& a,
        const std::vector<float>& b) const;

    // Feature weights (tune search behavior)
    struct FeatureWeights {
        float speed = 3.0f;           // Weight for speed dimension
        float velocityX = 2.0f;       // Weight for X velocity
        float velocityZ = 2.0f;       // Weight for Z velocity
        float direction = 1.5f;       // Weight for direction
        float footPlant = 1.0f;       // Weight for foot plant state
    };

    FeatureWeights weights;

    // Constants
    static constexpr int NUM_FEATURES = 5;  // Number of dimensions in feature space
};
