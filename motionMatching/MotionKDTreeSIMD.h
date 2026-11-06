#pragma once
// ============================================================================
// MotionKDTreeSIMD.h — AVX2-vectorised SoA pose search
// ============================================================================
// 8-wide SIMD distance evaluator for the flat (SoA) pose database. The search
// runs the SAME weighted metric as MotionDatabase::CalculatePoseScoreCacheOptimized
// (the scalar reference), just eight poses per iteration:
//
//   score = ‖v − q_v‖ · w_velocity              (3-component length, includes Y)
//         + |speed − q_speed| · w_speed
//         + angularDiff(moveAngle, q_angle) · w_direction
//         + [leftFoot  ≠ q_left ] · w_foot
//         + [rightFoot ≠ q_right] · w_foot
//         + [crouching ≠ q_crouch] · w_crouch
//
// keeping the scalar path as the reference implementation and the non-AVX2
// fallback. Both are exercised by tests/test_motion_simd.cpp, which asserts the
// AVX2 result set matches the scalar one.
//
// TRAJECTORY PARITY: the flat search also scores the Unreal-style root-relative
// future path - the same feature the KD tree indexes as dims 7..7+2*kTrajectorySteps-1
// - using the shared near->far falloff (kDefaultTrajectoryFalloff):
//
//   term = trajectoryWeight · Σ_{k<cmp} w_k·‖Δpath_k‖ / Σ_{k<cmp} w_k
//
// with cmp = min(kTrajectorySteps, query.trajCount) and w_k the falloff. The
// scalar reference (MotionDatabase::CalculateTrajectoryTerm) applies the exact
// same expression, so the two implementations stay in lockstep; both are
// switched off together by MotionDatabase::SetFlatSearchUsesTrajectory(false).
//
// NOTE: `pose.trajectory.positions/velocities` (the WORLD-space path, used by
// Trajectory::getDifference) are never populated for database poses - only
// `trajectory.localPositions` is - so the legacy getDifference term was inert.
// The local path above is the one that actually carries information.
//
// Compile: AVX2/FMA are applied per-function via __attribute__((target(...))),
// so no global -mavx2 is needed and non-x86 builds fall back to scalar.
// ============================================================================

#include <vector>
#include <algorithm>
#include <limits>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <new>
#include <utility>
#include "MotionMatchingTypes.h"

#if defined(__GNUC__) && defined(__x86_64__)
#include <immintrin.h>
#define RTT_MOTION_AVX2 1
#else
#define RTT_MOTION_AVX2 0
#endif

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

// ---- Runtime AVX2 + FMA detection ------------------------------------------
// FMA is required by the kernel, so a CPU with AVX2 but no FMA must take the
// scalar path. Result is cached; __builtin_cpu_supports is a cheap bit test.
inline bool cpuSupportsAVX2() {
#if RTT_MOTION_AVX2
    static const bool has =
        __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    return has;
#elif defined(_MSC_VER)
    int info[4] = {0, 0, 0, 0};
    __cpuidex(info, 7, 0);
    const bool avx2 = (info[1] & (1 << 5)) != 0;
    const bool fma  = (info[1] & (1 << 12)) != 0;
    return avx2 && fma;
#else
    return false;
#endif
}

// ---- SoA pose database for SIMD ---------------------------------------------
// Every channel is kept at the SAME padded length (paddedCount) so a lane
// index is valid in all of them at once. `poseCount` is the real (unpadded)
// database size; the padding lanes at [poseCount, paddedCount) are pure
// SIMD-width filler and are never returned as candidates.
struct SIMDMotionDatabaseSoA {
    SIMDVector<float> speeds;
    SIMDVector<float> velocitiesX;
    SIMDVector<float> velocitiesY;
    SIMDVector<float> velocitiesZ;
    SIMDVector<float> moveAngles;
    SIMDVector<float> leftFootPlanted;
    SIMDVector<float> rightFootPlanted;
    SIMDVector<float> crouching;
    SIMDVector<float> airborne;

    // Root-relative future path, x/z per step. Mirrors the KD tree's dims
    // 7..7+2*kTrajectorySteps-1 so both search paths score the same feature.
    // trajValid is 1 for poses that actually carry a path (localNumPoints > 0)
    // and 0 otherwise, so a pathless pose contributes nothing - exactly like
    // the scalar reference's early-out.
    SIMDVector<float> trajX[kTrajectorySteps];
    SIMDVector<float> trajZ[kTrajectorySteps];
    SIMDVector<float> trajectoryValid;

    // Lane → original pose index (unpadded). Padding lanes hold -1.
    SIMDVector<int> originalIndex;

    size_t poseCount = 0;   // real poses

    size_t GetPoseCount() const { return poseCount; }
    size_t GetPaddedCount() const { return speeds.size(); }

    // Pad every channel to the next multiple of 8 with the SAME value count, so
    // no channel can be short of the SIMD loop's stride (the previous partial
    // padding left the interleaved/trajectory channels unallocated, which the
    // search then read out of bounds).
    void EnforceVectorPadding() {
        const size_t padded = (poseCount + 7) & ~size_t(7);
        speeds.resize(padded, 0.0f);
        velocitiesX.resize(padded, 0.0f);
        velocitiesY.resize(padded, 0.0f);
        velocitiesZ.resize(padded, 0.0f);
        moveAngles.resize(padded, 0.0f);
        leftFootPlanted.resize(padded, 0.0f);
        rightFootPlanted.resize(padded, 0.0f);
        crouching.resize(padded, 0.0f);
        airborne.resize(padded, 0.0f);
        for (int k = 0; k < kTrajectorySteps; ++k) {
            trajX[k].resize(padded, 0.0f);
            trajZ[k].resize(padded, 0.0f);
        }
        trajectoryValid.resize(padded, 0.0f);
        originalIndex.resize(padded, -1);
    }
};

// ---- Query vector -----------------------------------------------------------
struct SIMDFeaturesQuery {
    float speed{0.0f};
    float velX{0.0f};
    float velY{0.0f};
    float velZ{0.0f};
    float moveAngle{0.0f};
    float leftFoot{0.0f};    // 0/1
    float rightFoot{0.0f};   // 0/1
    float crouch{0.0f};      // 0/1

    // Root-relative future path of the query (root-local x,z per step), plus
    // how many of those steps are valid (0 = trajectory feature disabled).
    float trajX[kTrajectorySteps]{0.0f, 0.0f, 0.0f, 0.0f};
    float trajZ[kTrajectorySteps]{0.0f, 0.0f, 0.0f, 0.0f};
    int   trajCount{0};
};

// ---- Weight configuration ---------------------------------------------------
struct SIMDWeightsConfig {
    float speed{0.0f};
    float velocity{0.0f};
    float direction{0.0f};
    float footPlant{0.0f};
    float crouch{0.0f};

    // Overall scale of the trajectory term (0 disables it entirely, in which
    // case the query path is ignored and no trajX/trajZ loads are issued).
    float trajectory{0.0f};
    // Near->far falloff per step (kDefaultTrajectoryFalloff).
    float trajectoryFalloff[kTrajectorySteps]{4.0f, 2.5f, 1.5f, 1.0f};
};

// ---- AVX2 search core -------------------------------------------------------
// Returns the top `maxCandidates` pose indices sorted by ascending score.
// `airborneFilter` mirrors MotionDatabase::SearchCacheOptimized: -1 = any pose,
// 0 = grounded only, 1 = airborne only.
#if RTT_MOTION_AVX2
__attribute__((target("avx2,fma")))
#endif
inline std::vector<std::pair<int, float>> ExecuteAVX2PoseSearch(
    const SIMDMotionDatabaseSoA& db,
    const SIMDFeaturesQuery& query,
    const SIMDWeightsConfig& weights,
    int maxCandidates = 10,
    int airborneFilter = -1)
{
    std::vector<std::pair<int, float>> candidates;
    if (maxCandidates <= 0) return candidates;

    const size_t realCount = db.GetPoseCount();
    const size_t paddedCount = db.GetPaddedCount();
    if (realCount == 0 || paddedCount < realCount) return candidates;

    candidates.reserve(static_cast<size_t>(maxCandidates));

    // Sorted-top-k insertion, byte-for-byte the same rule the scalar reference
    // uses so ties and cut-offs behave identically.
    const auto consider = [&candidates, maxCandidates](int idx, float score) {
        if (candidates.empty() || score < candidates.back().second) {
            const std::pair<int, float> result{idx, score};
            auto it = std::lower_bound(candidates.begin(), candidates.end(), result,
                [](const std::pair<int, float>& a, const std::pair<int, float>& b) {
                    return a.second < b.second;
                });
            candidates.insert(it, result);
            if (static_cast<int>(candidates.size()) > maxCandidates) {
                candidates.pop_back();
            }
        }
    };

    // Movement-state gate: a lane is eligible only when its airborne flag
    // matches the requested state (or filtering is disabled).
    const auto gateOk = [airborneFilter](float airborneVal) {
        if (airborneFilter < 0) return true;
        return (airborneVal != 0.0f) == (airborneFilter == 1);
    };

#if RTT_MOTION_AVX2
    const size_t simdEnd = paddedCount & ~size_t(7);

    const __m256 qVelX = _mm256_set1_ps(query.velX);
    const __m256 qVelY = _mm256_set1_ps(query.velY);
    const __m256 qVelZ = _mm256_set1_ps(query.velZ);
    const __m256 qSpeed = _mm256_set1_ps(query.speed);
    const __m256 qAngle = _mm256_set1_ps(query.moveAngle);
    const __m256 qFootL = _mm256_set1_ps(query.leftFoot);
    const __m256 qFootR = _mm256_set1_ps(query.rightFoot);
    const __m256 qCrouch = _mm256_set1_ps(query.crouch);

    const __m256 wVelocity = _mm256_set1_ps(weights.velocity);
    const __m256 wSpeed = _mm256_set1_ps(weights.speed);
    const __m256 wDirection = _mm256_set1_ps(weights.direction);
    const __m256 wFoot = _mm256_set1_ps(weights.footPlant);
    const __m256 wCrouch = _mm256_set1_ps(weights.crouch);

    const __m256 absMask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
    const __m256 twoPi = _mm256_set1_ps(6.28318530f);

    // Pre-resolve the trajectory term: valid steps and their normalized
    // falloff weights (Σ w_k·‖Δpath_k‖ / Σ w_k · trajectoryWeight), so the hot
    // loop only needs one broadcast + one FMA per step. cmp = 0 (or a zero
    // weight) disables the term without touching memory.
    const int trajCompare = std::max(0, std::min(kTrajectorySteps, query.trajCount));
    float effTrajW[kTrajectorySteps] = {0.0f, 0.0f, 0.0f, 0.0f};
    const bool useTrajectory = (weights.trajectory != 0.0f) && (trajCompare > 0);
    if (useTrajectory) {
        float wSum = 0.0f;
        for (int k = 0; k < trajCompare; ++k) wSum += weights.trajectoryFalloff[k];
        if (wSum > 0.0f) {
            for (int k = 0; k < trajCompare; ++k)
                effTrajW[k] = weights.trajectory * weights.trajectoryFalloff[k] / wSum;
        }
    }

    for (size_t i = 0; i < simdEnd; i += 8) {
        // 32-byte aligned loads (guaranteed by EnforceVectorPadding's aligned
        // allocator + a multiple-of-8 channel length).
        const __m256 vVelX = _mm256_load_ps(&db.velocitiesX[i]);
        const __m256 vVelY = _mm256_load_ps(&db.velocitiesY[i]);
        const __m256 vVelZ = _mm256_load_ps(&db.velocitiesZ[i]);

        // 1. Velocity match: ‖v − q_v‖ · w_velocity  (length, not per-axis).
        const __m256 dx = _mm256_sub_ps(vVelX, qVelX);
        const __m256 dy = _mm256_sub_ps(vVelY, qVelY);
        const __m256 dz = _mm256_sub_ps(vVelZ, qVelZ);
        const __m256 lenSq = _mm256_add_ps(
            _mm256_add_ps(_mm256_mul_ps(dx, dx), _mm256_mul_ps(dy, dy)),
            _mm256_mul_ps(dz, dz));
        __m256 score = _mm256_mul_ps(_mm256_sqrt_ps(lenSq), wVelocity);

        // 2. Speed match: |Δspeed| · w_speed
        const __m256 dSpeed = _mm256_and_ps(
            _mm256_sub_ps(_mm256_load_ps(&db.speeds[i]), qSpeed), absMask);
        score = _mm256_fmadd_ps(dSpeed, wSpeed, score);

        // 3. Direction match, mirroring the scalar fold exactly:
        //    d = |Δangle|; if (d > π) d = 2π − d
        __m256 dAngle = _mm256_and_ps(
            _mm256_sub_ps(_mm256_load_ps(&db.moveAngles[i]), qAngle), absMask);
        const __m256 overPi = _mm256_cmp_ps(dAngle, _mm256_set1_ps(3.14159265f),
                                            _CMP_GT_OQ);
        dAngle = _mm256_blendv_ps(dAngle, _mm256_sub_ps(twoPi, dAngle), overPi);
        score = _mm256_fmadd_ps(dAngle, wDirection, score);

        // 4. Foot-plant mismatch (both flags are exactly 0/1, so the squared
        //    difference is 1 iff they differ - identical to the scalar
        //    `!=` branch, without a branch).
        const __m256 dFootL = _mm256_sub_ps(_mm256_load_ps(&db.leftFootPlanted[i]), qFootL);
        const __m256 dFootR = _mm256_sub_ps(_mm256_load_ps(&db.rightFootPlanted[i]), qFootR);
        score = _mm256_fmadd_ps(_mm256_mul_ps(dFootL, dFootL), wFoot, score);
        score = _mm256_fmadd_ps(_mm256_mul_ps(dFootR, dFootR), wFoot, score);

        // 5. Crouch mismatch.
        const __m256 dCrouch = _mm256_sub_ps(_mm256_load_ps(&db.crouching[i]), qCrouch);
        score = _mm256_fmadd_ps(_mm256_mul_ps(dCrouch, dCrouch), wCrouch, score);

        // 6. Root-relative future path (trajectory parity with the KD tree).
        if (useTrajectory) {
            // Poses with no path (localNumPoints == 0) score 0, matching the
            // scalar reference's early-out rather than comparing against an
            // all-zero path.
            const __m256 valid = _mm256_load_ps(&db.trajectoryValid[i]);
            for (int k = 0; k < trajCompare; ++k) {
                const __m256 dtx = _mm256_sub_ps(_mm256_load_ps(&db.trajX[k][i]),
                                                 _mm256_set1_ps(query.trajX[k]));
                const __m256 dtz = _mm256_sub_ps(_mm256_load_ps(&db.trajZ[k][i]),
                                                 _mm256_set1_ps(query.trajZ[k]));
                const __m256 len = _mm256_sqrt_ps(_mm256_add_ps(_mm256_mul_ps(dtx, dtx),
                                                                _mm256_mul_ps(dtz, dtz)));
                const __m256 wk = _mm256_mul_ps(_mm256_set1_ps(effTrajW[k]), valid);
                score = _mm256_fmadd_ps(len, wk, score);
            }
        }

        alignas(32) float scores[8];
        _mm256_store_ps(scores, score);

        // Scalar bookkeeping for this chunk: the movement-state gate and the
        // bounded top-k insert (unchanged semantics vs the scalar reference).
        const size_t lanes = std::min<size_t>(8, realCount - i);
        for (size_t lane = 0; lane < lanes; ++lane) {
            const size_t gi = i + lane;
            if (!gateOk(db.airborne[gi])) continue;
            const int idx = db.originalIndex[gi];
            consider(idx >= 0 ? idx : static_cast<int>(gi), scores[lane]);
        }
    }
#else
    const size_t simdEnd = 0;
#endif  // RTT_MOTION_AVX2

    // Scalar tail (and the whole search on a non-AVX2 build).
    for (size_t i = simdEnd; i < realCount; ++i) {
        if (!gateOk(db.airborne[i])) continue;
        const float dx = db.velocitiesX[i] - query.velX;
        const float dy = db.velocitiesY[i] - query.velY;
        const float dz = db.velocitiesZ[i] - query.velZ;
        float s = std::sqrt(dx * dx + dy * dy + dz * dz) * weights.velocity;
        s += std::fabs(db.speeds[i] - query.speed) * weights.speed;
        float dAngle = std::fabs(db.moveAngles[i] - query.moveAngle);
        if (dAngle > 3.14159265f) dAngle = 6.28318530f - dAngle;
        s += dAngle * weights.direction;
        if (db.leftFootPlanted[i] != query.leftFoot) s += weights.footPlant;
        if (db.rightFootPlanted[i] != query.rightFoot) s += weights.footPlant;
        if (db.crouching[i] != query.crouch) s += weights.crouch;
        if (useTrajectory && db.trajectoryValid[i] != 0.0f) {
            for (int k = 0; k < trajCompare; ++k) {
                const float dtx = db.trajX[k][i] - query.trajX[k];
                const float dtz = db.trajZ[k][i] - query.trajZ[k];
                s += std::sqrt(dtx * dtx + dtz * dtz) * effTrajW[k];
            }
        }
        const int idx = db.originalIndex[i];
        consider(idx >= 0 ? idx : static_cast<int>(i), s);
    }

    return candidates;
}
