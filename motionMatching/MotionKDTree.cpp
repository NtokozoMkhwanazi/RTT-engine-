#include "MotionKDTree.h"
#include <iostream>
#include <queue>
#include <iomanip>
#include <limits>

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
    return {
        pose.features.speed,                          // 0: Speed (0-8 m/s)
        pose.features.rootVelocity.x,                 // 1: X velocity
        pose.features.rootVelocity.z,                 // 2: Z velocity
        pose.features.moveAngle,                      // 3: Movement direction
        pose.features.leftFootPlanted ? 1.0f : 0.0f,  // 4: Left foot planted
        pose.features.rightFootPlanted ? 1.0f : 0.0f  // 5: Right foot planted
    };
}

std::vector<float> MotionKDTree::GetFeatureVector(const MotionFeatures& features) const {
    return {
        features.speed,
        features.rootVelocity.x,
        features.rootVelocity.z,
        features.moveAngle,
        features.leftFootPlanted ? 1.0f : 0.0f,
        features.rightFootPlanted ? 1.0f : 0.0f
    };
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
        // Store all indices in this leaf (we'll just use the first one for simplicity)
        if (!indices.empty()) {
            node->poseIndex = indices[0];
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
        if (!indices.empty()) {
            node->poseIndex = indices[0];
        }
        return node;
    }
    
    // Find best split using SAH with binning
    SAHSplit split = FindBestSplit(indices, maxLeafSize);
    
    // If no valid split found, make this a leaf
    if (split.bestAxis < 0) {
        if (!indices.empty()) {
            node->poseIndex = indices[0];
        }
        return node;
    }
    
    // Set split parameters
    node->splitAxis = split.bestAxis;
    float axisMin = bounds.minBounds[split.bestAxis];
    float axisMax = bounds.maxBounds[split.bestAxis];
    float binWidth = (axisMax - axisMin) / numBins;
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
    
    // Compute child bounds
    BinBounds leftBounds = bounds;
    BinBounds rightBounds = bounds;
    leftBounds.maxBounds[split.bestAxis] = node->splitValue;
    rightBounds.minBounds[split.bestAxis] = node->splitValue;
    
    // Build subtrees
    if (!leftIndices.empty()) {
        node->left = BuildWithSAHRecursive(leftIndices, depth + 1, maxLeafSize, leftBounds, numBins);
    }
    if (!rightIndices.empty()) {
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

    // If leaf, check distance
    if (node->isLeaf()) {
        if (node->poseIndex >= 0 && node->poseIndex < static_cast<int>(poseData.size())) {
            auto poseVec = GetFeatureVector(poseData[node->poseIndex]);
            float dist = CalculateDistance(query, poseVec);

            if (dist < bestDist) {
                bestDist = dist;
                bestIndex = node->poseIndex;
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
    bool goLeft = queryVal < node->splitValue;
    
    // Search closer subtree first
    if (goLeft) {
        SearchRecursive(node->left.get(), query, bestDist, bestIndex);
        
        // Check if we need to search the other subtree
        float distToSplit = std::abs(queryVal - node->splitValue);
        if (distToSplit < bestDist) {
            SearchRecursive(node->right.get(), query, bestDist, bestIndex);
        }
    } else {
        SearchRecursive(node->right.get(), query, bestDist, bestIndex);
        
        float distToSplit = std::abs(queryVal - node->splitValue);
        if (distToSplit < bestDist) {
            SearchRecursive(node->left.get(), query, bestDist, bestIndex);
        }
    }
}

void MotionKDTree::SearchKNearestRecursive(
    const KDTreeNode* node,
    const std::vector<float>& query,
    int k,
    std::vector<KDTSearchResult>& results,
    float& maxDistInResults) const {
    
    if (!node) return;
    
    // If leaf, add to results
    if (node->isLeaf()) {
        if (node->poseIndex >= 0 && node->poseIndex < static_cast<int>(poseData.size())) {
            auto poseVec = GetFeatureVector(poseData[node->poseIndex]);
            float dist = CalculateDistance(query, poseVec);
            
            KDTSearchResult result;
            result.poseIndex = node->poseIndex;
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
        float weight = 1.0f;
        
        // Apply weights based on feature type
        if (i == 0) weight = weights.speed;        // Speed
        else if (i == 1) weight = weights.velocityX;  // Velocity X
        else if (i == 2) weight = weights.velocityZ;  // Velocity Z
        else if (i == 3) weight = weights.direction;  // Direction
        else if (i >= 4) weight = weights.footPlant;  // Foot plant
        
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
