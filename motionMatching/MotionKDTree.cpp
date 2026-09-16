#include "MotionKDTree.h"
#include <iostream>
#include <queue>
#include <iomanip>
#include <limits>
#include <future>

// Only parallelize KD-Tree construction for large pose sets; small trees (and
// the small test fixtures) stay serial so the build stays deterministic and
// thread-overhead-free. The two subtrees are built from DISJOINT index vectors
// and only READ poseData (written once before recursion), so the fork is
// race-free by construction; max in-flight threads is O(log(n/threshold)).
static constexpr int kParallelBuildThreshold = 128;

MotionKDTree::MotionKDTree() {}

MotionKDTree::~MotionKDTree() {
    Clear();
}

void MotionKDTree::Clear() {
    root.reset();
    poseData.clear();
    poseCount = 0;
}

// ============================================================================
// FEATURE EXTRACTION
// ============================================================================

std::vector<float> MotionKDTree::GetFeatureVector(const PoseSample& pose) const {
    // Extract key features for KD-Tree search
    // These dimensions are chosen for good search performance
    std::vector<float> vec = {
        pose.features.speed,                          // 0: Speed
        pose.features.rootVelocity.x,                 // 1: X velocity
        pose.features.rootVelocity.z,                 // 2: Z velocity
        pose.features.moveAngle,                      // 3: Movement direction
        pose.features.leftFootPlanted ? 1.0f : 0.0f,  // 4: Left foot planted
        pose.features.rightFootPlanted ? 1.0f : 0.0f, // 5: Right foot planted
        pose.features.rootVelocity.y                  // 6: Vertical velocity (airborne)
    };
    // 7..: root-relative future path (x,z per point) - the Unreal-style feature
    // that ranks same-speed poses by where they're going.
    for (int k = 0; k < kTrajectorySteps; ++k) {
        if (pose.trajectory.localNumPoints > k) {
            vec.push_back(pose.trajectory.localPositions[k].x);
            vec.push_back(pose.trajectory.localPositions[k].z);
        } else {
            vec.push_back(0.0f);
            vec.push_back(0.0f);
        }
    }
    return vec;
}

std::vector<float> MotionKDTree::GetFeatureVector(const MotionFeatures& features) const {
    std::vector<float> vec = {
        features.speed,
        features.rootVelocity.x,
        features.rootVelocity.z,
        features.moveAngle,
        features.leftFootPlanted ? 1.0f : 0.0f,
        features.rightFootPlanted ? 1.0f : 0.0f,
        features.rootVelocity.y
    };
    for (int k = 0; k < kTrajectorySteps; ++k) {
        if (features.futureCount > k) {
            vec.push_back(features.futureLocal[k].x);
            vec.push_back(features.futureLocal[k].y);
        } else {
            vec.push_back(0.0f);
            vec.push_back(0.0f);
        }
    }
    return vec;
}

// ============================================================================
// TREE BUILDING
// ============================================================================

void MotionKDTree::Build(const std::vector<PoseSample>& inPoses, int maxLeafSize) {
    // Use SAH-based building by default for better tree quality
    BuildWithSAH(inPoses, maxLeafSize, true, SAH_NUM_BINS);
}

std::unique_ptr<KDTreeNode> MotionKDTree::BuildRecursive(
    std::vector<int>& indices,
    int depth,
    int maxLeafSize) {
    
    auto node = std::make_unique<KDTreeNode>();
    
    // Choose split axis (cycle through dimensions)
    node->splitAxis = depth % NUM_FEATURES;
    
    // If few enough poses, make this a leaf
    if (static_cast<int>(indices.size()) <= maxLeafSize) {
        // CRITICAL: store ALL poses in the leaf. The search evaluates every
        // one, which is what makes FindKNearest an exact kNN search.
        node->leafIndices = indices;
        if (!indices.empty()) {
            node->poseIndex = indices[0];  // kept for stats/debug
        }
        return node;
    }
    
    // Find median along split axis
    int mid = indices.size() / 2;
    
    // Partial sort to find median
    std::nth_element(indices.begin(),
                     indices.begin() + mid,
                     indices.end(),
                     [this, axis = node->splitAxis](int a, int b) {
                         auto vecA = GetFeatureVector(poseData[a]);
                         auto vecB = GetFeatureVector(poseData[b]);
                         return vecA[axis] < vecB[axis];
                     });

    // Set split value
    node->splitValue = GetFeatureVector(poseData[indices[mid]])[node->splitAxis];
    node->poseIndex = indices[mid];
    
    // Split indices into left and right
    std::vector<int> leftIndices(indices.begin(), indices.begin() + mid);
    std::vector<int> rightIndices(indices.begin() + mid + 1, indices.end());
    
    // Build subtrees
    if (!leftIndices.empty()) {
        node->left = BuildRecursive(leftIndices, depth + 1, maxLeafSize);
    }
    if (!rightIndices.empty()) {
        node->right = BuildRecursive(rightIndices, depth + 1, maxLeafSize);
    }
    
    return node;
}

// ============================================================================
// SAH-BASED TREE BUILDING WITH BINNING
// ============================================================================

MotionKDTree::BinBounds MotionKDTree::ComputeBounds(const std::vector<int>& indices) const {
    BinBounds bounds;
    
    // Initialize bounds with first primitive
    if (indices.empty()) {
        for (int i = 0; i < NUM_FEATURES; i++) {
            bounds.minBounds[i] = 0.0f;
            bounds.maxBounds[i] = 0.0f;
        }
        return bounds;
    }
    
    auto firstVec = GetFeatureVector(poseData[indices[0]]);
    for (int i = 0; i < NUM_FEATURES; i++) {
        bounds.minBounds[i] = firstVec[i];
        bounds.maxBounds[i] = firstVec[i];
    }
    
    // Expand bounds to include all primitives
    for (size_t i = 1; i < indices.size(); i++) {
        auto vec = GetFeatureVector(poseData[indices[i]]);
        for (int j = 0; j < NUM_FEATURES; j++) {
            bounds.minBounds[j] = std::min(bounds.minBounds[j], vec[j]);
            bounds.maxBounds[j] = std::max(bounds.maxBounds[j], vec[j]);
        }
    }
    
    return bounds;
}

MotionKDTree::SAHSplit MotionKDTree::FindBestSplit(const std::vector<int>& indices, int maxLeafSize) const {
    SAHSplit bestSplit;
    
    if (indices.size() <= static_cast<size_t>(maxLeafSize)) {
        return bestSplit;  // No split needed
    }
    
    // Compute bounds
    BinBounds bounds = ComputeBounds(indices);
    
    // Try each axis
    for (int axis = 0; axis < NUM_FEATURES; axis++) {
        // Binary categorical escape: axes 4 and 5 are foot-plant states
        // (0.0f or 1.0f only). Standard floating-point binning splits at
        // fractional positions like 0.333f, creating degenerate sub-trees
        // with zero variance that collapse to O(n). Split exactly at 0.5f.
        if (axis == 4 || axis == 5) {
            float cost = SAH_TRAVERSAL_COST +
                         (indices.size() * 0.5f * SAH_INTERSECTION_COST);
            if (cost < bestSplit.bestCost) {
                bestSplit.bestAxis = axis;
                bestSplit.bestBin = SAH_NUM_BINS / 2;  // partition at 50% boundary
                bestSplit.bestCost = cost;
            }
            continue;  // skip standard floating-point continuous binning
        }

        float axisMin = bounds.minBounds[axis];
        float axisMax = bounds.maxBounds[axis];
        float axisRange = axisMax - axisMin;
        
        if (axisRange < 1e-6f) {
            continue;  // Skip degenerate axis
        }
        
        // Divide axis into bins
        const int numBins = SAH_NUM_BINS;
        float binWidth = axisRange / numBins;
        
        // Count primitives in each bin
        std::vector<int> leftCounts(numBins + 1, 0);
        std::vector<int> rightCounts(numBins + 1, 0);
        
        // Populate bin counts
        for (int idx : indices) {
            float value = GetFeatureVector(poseData[idx])[axis];
            int bin = std::min(numBins - 1, static_cast<int>((value - axisMin) / binWidth));
            
            // Add to right counts for all bins
            for (int b = 0; b <= numBins; b++) {
                if (b <= bin) {
                    leftCounts[b]++;
                } else {
                    rightCounts[b]++;
                }
            }
        }
        
        // Evaluate SAH cost at each bin boundary
        float totalSA = axisMax - axisMin;
        for (int otherAxis = 0; otherAxis < NUM_FEATURES; otherAxis++) {
            if (otherAxis != axis) {
                totalSA *= (bounds.maxBounds[otherAxis] - bounds.minBounds[otherAxis]);
            }
        }
        
        for (int bin = 1; bin < numBins; bin++) {
            int leftCount = leftCounts[bin];
            int rightCount = rightCounts[bin];
            
            if (leftCount == 0 || rightCount == 0) {
                continue;  // Skip empty splits
            }
            
            // Calculate SAH cost
            // Cost = C_traversal + C_intersection * (N_left * SA_left/SA_parent + N_right * SA_right/SA_parent)
            float binPos = axisMin + bin * binWidth;
            float leftSA = (binPos - axisMin) / axisRange;
            float rightSA = (axisMax - binPos) / axisRange;
            
            float cost = SAH_TRAVERSAL_COST + 
                         SAH_INTERSECTION_COST * (leftCount * leftSA + rightCount * rightSA);
            
            if (cost < bestSplit.bestCost) {
                bestSplit.bestAxis = axis;
                bestSplit.bestBin = bin;
                bestSplit.bestCost = cost;
            }
        }
    }
    
    return bestSplit;
}

std::unique_ptr<KDTreeNode> MotionKDTree::BuildWithSAHRecursive(
    std::vector<int>& indices,
    int depth,
    int maxLeafSize,
    const BinBounds& bounds,
    int numBins) {
    
    auto node = std::make_unique<KDTreeNode>();
    
    // If few enough primitives, make this a leaf
    if (static_cast<int>(indices.size()) <= maxLeafSize) {
        // CRITICAL: store ALL poses in the leaf so the search evaluates every
        // one (exact kNN) instead of a single arbitrary representative.
        node->leafIndices = indices;
        if (!indices.empty()) {
            node->poseIndex = indices[0];  // kept for stats/debug
        }
        return node;
    }
    
    // Find best split using SAH with binning
    SAHSplit split = FindBestSplit(indices, maxLeafSize);
    
    // If no valid split found (e.g. a subtree whose poses have identical
    // features), make this a leaf - and store ALL its poses. The search
    // evaluates leafIndices; leaving it empty here made such poses invisible
    // to the nearest-neighbor search.
    if (split.bestAxis < 0) {
        node->leafIndices = indices;
        if (!indices.empty()) {
            node->poseIndex = indices[0];  // kept for stats/debug
        }
        return node;
    }
    
    // Recompute the ACTUAL bounds of this index set. FindBestSplit bins from
    // the real data, so splitValue must use those same bounds; otherwise the
    // partition below can push every pose onto one side and recurse forever.
    BinBounds actualBounds = ComputeBounds(indices);
    node->splitAxis = split.bestAxis;
    float axisMin = actualBounds.minBounds[split.bestAxis];
    float axisMax = actualBounds.maxBounds[split.bestAxis];
    float axisRange = axisMax - axisMin;
    if (axisRange < 1e-6f) {
        // Degenerate along the chosen axis: bail out to a leaf.
        node->leafIndices = indices;
        if (!indices.empty()) {
            node->poseIndex = indices[0];
        }
        return node;
    }
    float binWidth = axisRange / numBins;
    node->splitValue = axisMin + split.bestBin * binWidth;
    node->poseIndex = -1;  // Internal node
    
    // Partition indices based on split
    std::vector<int> leftIndices, rightIndices;
    leftIndices.reserve(indices.size());
    rightIndices.reserve(indices.size());
    
    for (int idx : indices) {
        float value = GetFeatureVector(poseData[idx])[split.bestAxis];
        if (value < node->splitValue) {
            leftIndices.push_back(idx);
        } else {
            rightIndices.push_back(idx);
        }
    }
    
    // Guard against a no-progress split (one child holds everything).
    // Without this the recursion can go infinite on duplicate/degenerate
    // feature vectors.
    if (leftIndices.empty() || rightIndices.empty() ||
        leftIndices.size() == indices.size() ||
        rightIndices.size() == indices.size()) {
        node->leafIndices = indices;
        if (!indices.empty()) {
            node->poseIndex = indices[0];
        }
        node->splitAxis = -1;
        return node;
    }
    
    // Compute child bounds
    BinBounds leftBounds = bounds;
    BinBounds rightBounds = bounds;
    leftBounds.maxBounds[split.bestAxis] = node->splitValue;
    rightBounds.minBounds[split.bestAxis] = node->splitValue;
    
    // Build subtrees - parallelize the independent right-subtree construction
    // across a worker thread so index builds overlap with IO on large pose
    // databases. The two halves operate on DISJOINT index vectors
    // (leftIndices vs rightIndices) and only READ poseData (populated once by
    // BuildWithSAH before any recursion), so the fork is race-free by
    // construction. Gated by kParallelBuildThreshold so small trees (and the
    // small test fixtures) stay serial and deterministic.
    if (!leftIndices.empty() && !rightIndices.empty() &&
        (int)indices.size() > kParallelBuildThreshold) {
        auto rightFuture = std::async(std::launch::async,
            [this, depth, maxLeafSize, numBins,
             rightBounds, rightIndices = std::move(rightIndices)]() mutable {
                return BuildWithSAHRecursive(rightIndices, depth + 1,
                                             maxLeafSize, rightBounds, numBins);
            });
        node->left  = BuildWithSAHRecursive(leftIndices, depth + 1, maxLeafSize, leftBounds, numBins);
        node->right = rightFuture.get();
    } else {
        if (!leftIndices.empty())
            node->left  = BuildWithSAHRecursive(leftIndices, depth + 1, maxLeafSize, leftBounds, numBins);
        if (!rightIndices.empty())
            node->right = BuildWithSAHRecursive(rightIndices, depth + 1, maxLeafSize, rightBounds, numBins);
    }
    
    return node;
}

void MotionKDTree::BuildWithSAH(const std::vector<PoseSample>& inPoses, int maxLeafSize, 
                                 bool useSAH, int numBins) {
    Clear();
    
    // Copy poses internally
    poseData = inPoses;
    poseCount = inPoses.size();
    
    if (poseCount == 0) {
        std::cerr << "[MotionKDTree] ERROR: Cannot build tree from empty pose list!\n";
        return;
    }
    
    // Create index array
    std::vector<int> indices(poseCount);
    for (size_t i = 0; i < poseCount; i++) {
        indices[i] = static_cast<int>(i);
    }
    
    // Compute initial bounds
    BinBounds bounds = ComputeBounds(indices);
    
    // Build tree
    if (useSAH) {
        std::cout << "[MotionKDTree] Building with SAH optimization (" << numBins << " bins)...\n";
        root = BuildWithSAHRecursive(indices, 0, maxLeafSize, bounds, numBins);
    } else {
        root = BuildRecursive(indices, 0, maxLeafSize);
    }
    
    TreeStats stats = GetStats();
    std::cout << "[MotionKDTree] Built: " << stats.totalNodes << " nodes, "
              << stats.leafNodes << " leaves, depth=" << stats.maxDepth << "\n";
}

// ============================================================================
// SEARCHING
// ============================================================================

int MotionKDTree::FindNearest(const MotionFeatures& query) const {
    if (!root) return -1;
    
    auto queryVec = GetFeatureVector(query);
    float bestDist = std::numeric_limits<float>::max();
    int bestIndex = -1;
    
    SearchRecursive(root.get(), queryVec, bestDist, bestIndex);
    
    return bestIndex;
}

std::vector<KDTSearchResult> MotionKDTree::FindKNearest(
    const MotionFeatures& query, 
    int k) const {
    
    std::vector<KDTSearchResult> results;
    if (!root) return results;
    
    auto queryVec = GetFeatureVector(query);
    float maxDistInResults = std::numeric_limits<float>::max();
    
    SearchKNearestRecursive(root.get(), queryVec, k, results, maxDistInResults);
    
    // Sort by distance
    std::sort(results.begin(), results.end());
    
    return results;
}

KDTSearchResult MotionKDTree::FindBestInClip(const MotionFeatures& query, int animIdx) const {
    KDTSearchResult best;
    best.poseIndex = -1;
    best.distance = std::numeric_limits<float>::max();
    best.score = 0.0f;
    if (!root) return best;

    auto queryVec = GetFeatureVector(query);
    for (size_t i = 0; i < poseData.size(); ++i) {
        if (poseData[i].animationIndex != animIdx) continue;
        auto poseVec = GetFeatureVector(poseData[i]);
        float dist = CalculateDistance(queryVec, poseVec);
        if (dist < best.distance) {
            best.distance = dist;
            best.poseIndex = static_cast<int>(i);
        }
    }
    if (best.poseIndex >= 0) {
        best.score = 1.0f / (1.0f + best.distance);
    }
    return best;
}

std::vector<KDTSearchResult> MotionKDTree::FindWithinRadius(
    const MotionFeatures& query,
    float radius) const {
    
    std::vector<KDTSearchResult> results;
    if (!root) return results;
    
    auto queryVec = GetFeatureVector(query);
    SearchRadiusRecursive(root.get(), queryVec, radius, results);
    
    // Sort by distance
    std::sort(results.begin(), results.end());
    
    return results;
}

// ============================================================================
// SEARCH HELPERS
// ============================================================================

void MotionKDTree::SearchRecursive(
    const KDTreeNode* node,
    const std::vector<float>& query,
    float& bestDist,
    int& bestIndex) const {
    
    if (!node) return;

    // If leaf, check EVERY pose it contains (exact nearest-neighbor search).
    if (node->isLeaf()) {
        for (int pi : node->leafIndices) {
            if (pi < 0 || pi >= static_cast<int>(poseData.size())) continue;
            auto poseVec = GetFeatureVector(poseData[pi]);
            float dist = CalculateDistance(query, poseVec);
            if (dist < bestDist) {
                bestDist = dist;
                bestIndex = pi;
            }
        }
        return;
    }

    // Internal node: check this point
    if (node->poseIndex >= 0 && node->poseIndex < static_cast<int>(poseData.size())) {
        auto poseVec = GetFeatureVector(poseData[node->poseIndex]);
        float dist = CalculateDistance(query, poseVec);

        if (dist < bestDist) {
            bestDist = dist;
            bestIndex = node->poseIndex;
        }
    }
    
    // Determine which subtree to search first
    float queryVal = query[node->splitAxis];
    float delta = queryVal - node->splitValue;

    // Scale-aware noise-margin corridor over the KD-Tree split plane. A flat
    // 0.03f margin fits the speed/velocity axes but is microscopic for the
    // move-angle axis (radians) and the future-path axes (centimeter model
    // units), so the query tripped the split plane during strafing/turning and
    // pruned the correct branch - clip pops on sharp motion. Size the margin to
    // the scale of the active split axis instead.
    float dynamicMargin = 0.03f; // Speed / velocity (m/s)
    if (node->splitAxis == 3) {
        dynamicMargin = 0.15f;   // Move angle (radians, ~8.5 degrees)
    } else if (node->splitAxis >= 7) {
        dynamicMargin = 2.5f;    // Future path (model units, ~cm)
    }

    if (delta < -dynamicMargin) {
        SearchRecursive(node->left.get(), query, bestDist, bestIndex);
        if ((delta * delta) < bestDist) {
            SearchRecursive(node->right.get(), query, bestDist, bestIndex);
        }
    } else if (delta > dynamicMargin) {
        SearchRecursive(node->right.get(), query, bestDist, bestIndex);
        if ((delta * delta) < bestDist) {
            SearchRecursive(node->left.get(), query, bestDist, bestIndex);
        }
    } else {
        // Query lies directly in the noise corridor: explore both subtrees.
        SearchRecursive(node->left.get(), query, bestDist, bestIndex);
        SearchRecursive(node->right.get(), query, bestDist, bestIndex);
    }
}

void MotionKDTree::SearchKNearestRecursive(
    const KDTreeNode* node,
    const std::vector<float>& query,
    int k,
    std::vector<KDTSearchResult>& results,
    float& maxDistInResults) const {
    
    if (!node) return;
    
    // If leaf, evaluate EVERY pose it contains (exact kNN - the old code only
    // looked at a single representative pose per leaf, which made the result
    // set arbitrary and clip-biased).
    if (node->isLeaf()) {
        for (int pi : node->leafIndices) {
            if (pi < 0 || pi >= static_cast<int>(poseData.size())) continue;
            auto poseVec = GetFeatureVector(poseData[pi]);
            float dist = CalculateDistance(query, poseVec);
            
            KDTSearchResult result;
            result.poseIndex = pi;
            result.distance = dist;
            result.score = 1.0f / (1.0f + dist);  // Convert distance to score
            
            // Insert in sorted order
            if (results.size() < static_cast<size_t>(k)) {
                results.push_back(result);
                std::sort(results.begin(), results.end());
                if (results.size() == static_cast<size_t>(k)) {
                    maxDistInResults = results.back().distance;
                }
            } else if (dist < maxDistInResults) {
                results.back() = result;
                std::sort(results.begin(), results.end());
                maxDistInResults = results.back().distance;
            }
        }
        return;
    }
    
    // Internal node: check this point
    if (node->poseIndex >= 0 && node->poseIndex < static_cast<int>(poseData.size())) {
        auto poseVec = GetFeatureVector(poseData[node->poseIndex]);
        float dist = CalculateDistance(query, poseVec);
        
        KDTSearchResult result;
        result.poseIndex = node->poseIndex;
        result.distance = dist;
        result.score = 1.0f / (1.0f + dist);
        
        if (results.size() < static_cast<size_t>(k)) {
            results.push_back(result);
            std::sort(results.begin(), results.end());
            if (results.size() == static_cast<size_t>(k)) {
                maxDistInResults = results.back().distance;
            }
        } else if (dist < maxDistInResults) {
            results.back() = result;
            std::sort(results.begin(), results.end());
            maxDistInResults = results.back().distance;
        }
    }
    
    // Determine search order
    float queryVal = query[node->splitAxis];
    bool goLeft = queryVal < node->splitValue;
    
    if (goLeft) {
        SearchKNearestRecursive(node->left.get(), query, k, results, maxDistInResults);
        
        float distToSplit = std::abs(queryVal - node->splitValue);
        if (results.size() < static_cast<size_t>(k) || distToSplit < maxDistInResults) {
            SearchKNearestRecursive(node->right.get(), query, k, results, maxDistInResults);
        }
    } else {
        SearchKNearestRecursive(node->right.get(), query, k, results, maxDistInResults);
        
        float distToSplit = std::abs(queryVal - node->splitValue);
        if (results.size() < static_cast<size_t>(k) || distToSplit < maxDistInResults) {
            SearchKNearestRecursive(node->left.get(), query, k, results, maxDistInResults);
        }
    }
}

void MotionKDTree::SearchRadiusRecursive(
    const KDTreeNode* node,
    const std::vector<float>& query,
    float radius,
    std::vector<KDTSearchResult>& results) const {
    
    if (!node) return;
    
    // Evaluate every pose in a leaf (the old code checked only the leaf's
    // single representative pose, missing poses that were nearer).
    if (node->isLeaf()) {
        for (int pi : node->leafIndices) {
            if (pi < 0 || pi >= static_cast<int>(poseData.size())) continue;
            auto poseVec = GetFeatureVector(poseData[pi]);
            float dist = CalculateDistance(query, poseVec);
            if (dist <= radius) {
                KDTSearchResult result;
                result.poseIndex = pi;
                result.distance = dist;
                result.score = 1.0f / (1.0f + dist);
                results.push_back(result);
            }
        }
        return;
    }
    
    // Check this point
    if (node->poseIndex >= 0 && node->poseIndex < static_cast<int>(poseData.size())) {
        auto poseVec = GetFeatureVector(poseData[node->poseIndex]);
        float dist = CalculateDistance(query, poseVec);
        
        if (dist <= radius) {
            KDTSearchResult result;
            result.poseIndex = node->poseIndex;
            result.distance = dist;
            result.score = 1.0f / (1.0f + dist);
            results.push_back(result);
        }
    }
    
    // Determine which subtrees to search
    float queryVal = query[node->splitAxis];
    float distToSplit = std::abs(queryVal - node->splitValue);
    
    if (queryVal < node->splitValue) {
        SearchRadiusRecursive(node->left.get(), query, radius, results);
        if (distToSplit <= radius) {
            SearchRadiusRecursive(node->right.get(), query, radius, results);
        }
    } else {
        SearchRadiusRecursive(node->right.get(), query, radius, results);
        if (distToSplit <= radius) {
            SearchRadiusRecursive(node->left.get(), query, radius, results);
        }
    }
}

// ============================================================================
// DISTANCE CALCULATION
// ============================================================================

float MotionKDTree::CalculateDistance(
    const std::vector<float>& a,
    const std::vector<float>& b) const {
    
    if (a.size() != b.size()) return std::numeric_limits<float>::max();
    
    float dist = 0.0f;
    
    // Weighted Euclidean distance
    for (size_t i = 0; i < a.size(); i++) {
        float diff = a[i] - b[i];
        // The move-angle feature (dim 3) is ANGULAR: two poses both pointing
        // "backward" can sit at +pi and -pi (the atan2 sign of a zero X
        // component is arbitrary), which the raw difference scores as 2pi - a
        // full circle - so a straight-backward character ranks backward poses
        // as far. Wrap the difference into [-pi, pi] before weighting.
        if (i == 3) {
            while (diff > 3.14159265f) diff -= 6.28318530f;
            while (diff < -3.14159265f) diff += 6.28318530f;
        }
        float weight = 1.0f;
        
        // Apply weights based on feature type
        if (i == 0) weight = weights.speed;                 // Speed
        else if (i == 1) weight = weights.velocityX;        // Velocity X
        else if (i == 2) weight = weights.velocityZ;        // Velocity Z
        else if (i == 3) weight = weights.direction;        // Direction
        else if (i == 4 || i == 5) weight = weights.footPlant;  // Foot plants
        else if (i == 6) weight = weights.verticalVelocity; // Vertical velocity
        else if (i < 7 + 2 * kTrajectorySteps) {
            // Root-local future path point k (two dims share one weight):
            // nearer points are more predictive than far ones.
            const int point = static_cast<int>((i - 7) / 2);
            weight = trajectoryWeights[point];
        }
        
        dist += (diff * diff) * weight;
    }
    
    return std::sqrt(dist);
}

// ============================================================================
// DEBUG
// ============================================================================

MotionKDTree::TreeStats MotionKDTree::GetStats() const {
    TreeStats stats;
    
    if (!root) return stats;
    
    // BFS to count nodes and measure depth
    std::queue<std::pair<const KDTreeNode*, int>> queue;
    queue.push({root.get(), 0});
    
    while (!queue.empty()) {
        auto [node, depth] = queue.front();
        queue.pop();
        
        stats.totalNodes++;
        stats.maxDepth = std::max(stats.maxDepth, depth);
        
        if (node->isLeaf()) {
            stats.leafNodes++;
        } else {
            stats.internalNodes++;
            if (node->left) queue.push({node->left.get(), depth + 1});
            if (node->right) queue.push({node->right.get(), depth + 1});
        }
    }
    
    if (stats.leafNodes > 0) {
        stats.avgLeafSize = static_cast<float>(poseCount) / stats.leafNodes;
    }
    
    return stats;
}

void MotionKDTree::PrintStructure(int maxDepth) const {
    if (!root) {
        std::cout << "[MotionKDTree] Tree is empty\n";
        return;
    }
    
    std::cout << "\n=== KD-TREE STRUCTURE (max depth=" << maxDepth << ") ===\n";
    
    // BFS with depth limit
    std::queue<std::pair<const KDTreeNode*, int>> queue;
    queue.push({root.get(), 0});
    
    while (!queue.empty()) {
        auto [node, depth] = queue.front();
        queue.pop();
        
        if (depth > maxDepth) continue;
        
        std::string indent(depth * 2, ' ');
        
        if (node->isLeaf()) {
            std::cout << indent << "Leaf: pose=" << node->poseIndex << "\n";
        } else {
            std::cout << indent << "Node: axis=" << node->splitAxis 
                      << " split=" << std::fixed << std::setprecision(2) << node->splitValue
                      << " pose=" << node->poseIndex << "\n";
            
            if (node->left) queue.push({node->left.get(), depth + 1});
            if (node->right) queue.push({node->right.get(), depth + 1});
        }
    }
}
