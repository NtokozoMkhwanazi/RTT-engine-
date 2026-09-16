#pragma once
// ============================================================================
// MotionKDTreeSIMD.h — AVX2-Vectorized SoA Pose Search
// ============================================================================
// Replaces the scalar KD-tree brute-force fallback with an 8-wide SIMD
// distance evaluator. The SoA (Struct-of-Arrays) layout guarantees 32-byte
// aligned contiguous loads into 256-bit registers, processing eight poses
// per iteration instead of one.
//
// Compile with: -mavx2 -mfma (GCC/Clang) or /arch:AVX2 (MSVC)
// Runtime check: cpu SupportsAVX2() before calling ExecuteAVX2PoseSearch.
// ============================================================================

#include <immintrin.h>
#include <vector>
#include <algorithm>   // std::sort
#include <limits>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <new>
#include "MotionMatchingTypes.h"  // kTrajectorySteps

// ---- 32-byte aligned allocator for AVX load/store ---------------------------
template <typename T>
struct AVXAlignedAllocator {
    using value_type = T;
    AVXAlignedAllocator() = default;
    template <typename U>
    constexpr AVXAlignedAllocator(const AVXAlignedAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n == 0) return nullptr;
        void* p = nullptr;
        if (posix_memalign(&p, 32, n * sizeof(T)) != 0 || !p)
            throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept { free(p); }

    template <typename U>
    bool operator==(const AVXAlignedAllocator<U>&) const noexcept { return true; }
    template <typename U>
    bool operator!=(const AVXAlignedAllocator<U>&) const noexcept { return false; }
};

template <typename T>
using SIMDVector = std::vector<T, AVXAlignedAllocator<T>>;

// ---- Runtime AVX2 detection -------------------------------------------------
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,fma")))
#endif
inline bool cpuSupportsAVX2() {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#elif defined(_MSC_VER)
    int info[4];
    __cpuidex(info, 7, 0);
    return (info[1] & (1 << 5)) != 0;
#else
    return false;
#endif
}

// ---- SoA pose database for SIMD ---------------------------------------------
// Must be populated from the existing MotionDatabase after building. The
// data is laid out so each feature channel is contiguous and padded to a
// multiple of 8 for exact register-width loads.
struct SIMDMotionDatabaseSoA {
    SIMDVector<float> speeds;
    SIMDVector<float> velocitiesX;
    SIMDVector<float> velocitiesZ;
    SIMDVector<float> moveAngles;
    SIMDVector<float> leftFootPlanted;
    SIMDVector<float> rightFootPlanted;
    SIMDVector<float> velocitiesY;

    // Trajectory future-path offsets (root-local x,z per step).
    // Maps KD-Tree dims 7..7+2*kTrajectorySteps-1 into contiguous SoA lanes.
    SIMDVector<float> trajectoryX[kTrajectorySteps];
    SIMDVector<float> trajectoryZ[kTrajectorySteps];

    // FIX (v11 Section 4): Interleaved trajectory layout — all 8 trajectory
    // floats for a single pose packed contiguously as [X0,Z0,X1,Z1,X2,Z2,X3,Z3].
    // A single _mm256_load_ps fetches the entire future-path for one pose,
    // replacing the inner kTrajectorySteps loop and eliminating cache-line
    // jumping between 8 separate memory streams. Size = poseCount * 8.
    SIMDVector<float> interleavedTrajectories;

    // Indices mapping SIMD lane back to the original pose index (for the
    // tail when poseCount is not a multiple of 8).
    SIMDVector<int>   originalIndex;

    size_t GetPoseCount() const { return speeds.size(); }

    // Pad all channels to the next multiple of 8.
    void EnforceVectorPadding() {
        const size_t sz = speeds.size();
        const size_t padded = (sz + 7) & ~size_t(7);
        const size_t rem = padded - sz;
        for (size_t i = 0; i < rem; ++i) {
            speeds.push_back(0.0f);
            velocitiesX.push_back(0.0f);
            velocitiesZ.push_back(0.0f);
            moveAngles.push_back(0.0f);
            leftFootPlanted.push_back(0.0f);
            rightFootPlanted.push_back(0.0f);
            velocitiesY.push_back(0.0f);
            originalIndex.push_back(-1);  // sentinel: tail lane
        }
        // Pad trajectory channels too (keeps SIMD _mm256_load_ps aligned)
        for (int k = 0; k < kTrajectorySteps; ++k) {
            for (size_t i = 0; i < rem; ++i) {
                trajectoryX[k].push_back(0.0f);
                trajectoryZ[k].push_back(0.0f);
            }
        }
        // Pad interleaved trajectory block (8 floats per pose)
        for (size_t i = 0; i < rem * 8; ++i) {
            interleavedTrajectories.push_back(0.0f);
        }
    }
};

// ---- Query vector -----------------------------------------------------------
struct SIMDFeaturesQuery {
    float speed;
    float velX;
    float velZ;
    float moveAngle;
    float leftFoot;
    float rightFoot;
    float velY;
    // Trajectory future-path offsets (root-local x,z per step)
    float trajX[kTrajectorySteps];
    float trajZ[kTrajectorySteps];
};

// ---- Weight configuration ---------------------------------------------------
struct SIMDWeightsConfig {
    float speed;
    float velX;
    float velZ;
    float direction;
    float footPlant;
    float velY;
    // Per-step trajectory falloff weights (near→far)
    float trajectoryWeights[kTrajectorySteps];
};

// ---- AVX2 search core -------------------------------------------------------
// Returns the top maxResults pose indices sorted by ascending distance.
// Uses an 8-wide SIMD inner loop with interleaved trajectory loads, collecting
// the best per-chunk result before a final scalar sort to get the top-N.
// This avoids storing all n distances while still using SIMD for the heavy
// feature comparison.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,fma")))
#endif
inline std::vector<std::pair<int, float>> ExecuteAVX2PoseSearch(
    const SIMDMotionDatabaseSoA& db,
    const SIMDFeaturesQuery& query,
    const SIMDWeightsConfig& weights,
    int maxResults = 10)
{
    const size_t totalPoses = db.GetPoseCount();
    std::vector<std::pair<int, float>> results;
    if (totalPoses == 0)
        return results;

    const size_t simdEnd = totalPoses & ~size_t(7);  // round down to multiple of 8

    // Broadcast query scalars across all 8 lanes
    const __m256 qSpeed  = _mm256_set1_ps(query.speed);
    const __m256 qVelX   = _mm256_set1_ps(query.velX);
    const __m256 qVelZ   = _mm256_set1_ps(query.velZ);
    const __m256 qAngle  = _mm256_set1_ps(query.moveAngle);
    const __m256 qFootL  = _mm256_set1_ps(query.leftFoot);
    const __m256 qFootR  = _mm256_set1_ps(query.rightFoot);
    const __m256 qVelY   = _mm256_set1_ps(query.velY);

    // Broadcast weights
    const __m256 wSpeed  = _mm256_set1_ps(weights.speed);
    const __m256 wVelX   = _mm256_set1_ps(weights.velX);
    const __m256 wVelZ   = _mm256_set1_ps(weights.velZ);
    const __m256 wAngle  = _mm256_set1_ps(weights.direction);
    const __m256 wFoot   = _mm256_set1_ps(weights.footPlant);
    const __m256 wVelY   = _mm256_set1_ps(weights.velY);

    // Constants for angle wrapping (branchless via bitmask)
    const __m256 pi      = _mm256_set1_ps(3.14159265f);
    const __m256 twoPi   = _mm256_set1_ps(6.28318530f);
    const __m256 negPi   = _mm256_set1_ps(-3.14159265f);
    // Directional flip penalty (Advanced - feet/footskate guard): clips whose
    // velocity is geometrically OPPOSED to the query win on scalar speed alone
    // (metric-space collapse, e.g. RunLookBack on a forward strafe -> footskate).
    const __m256 zeroVector = _mm256_setzero_ps();
    const __m256 directionalHyperPenalty = _mm256_set1_ps(150.0f);

    // Reserve results buffer for all SIMD poses (upper bound)
    results.reserve(simdEnd + 8);

    // ---- Main SIMD loop: 8 poses per iteration ----------------------------
    for (size_t i = 0; i < simdEnd; i += 8) {
        // 32-byte aligned loads (enforced by EnforceVectorPadding)
        const __m256 dbSpeed = _mm256_load_ps(&db.speeds[i]);
        const __m256 dbVelX  = _mm256_load_ps(&db.velocitiesX[i]);
        const __m256 dbVelZ  = _mm256_load_ps(&db.velocitiesZ[i]);
        const __m256 dbAngle = _mm256_load_ps(&db.moveAngles[i]);
        const __m256 dbFootL = _mm256_load_ps(&db.leftFootPlanted[i]);
        const __m256 dbFootR = _mm256_load_ps(&db.rightFootPlanted[i]);
        const __m256 dbVelY  = _mm256_load_ps(&db.velocitiesY[i]);

        // Speed channel
        __m256 d = _mm256_sub_ps(dbSpeed, qSpeed);
        __m256 score = _mm256_mul_ps(d, d);
        score = _mm256_mul_ps(score, wSpeed);

        // Velocity X (FMA)
        d = _mm256_sub_ps(dbVelX, qVelX);
        score = _mm256_fmadd_ps(_mm256_mul_ps(d, d), wVelX, score);

        // Velocity Z (FMA)
        d = _mm256_sub_ps(dbVelZ, qVelZ);
        score = _mm256_fmadd_ps(_mm256_mul_ps(d, d), wVelZ, score);

        // Move angle — branchless wrapping via bitmask
        d = _mm256_sub_ps(dbAngle, qAngle);
        {
            __m256 above = _mm256_cmp_ps(d, pi, _CMP_GT_OQ);
            d = _mm256_sub_ps(d, _mm256_and_ps(above, twoPi));
            __m256 below = _mm256_cmp_ps(d, negPi, _CMP_LT_OQ);
            d = _mm256_add_ps(d, _mm256_and_ps(below, twoPi));
        }
        score = _mm256_fmadd_ps(_mm256_mul_ps(d, d), wAngle, score);

        // Left foot plant
        d = _mm256_sub_ps(dbFootL, qFootL);
        score = _mm256_fmadd_ps(_mm256_mul_ps(d, d), wFoot, score);

        // Right foot plant
        d = _mm256_sub_ps(dbFootR, qFootR);
        score = _mm256_fmadd_ps(_mm256_mul_ps(d, d), wFoot, score);

        // Vertical velocity
        d = _mm256_sub_ps(dbVelY, qVelY);
        score = _mm256_fmadd_ps(_mm256_mul_ps(d, d), wVelY, score);

        // FIX (v11 Section 4): Interleaved trajectory layout. Primary motion
        // score (including flip penalty) is now computed entirely in SIMD and
        // stored to a per-lane array.  Then each of the 8 poses loads its entire
        // 8-float future-path block [X0,Z0,X1,Z1,X2,Z2,X3,Z3] in ONE
        // _mm256_load_ps, computes the weighted squared-diff trajectory cost
        // in a single register, horizontally reduces it to a scalar, and adds
        // it to the primary score before the Euclidean sqrt.

        // --- Directional flip penalty (keep before trajectory split) ---
        __m256 dotProduct = _mm256_fmadd_ps(dbVelZ, qVelZ,
            _mm256_mul_ps(dbVelX, qVelX));
        __m256 backwardMotionMask = _mm256_cmp_ps(dotProduct, zeroVector, _CMP_LT_OQ);
        score = _mm256_add_ps(score,
            _mm256_and_ps(backwardMotionMask, directionalHyperPenalty));

        // Store primary+flip score for all 8 lanes
        alignas(32) float accumulatedBaseDist[8];
        _mm256_store_ps(accumulatedBaseDist, score);

        // --- Interleaved trajectory evaluation (one load per pose) ---
        // Pack query trajectory into a broadcastable register:
        // [QX0, QZ0, QX1, QZ1, QX2, QZ2, QX3, QZ3]
        alignas(32) const float packedQueryTraj[8] = {
            query.trajX[0], query.trajZ[0],
            query.trajX[1], query.trajZ[1],
            query.trajX[2], query.trajZ[2],
            query.trajX[3], query.trajZ[3]
        };
        const __m256 vQueryTrajectory = _mm256_load_ps(packedQueryTraj);

        // Pack trajectory weights (each weight applied to both X and Z of its step)
        alignas(32) const float packedWeightsTraj[8] = {
            weights.trajectoryWeights[0], weights.trajectoryWeights[0],
            weights.trajectoryWeights[1], weights.trajectoryWeights[1],
            weights.trajectoryWeights[2], weights.trajectoryWeights[2],
            weights.trajectoryWeights[3], weights.trajectoryWeights[3]
        };
        const __m256 vWeightsTrajectory = _mm256_load_ps(packedWeightsTraj);

        // Per-pose trajectory evaluation: 8 loads + 8 subs + 8 reductions
        for (int lane = 0; lane < 8; ++lane) {
            const size_t gi = i + lane;
            if (gi >= totalPoses) break;

            // Single _mm256_load_ps: all 8 trajectory floats for this pose
            const __m256 vPoseTrajectory =
                _mm256_load_ps(&db.interleavedTrajectories[gi * 8]);

            // trajectoryCost = (pose - query)^2 * weights (element-wise)
            const __m256 vDiff = _mm256_sub_ps(vPoseTrajectory, vQueryTrajectory);
            const __m256 vTrajectoryCost =
                _mm256_mul_ps(_mm256_mul_ps(vDiff, vDiff), vWeightsTrajectory);

            // Horizontal reduction: [a,b,c,d,e,f,g,h] → sum of all 8 floats
            __m128 vLow  = _mm256_extractf128_ps(vTrajectoryCost, 0);
            __m128 vHigh = _mm256_extractf128_ps(vTrajectoryCost, 1);
            __m128 vSum  = _mm_add_ps(vLow, vHigh);  // 4 floats summed pairwise
            vSum = _mm_hadd_ps(vSum, vSum);          // [a+b, c+d, a+b, c+d]
            vSum = _mm_hadd_ps(vSum, vSum);          // [a+b+c+d, ..., ..., ...]
            float trajectoryCost;
            _mm_store_ss(&trajectoryCost, vSum);

            // Total Euclidean distance for this pose
            const float totalPoseScore = accumulatedBaseDist[lane] + trajectoryCost;
            const float finalEuclideanDistance = std::sqrt(totalPoseScore);

            results.emplace_back(
                (db.originalIndex[gi] >= 0) ? db.originalIndex[gi] : static_cast<int>(gi),
                finalEuclideanDistance);
        }
    }

    // ---- Scalar tail for non-multiple-of-8 remainder ----------------------
    for (size_t i = simdEnd; i < totalPoses; ++i) {
        float ds = db.speeds[i] - query.speed;
        float dx = db.velocitiesX[i] - query.velX;
        float dz = db.velocitiesZ[i] - query.velZ;
        float da = db.moveAngles[i] - query.moveAngle;
        if (da >  3.14159265f) da -= 6.28318530f;
        if (da < -3.14159265f) da += 6.28318530f;
        float dl = db.leftFootPlanted[i] - query.leftFoot;
        float dr = db.rightFootPlanted[i] - query.rightFoot;
        float dv = db.velocitiesY[i] - query.velY;

        float s  = ds * ds * weights.speed
                 + dx * dx * weights.velX
                 + dz * dz * weights.velZ
                 + da * da * weights.direction
                 + dl * dl * weights.footPlant
                 + dr * dr * weights.footPlant
                 + dv * dv * weights.velY;
        // Trajectory future-path (scalar tail)
        for (int k = 0; k < kTrajectorySteps; ++k) {
            float dtx = db.trajectoryX[k][i] - query.trajX[k];
            float dtz = db.trajectoryZ[k][i] - query.trajZ[k];
            s += (dtx * dtx + dtz * dtz) * weights.trajectoryWeights[k];
        }
        // Directional flip penalty (scalar tail): matches the SIMD loop -
        // demote clips whose velocity is opposed to the query.
        float dotProd = (db.velocitiesX[i] * query.velX) +
                        (db.velocitiesZ[i] * query.velZ);
        float flipPenalty = (dotProd < 0.0f) ? 150.0f : 0.0f;
        s += flipPenalty;
        float dist = std::sqrt(s);
        results.emplace_back(
            (db.originalIndex[i] >= 0) ? db.originalIndex[i] : static_cast<int>(i),
            dist);
    }

    // Partial sort: only sort the top maxResults (O(n log k) instead of O(n log n))
    if (static_cast<int>(results.size()) > maxResults) {
        std::partial_sort(results.begin(), results.begin() + maxResults, results.end(),
              [](const std::pair<int, float>& a, const std::pair<int, float>& b) {
                  return a.second < b.second;
              });
        results.resize(maxResults);
    } else if (!results.empty()) {
        std::sort(results.begin(), results.end(),
              [](const std::pair<int, float>& a, const std::pair<int, float>& b) {
                  return a.second < b.second;
              });
    }
    return results;
}
