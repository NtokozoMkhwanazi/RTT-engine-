#include "MotionKDTree.h"
// Brings in the AVX2 availability macro + cpuSupportsAVX2(), and the shared
// kTrajectorySteps constants - the tree deliberately reuses the project's one
// AVX2 detection rather than rolling its own.
#include "MotionKDTreeSIMD.h"
#include <iostream>
#include <queue>
#include <iomanip>
#include <limits>
#include <cmath>

namespace {
// Process-wide switch that pins leaf evaluation to the scalar reference (see
// MotionKDTree::SetForceScalarLeafEval). Used by tests to A/B the kernels.
bool gForceScalarLeafEval = false;

#if RTT_MOTION_AVX2
// 8-wide weighted-Euclidean distance for one chunk of pose indices, over the
// transposed feature SoA (soa[d * stride + pose]). Arithmetic order matches the
// scalar CalculateDistance loop - accumulate ((diff*diff) * w) dimension by
// dimension - so results agree to within 1 ulp (the only deliberate deviation
// is dim 3's angular wrap; see WrapAngle8 below).
__attribute__((target("avx2,fma")))
inline void LeafDistancesAVX2(const float* soa, size_t stride, int dims,
                              const float* q, const float* w,
                              const int* idx8, float* out8) {
    const __m256i vidx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(idx8));
    __m256 acc = _mm256_setzero_ps();

    for (int d = 0; d < dims; ++d) {
        const __m256 g = _mm256_i32gather_ps(
            soa + static_cast<size_t>(d) * stride, vidx, 4);
        __m256 diff = _mm256_sub_ps(g, _mm256_set1_ps(q[d]));
        if (d == 3) {
            // Angular dimension. The scalar path wraps with fmod(); for the
            // canonicalised feature range (both angles in [0, 2pi), so
            // |diff| < 2pi) the fold below is the same operation:
            //   |d| <= pi -> d, otherwise d -+ 2pi.
            const __m256 absMask =
                _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
            const __m256 pi = _mm256_set1_ps(3.14159265f);
            const __m256 twoPi = _mm256_set1_ps(6.28318530f);
            const __m256 overPi = _mm256_cmp_ps(diff, pi, _CMP_GT_OQ);
            const __m256 belowNegPi = _mm256_cmp_ps(diff, _mm256_sub_ps(_mm256_setzero_ps(), pi),
                                                    _CMP_LT_OQ);
            diff = _mm256_sub_ps(diff, _mm256_and_ps(overPi, twoPi));
            diff = _mm256_add_ps(diff, _mm256_and_ps(belowNegPi, twoPi));
            (void)absMask;
        }
        acc = _mm256_add_ps(acc, _mm256_mul_ps(_mm256_mul_ps(diff, diff),
                                               _mm256_set1_ps(w[d])));
    }
    _mm256_storeu_ps(out8, _mm256_sqrt_ps(acc));
}
#endif  // RTT_MOTION_AVX2
}  // namespace

void MotionKDTree::SetForceScalarLeafEval(bool force) { gForceScalarLeafEval = force; }
bool MotionKDTree::IsForceScalarLeafEval() { return gForceScalarLeafEval; }

MotionKDTree::MotionKDTree() {}

MotionKDTree::~MotionKDTree() {
    Clear();
}

void MotionKDTree::Clear() {
    root.reset();
    poseData.clear();
    poseCount = 0;
    featureSoA_.clear();
    leafDistScratch_.clear();
    leafIdxScratch_.clear();
}

void MotionKDTree::FillDimensionWeights(float out[NUM_FEATURES]) const {
    out[0] = weights.speed;
    out[1] = weights.velocityX;
    out[2] = weights.velocityZ;
    out[3] = weights.direction;
    out[4] = weights.footPlant;
    out[5] = weights.footPlant;
    out[6] = weights.verticalVelocity;
    for (int k = 0; k < kTrajectorySteps; ++k) {
        out[7 + 2 * k] = trajectoryWeights[k];
        out[7 + 2 * k + 1] = trajectoryWeights[k];
    }
}

void MotionKDTree::BuildFeatureSoA() {
    featureSoA_.assign(static_cast<size_t>(NUM_FEATURES) * poseCount, 0.0f);
    for (size_t i = 0; i < poseCount; ++i) {
        const std::vector<float> v = GetFeatureVector(poseData[i]);
        for (int d = 0; d < NUM_FEATURES && d < static_cast<int>(v.size()); ++d) {
            featureSoA_[static_cast<size_t>(d) * poseCount + i] = v[d];
        }
    }
}

std::vector<float> MotionKDTree::GetLeafDistances(
    const MotionFeatures& query, const std::vector<int>& poseIndices) const {
    std::vector<float> out;
    GetLeafDistancesVec(GetFeatureVector(query), poseIndices, out);
    return out;
}

void MotionKDTree::GetLeafDistancesVec(
    const std::vector<float>& qv, const std::vector<int>& poseIndices,
    std::vector<float>& out) const {
    out.assign(poseIndices.size(), 0.0f);
    if (poseIndices.empty()) return;

    const int dims = std::min<int>(NUM_FEATURES, static_cast<int>(qv.size()));

    bool simd = false;
#if RTT_MOTION_AVX2
    simd = !gForceScalarLeafEval && !featureSoA_.empty() &&
           cpuSupportsAVX2() && dims == NUM_FEATURES;
#endif

    if (!simd) {
        for (size_t i = 0; i < poseIndices.size(); ++i) {
            const int pi = poseIndices[i];
            if (pi < 0 || pi >= static_cast<int>(poseData.size())) continue;
            out[i] = CalculateDistance(qv, GetFeatureVector(poseData[pi]));
        }
        return;
    }

#if RTT_MOTION_AVX2
    float wdims[NUM_FEATURES];
    FillDimensionWeights(wdims);

    alignas(32) int idx8[8];
    alignas(32) float out8[8];
    for (size_t i = 0; i < poseIndices.size(); i += 8) {
        const size_t count = std::min<size_t>(8, poseIndices.size() - i);
        for (size_t l = 0; l < 8; ++l) idx8[l] = 0;
        for (size_t l = 0; l < count; ++l) {
            const int pi = poseIndices[i + l];
            idx8[l] = (pi >= 0 && pi < static_cast<int>(poseCount)) ? pi : 0;
        }
        LeafDistancesAVX2(featureSoA_.data(), poseCount, dims, qv.data(), wdims,
                          idx8, out8);
        for (size_t l = 0; l < count; ++l) out[i + l] = out8[l];
    }
#endif
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
            if (bounds.maxBounds[axis] - bounds.minBounds[axis] < 1e-4f) {
                continue; // Guard against zero-variance binary nodes
            }
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
    
    // Safety cap on SAH recursion depth (bail to leaf). See kMaxBuildDepth.
    // Search is exact over leafIndices regardless of depth, so this only
    // bounds build time on degenerate subtrees.
    if (depth >= kMaxBuildDepth) {
        node->leafIndices = indices;
        if (!indices.empty()) {
            node->poseIndex = indices[0];
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
    
    // Transposed feature cache for the AVX2 leaf sweep (also removes the
    // per-pose std::vector allocation the leaf loops used to make).
    BuildFeatureSoA();

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
        // Vectorised leaf sweep: score every pose in the leaf 8-wide (scalar /
        // non-AVX2 falls back inside GetLeafDistancesInto, same values).
        leafIdxScratch_.clear();
        for (int pi : node->leafIndices) {
            if (pi >= 0 && pi < static_cast<int>(poseData.size())) leafIdxScratch_.push_back(pi);
        }
        GetLeafDistancesVec(query, leafIdxScratch_, leafDistScratch_);
        for (size_t i = 0; i < leafIdxScratch_.size(); ++i) {
            const float dist = leafDistScratch_[i];
            if (dist < bestDist) {
                bestDist = dist;
                bestIndex = leafIdxScratch_[i];
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
        // Vectorised leaf sweep (see GetLeafDistancesInto). The bounded top-k
        // insertion stays scalar - only the distance math is 8-wide.
        leafIdxScratch_.clear();
        for (int pi : node->leafIndices) {
            if (pi >= 0 && pi < static_cast<int>(poseData.size())) leafIdxScratch_.push_back(pi);
        }
        GetLeafDistancesVec(query, leafIdxScratch_, leafDistScratch_);
        for (size_t i = 0; i < leafIdxScratch_.size(); ++i) {
            const int pi = leafIdxScratch_[i];
            const float dist = leafDistScratch_[i];

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
        leafIdxScratch_.clear();
        for (int pi : node->leafIndices) {
            if (pi >= 0 && pi < static_cast<int>(poseData.size())) leafIdxScratch_.push_back(pi);
        }
        GetLeafDistancesVec(query, leafIdxScratch_, leafDistScratch_);
        for (size_t i = 0; i < leafIdxScratch_.size(); ++i) {
            const int pi = leafIdxScratch_[i];
            const float dist = leafDistScratch_[i];
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
            diff = std::fmod(diff + 3.14159265f, 6.28318530f);
            if (diff < 0.0f) diff += 6.28318530f;
            diff -= 3.14159265f;
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
