#include "TransitionClipGenerator.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <limits>
#include "../boneSystem/BoneName.h"

// AVX2 intrinsics for the windowed point-cloud accumulation passes
// (ComputeOptimal2DAlignment / the weighted residual in
// WindowPointCloudDistance). Guarded so the header stays portable on non-x86;
// every AVX2 entry point has a scalar twin and a runtime dispatcher. Same
// pattern as animationSystem/Skinning.h.
#if defined(__GNUC__) && defined(__x86_64__)
#include <immintrin.h>
#define RTT_KG_AVX2 1
#else
#define RTT_KG_AVX2 0
#endif

namespace {

// One window of point-cloud samples in Struct-of-Arrays form: 32-byte-friendly
// contiguous x/y/z lanes (an AoS glm::vec3 stream is 12-byte strided and can't
// be fed to a 256-bit load). Built once per window, consumed by both the
// alignment solve and the residual pass.
struct CloudSoA {
    std::vector<float> x, y, z;
};

inline CloudSoA ToSoA(const std::vector<glm::vec3>& pts) {
    CloudSoA out;
    out.x.resize(pts.size());
    out.y.resize(pts.size());
    out.z.resize(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) {
        out.x[i] = pts[i].x;
        out.y[i] = pts[i].y;
        out.z[i] = pts[i].z;
    }
    return out;
}

// Weighted cross/dot sums for the Kovar & Gleicher closed-form 2D alignment
// (equations 2-4). Shared scalar reference - the AVX2 path is a 8-wide twin of
// this exact arithmetic.
//   out[0]=Σw  out[1]=Σw(xᵢx₀ᵢ+zᵢz₀ᵢ)  out[2]=Σw(xᵢz₀ᵢ−x₀ᵢzᵢ)
//   out[3]=Σw xᵢ  out[4]=Σw zᵢ  out[5]=Σw x₀ᵢ  out[6]=Σw z₀ᵢ
// Hips-relative leg reach (|hips -> foot|) measured straight out of a sampled
// point cloud. SampleBonePositionsAtPose re-centres every bone on the hips, so
// the LENGTH of a foot bone's entry is the character's hip-to-foot reach in
// model units. Returns 0 when no foot bone can be resolved.
inline float LegReachFromCloud(const std::vector<glm::vec3>& pts,
                               const Skeleton* skeleton) {
    if (!skeleton || pts.empty()) return 0.0f;
    static const char* kFootNames[] = {
        "leftfoot", "LeftFoot", "mixamorig:LeftFoot", "mixamorig:leftfoot",
        "rightfoot", "RightFoot", "mixamorig:RightFoot", "mixamorig:rightfoot"
    };
    float best = 0.0f;
    for (const char* n : kFootNames) {
        const int idx = skeleton->GetBoneIndex(n);
        if (idx >= 0 && idx < static_cast<int>(pts.size())) {
            best = std::max(best, glm::length(pts[idx]));
        }
    }
    return best;
}

// Scale factor that re-expresses the TARGET pack's point cloud in the SOURCE
// pack's units. Returns exactly 1.0 (a no-op) when the two reaches agree within
// 2% - i.e. for every single-skeleton setup - so adding this cannot silently
// retune the existing distance gates. Degenerate/degenerate reach is also 1.0.
inline float ComputePackScale(float reachSrc, float reachTgt) {
    if (reachSrc <= 1e-3f || reachTgt <= 1e-3f) return 1.0f;
    float scale = reachSrc / reachTgt;
    scale = std::max(0.5f, std::min(2.0f, scale));
    if (std::fabs(scale - 1.0f) < 0.02f) return 1.0f;
    return scale;
}

inline void AccumulateAlignmentSumsScalar(const float* sx, const float* sz,
                                          const float* tx, const float* tz,
                                          const float* w, size_t n, float out[7]) {
    for (int k = 0; k < 7; ++k) out[k] = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float wi = w[i];
        const float ax = sx[i], az = sz[i];
        const float bx = tx[i], bz = tz[i];
        out[0] += wi;
        out[1] += wi * (ax * bx + az * bz);
        out[2] += wi * (ax * bz - bx * az);
        out[3] += wi * ax;
        out[4] += wi * az;
        out[5] += wi * bx;
        out[6] += wi * bz;
    }
}

#if RTT_KG_AVX2
__attribute__((target("avx2,fma")))
inline void AccumulateAlignmentSumsAVX2(const float* sx, const float* sz,
                                        const float* tx, const float* tz,
                                        const float* w, size_t n, float out[7]) {
    __m256 aW = _mm256_setzero_ps(), aXX0 = _mm256_setzero_ps();
    __m256 aXZ0 = _mm256_setzero_ps(), aX = _mm256_setzero_ps();
    __m256 aZ = _mm256_setzero_ps(), aX0 = _mm256_setzero_ps();
    __m256 aZ0 = _mm256_setzero_ps();

    const size_t simdEnd = n & ~size_t(7);
    for (size_t i = 0; i < simdEnd; i += 8) {
        const __m256 vW = _mm256_loadu_ps(w + i);
        const __m256 vAX = _mm256_loadu_ps(sx + i);
        const __m256 vAZ = _mm256_loadu_ps(sz + i);
        const __m256 vBX = _mm256_loadu_ps(tx + i);
        const __m256 vBZ = _mm256_loadu_ps(tz + i);

        aW = _mm256_add_ps(aW, vW);
        aXX0 = _mm256_fmadd_ps(vW, _mm256_add_ps(_mm256_mul_ps(vAX, vBX),
                                                 _mm256_mul_ps(vAZ, vBZ)), aXX0);
        aXZ0 = _mm256_fmadd_ps(vW, _mm256_sub_ps(_mm256_mul_ps(vAX, vBZ),
                                                 _mm256_mul_ps(vBX, vAZ)), aXZ0);
        aX  = _mm256_fmadd_ps(vW, vAX, aX);
        aZ  = _mm256_fmadd_ps(vW, vAZ, aZ);
        aX0 = _mm256_fmadd_ps(vW, vBX, aX0);
        aZ0 = _mm256_fmadd_ps(vW, vBZ, aZ0);
    }

    alignas(32) float t0[8], t1[8], t2[8], t3[8], t4[8], t5[8], t6[8];
    _mm256_store_ps(t0, aW);   _mm256_store_ps(t1, aXX0); _mm256_store_ps(t2, aXZ0);
    _mm256_store_ps(t3, aX);   _mm256_store_ps(t4, aZ);   _mm256_store_ps(t5, aX0);
    _mm256_store_ps(t6, aZ0);
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0;
    for (int k = 0; k < 8; ++k) {
        s0 += t0[k]; s1 += t1[k]; s2 += t2[k]; s3 += t3[k];
        s4 += t4[k]; s5 += t5[k]; s6 += t6[k];
    }

    // Scalar tail for the remainder lanes.
    for (size_t i = simdEnd; i < n; ++i) {
        const float wi = w[i];
        const float ax = sx[i], az = sz[i];
        const float bx = tx[i], bz = tz[i];
        s0 += wi;
        s1 += wi * (ax * bx + az * bz);
        s2 += wi * (ax * bz - bx * az);
        s3 += wi * ax;
        s4 += wi * az;
        s5 += wi * bx;
        s6 += wi * bz;
    }

    out[0] = s0; out[1] = s1; out[2] = s2; out[3] = s3;
    out[4] = s4; out[5] = s5; out[6] = s6;
}
#endif  // RTT_KG_AVX2

// FMA is used by both kernels, so the runtime gate must cover it as well
// (an AVX2-but-no-FMA CPU would still take the scalar path).
inline bool KgHasAvx2Fma() {
#if RTT_KG_AVX2
    static const bool has = __builtin_cpu_supports("avx2") &&
                            __builtin_cpu_supports("fma");
    return has;
#else
    return false;
#endif
}

inline void AccumulateAlignmentSums(const float* sx, const float* sz,
                                    const float* tx, const float* tz,
                                    const float* w, size_t n, float out[7]) {
#if RTT_KG_AVX2
    if (KgHasAvx2Fma()) {
        AccumulateAlignmentSumsAVX2(sx, sz, tx, tz, w, n, out);
        return;
    }
#endif
    AccumulateAlignmentSumsScalar(sx, sz, tx, tz, w, n, out);
}

// Σ wᵢ‖aᵢ − (R(θ)·bᵢ + t)‖² and Σ wᵢ over a window, with R the same rotation
// convention as Apply2DTransform: R(x,z) = (c·x + s·z, −s·x + c·z).
inline float WeightedResidualScalar(const CloudSoA& a, const CloudSoA& b,
                                    const std::vector<float>& w,
                                    float c, float sn, float trx, float trz,
                                    float& outTotalW) {
    float totalDist = 0.0f, totalW = 0.0f;
    for (size_t i = 0; i < w.size(); ++i) {
        const float bx = b.x[i], by = b.y[i], bz = b.z[i];
        const float rx = c * bx + sn * bz + trx;
        const float rz = -sn * bx + c * bz + trz;
        const float dx = a.x[i] - rx;
        const float dy = a.y[i] - by;
        const float dz = a.z[i] - rz;
        const float d2 = dx * dx + dy * dy + dz * dz;
        totalDist += w[i] * d2;
        totalW += w[i];
    }
    outTotalW = totalW;
    return totalW > 0.0f ? totalDist / totalW : std::numeric_limits<float>::max();
}

#if RTT_KG_AVX2
__attribute__((target("avx2,fma")))
inline float WeightedResidualAVX2(const CloudSoA& a, const CloudSoA& b,
                                  const std::vector<float>& w,
                                  float c, float sn, float trx, float trz,
                                  float& outTotalW) {
    const __m256 vC = _mm256_set1_ps(c);
    const __m256 vS = _mm256_set1_ps(sn);
    const __m256 vTx = _mm256_set1_ps(trx);
    const __m256 vTz = _mm256_set1_ps(trz);
    __m256 aDist = _mm256_setzero_ps(), aW = _mm256_setzero_ps();

    const size_t n = w.size();
    const size_t simdEnd = n & ~size_t(7);
    for (size_t i = 0; i < simdEnd; i += 8) {
        const __m256 bx = _mm256_loadu_ps(b.x.data() + i);
        const __m256 by = _mm256_loadu_ps(b.y.data() + i);
        const __m256 bz = _mm256_loadu_ps(b.z.data() + i);

        // rotated target: (c·x + s·z + tx, −s·x + c·z + tz)
        const __m256 rotX = _mm256_add_ps(
            _mm256_fmadd_ps(vC, bx, _mm256_mul_ps(vS, bz)), vTx);
        const __m256 rotZ = _mm256_add_ps(
            _mm256_fmadd_ps(vC, bz, _mm256_mul_ps(_mm256_sub_ps(_mm256_setzero_ps(), vS), bx)), vTz);

        const __m256 dx = _mm256_sub_ps(_mm256_loadu_ps(a.x.data() + i), rotX);
        const __m256 dy = _mm256_sub_ps(_mm256_loadu_ps(a.y.data() + i), by);
        const __m256 dz = _mm256_sub_ps(_mm256_loadu_ps(a.z.data() + i), rotZ);

        const __m256 d2 = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(dx, dx),
                                                      _mm256_mul_ps(dy, dy)),
                                        _mm256_mul_ps(dz, dz));
        const __m256 vW = _mm256_loadu_ps(w.data() + i);
        aDist = _mm256_fmadd_ps(vW, d2, aDist);
        aW = _mm256_add_ps(aW, vW);
    }

    alignas(32) float td[8], tw[8];
    _mm256_store_ps(td, aDist);
    _mm256_store_ps(tw, aW);
    float totalDist = 0.0f, totalW = 0.0f;
    for (int k = 0; k < 8; ++k) {
        totalDist += td[k];
        totalW += tw[k];
    }

    for (size_t i = simdEnd; i < n; ++i) {
        const float bx = b.x[i], by = b.y[i], bz = b.z[i];
        const float rx = c * bx + sn * bz + trx;
        const float rz = -sn * bx + c * bz + trz;
        const float dx = a.x[i] - rx;
        const float dy = a.y[i] - by;
        const float dz = a.z[i] - rz;
        totalDist += w[i] * (dx * dx + dy * dy + dz * dz);
        totalW += w[i];
    }

    outTotalW = totalW;
    return totalW > 0.0f ? totalDist / totalW : std::numeric_limits<float>::max();
}
#endif  // RTT_KG_AVX2

inline float WeightedResidual(const CloudSoA& a, const CloudSoA& b,
                             const std::vector<float>& w,
                             float c, float sn, float trx, float trz,
                             float& outTotalW) {
#if RTT_KG_AVX2
    if (KgHasAvx2Fma()) {
        return WeightedResidualAVX2(a, b, w, c, sn, trx, trz, outTotalW);
    }
#endif
    return WeightedResidualScalar(a, b, w, c, sn, trx, trz, outTotalW);
}

}  // namespace

// ============================================================================
// Implementation of the motion-graph transition pre-computation.
//
// This replaces the runtime patch approach (stature offsets, pose snapshot
// blending, hardcoded height factors) with pre-computed transition clips
// built at load time using the Kovar & Gleicher (SIGGRAPH 2002) method.
// ============================================================================

float TransitionClipGenerator::SmoothstepC1(float p, float k) {
    // Equation 7: α(p) = 2((p+1)/k)³ − 3((p+1)/k)² + 1
    // C¹-continuous: α(−1)=1, α(k)=0, α'(−1)=α'(k)=0
    if (p <= -1.0f) return 1.0f;
    if (p >= k)     return 0.0f;
    float t = (p + 1.0f) / k;
    return 2.0f * t * t * t - 3.0f * t * t + 1.0f;
}

glm::vec3 TransitionClipGenerator::Apply2DTransform(
    const glm::vec3& point,
    float rotationRad,
    const glm::vec2& translation) {
    // Rotate about Y axis (equation 1: T rotates about vertical axis)
    float c = std::cos(rotationRad);
    float s = std::sin(rotationRad);
    glm::vec3 rotated;
    rotated.x = c * point.x + s * point.z;
    rotated.y = point.y;  // Y unchanged (vertical axis)
    rotated.z = -s * point.x + c * point.z;
    return rotated + glm::vec3(translation.x, 0.0f, translation.y);
}

// Internal, SoA-parametric form of the alignment solve. ComputeOptimal2DAlignment
// (the public, vector<glm::vec3> entry point) and WindowPointCloudDistance both
// funnel through this so the closed-form solution exists in exactly one place.
static void ComputeOptimal2DAlignmentSoA(
    const CloudSoA& src, const CloudSoA& tgt, const std::vector<float>& weights,
    float& outRotation, glm::vec2& outTranslation) {

    // We want the rigid transform T(p) = R(θ)·p + t that best maps the TARGET
    // cloud onto the SOURCE cloud:
    //     minimize  Σ wᵢ ‖aᵢ − (R(θ)·bᵢ + t)‖²
    // with R(θ)·(x,z) = (c·x + s·z, −s·x + c·z)   (Apply2DTransform's convention).
    //
    // Weighted centroids, then a closed-form θ:
    //     t = ā − R(θ)·b̄
    //     θ = atan2( Σ wᵢ(ãᵢ_x b̃ᵢ_z − ãᵢ_z b̃ᵢ_x), Σ wᵢ(ãᵢ_x b̃ᵢ_x + ãᵢ_z b̃ᵢ_z) )
    // where ã = a − ā, b̃ = b − b̄ (Kovar & Gleicher equations 2-4).
    //
    // The accumulate step below computes the raw (un-centered) sums; the
    // centroid terms are subtracted out afterwards, which is algebraically
    // identical to summing the centered products.
    const size_t n = src.x.size();
    float sums[7] = {0.0f};
    AccumulateAlignmentSums(src.x.data(), src.z.data(), tgt.x.data(),
                            tgt.z.data(), weights.data(), n, sums);

    const float sumW = sums[0];
    if (sumW < 1e-8f) {
        outRotation = 0.0f;
        outTranslation = {0.0f, 0.0f};
        return;
    }

    const float xBar  = sums[3] / sumW;   // source centroid x
    const float zBar  = sums[4] / sumW;   // source centroid z
    const float x0Bar = sums[5] / sumW;   // target centroid x
    const float z0Bar = sums[6] / sumW;   // target centroid z

    // Σ wᵢ(xᵢ z₀ᵢ − x₀ᵢ zᵢ) − (x̄ z̄₀ − x̄₀ z̄)Σwᵢ  ==  Σ wᵢ(ãᵢ_x b̃ᵢ_z − ãᵢ_z b̃ᵢ_x)
    const float numerator   = sums[2] - (xBar * z0Bar - x0Bar * zBar) * sumW;
    // Σ wᵢ(xᵢ x₀ᵢ + zᵢ z₀ᵢ) − (x̄ x̄₀ + z̄ z̄₀)Σwᵢ  ==  Σ wᵢ(ãᵢ_x b̃ᵢ_x + ãᵢ_z b̃ᵢ_z)
    const float denominator = sums[1] - (xBar * x0Bar + zBar * z0Bar) * sumW;

    outRotation = std::atan2(numerator, denominator);

    const float c = std::cos(outRotation);
    const float s = std::sin(outRotation);

    // Equation (3)-(4): align the centroids. The translation is the residual
    // between the source centroid and the ROTATED target centroid, i.e.
    //     t = ā − R(θ)·b̄
    // (Note the direction: the transform maps target → source, matching how
    // WindowPointCloudDistance consumes it. Computing b̄ − R⁻¹(θ)·ā here - i.e.
    // rotating with the opposite sign and swapping the centroids - inverts the
    // translation, which at θ = 0 collapses to a sign flip and makes a pure
    // translation of d score as 2d.)
    outTranslation.x = xBar - (c * x0Bar + s * z0Bar);
    outTranslation.y = zBar - (-s * x0Bar + c * z0Bar);
}

// Public vector<glm::vec3> entry point (also the unit-test surface).
void TransitionClipGenerator::ComputeOptimal2DAlignment(
    const std::vector<glm::vec3>& sourcePoints,
    const std::vector<glm::vec3>& targetPoints,
    const std::vector<float>& weights,
    float& outRotation,
    glm::vec2& outTranslation) {

    if (sourcePoints.empty() || sourcePoints.size() != targetPoints.size()) {
        outRotation = 0.0f;
        outTranslation = {0.0f, 0.0f};
        return;
    }

    std::vector<float> fullWeights = weights;
    fullWeights.resize(sourcePoints.size(), 1.0f);

    ComputeOptimal2DAlignmentSoA(ToSoA(sourcePoints), ToSoA(targetPoints),
                                 fullWeights, outRotation, outTranslation);
}

std::vector<glm::vec3> TransitionClipGenerator::SampleBonePositionsAtPose(
    const MotionDatabase& db,
    int poseIndex,
    const Skeleton* skeleton) {

    std::vector<glm::vec3> positions;
    if (poseIndex < 0 || poseIndex >= (int)db.GetPoseCount() || !skeleton) {
        return positions;
    }

    const PoseSample& pose = db.GetPose(poseIndex);
    auto anim = db.GetAnimation(pose.animationIndex);
    if (!anim) return positions;

    int numBones = (int)skeleton->bones.size();
    positions.resize(numBones, glm::vec3(0.0f));

    // Pre-build a reverse lookup: bone index -> bone name.
    // The old code did an O(N) scan through boneMapping for EACH bone, making
    // SampleBonePositionsAtPose O(N²) per call. With 40 window frames * 2 calls
    // * 200*200 pose pairs, this was ~32 billion operations → hang.
    // Now it's O(N) once per call.
    std::vector<std::string> boneNamesByIndex(numBones);
    for (const auto& [name, idx] : skeleton->boneMapping) {
        if (idx >= 0 && idx < numBones) {
            boneNamesByIndex[idx] = name; // Kept raw for GetBoneAnimation mappings
        }
    }

    // CRITICAL CORRECTION: Relativize point clouds to the Hips bone.
    // Mixamo animations bake motion onto the Hips bone while keeping the
    // Root bone static, so the character's coordinate frame drifts relative
    // to the absolute origin. Normalizing around the Hips removes this drift
    // so windowed point-cloud distance comparisons are between poses in the
    // same local frame.
    glm::vec3 hipsOffset(0.0f);
    for (int b = 0; b < numBones; ++b) {
        if (boneNamesByIndex[b].empty()) continue;
        std::string normName = NormalizeBoneName(boneNamesByIndex[b]);
        if (normName == "hips") {
            const BoneAnimation* hipsAnim = anim->GetBoneAnimation(boneNamesByIndex[b]);
            if (hipsAnim) {
                hipsOffset = hipsAnim->InterpolatePosition(pose.timeInSeconds);
                hipsOffset.y = 0.0f; // Maintain vertical height continuity
            }
            break;
        }
    }

    // Sample root-relative positions for each bone
    float t = pose.timeInSeconds;
    for (int b = 0; b < numBones; ++b) {
        const std::string& boneName = boneNamesByIndex[b];
        if (boneName.empty()) continue;

        const BoneAnimation* boneAnim = anim->GetBoneAnimation(boneName);
        if (boneAnim) {
            // Subtract hipsOffset to align point clouds relative to the
            // character hub (hips), eliminating Mixamo root-frame drift.
            positions[b] = boneAnim->InterpolatePosition(t) - hipsOffset;
        }
    }

    return positions;
}

void TransitionClipGenerator::SampleBoneTRS(
    const Animation& anim,
    float time,
    const Skeleton* skeleton,
    std::vector<glm::vec3>& outPositions,
    std::vector<glm::quat>& outRotations,
    std::vector<glm::vec3>& outScales) {

    int numBones = (int)skeleton->bones.size();
    outPositions.resize(numBones, glm::vec3(0.0f));
    outRotations.resize(numBones, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    outScales.resize(numBones, glm::vec3(1.0f));

    // Pre-build reverse lookup: bone index -> bone name (same optimization
    // as SampleBonePositionsAtPose — avoids O(N²) boneMapping scan).
    std::vector<std::string> boneNamesByIndex(numBones);
    for (const auto& [name, idx] : skeleton->boneMapping) {
        if (idx >= 0 && idx < numBones) {
            boneNamesByIndex[idx] = name;
        }
    }

    for (int b = 0; b < numBones; ++b) {
        const std::string& boneName = boneNamesByIndex[b];
        if (boneName.empty()) continue;

        const BoneAnimation* boneAnim = anim.GetBoneAnimation(boneName);
        if (boneAnim) {
            outPositions[b] = boneAnim->InterpolatePosition(time);
            outRotations[b] = boneAnim->InterpolateRotation(time);
            outScales[b] = boneAnim->InterpolateScale(time);
        }
    }
}

float TransitionClipGenerator::WindowPointCloudDistance(
    const MotionDatabase& sourceDB,
    const MotionDatabase& targetDB,
    int sourcePoseIdx,
    int targetPoseIdx,
    int windowSize,
    const Skeleton* skeleton,
    float& outRotation,
    glm::vec2& outTranslation) {

    if (sourcePoseIdx < 0 || targetPoseIdx < 0) {
        outRotation = 0.0f;
        outTranslation = {0.0f, 0.0f};
        return std::numeric_limits<float>::max();
    }

    // Build window of points around source pose. Stored directly in SoA form so
    // the two accumulation passes below are contiguous 8-wide streams (an AoS
    // glm::vec3 window is 12-byte strided and can't be vector-loaded).
    CloudSoA sourceCloud;
    CloudSoA targetCloud;
    std::vector<float> weights;

    // Centre-frame samples, kept for the cross-pack scale normalisation below.
    std::vector<glm::vec3> sourceCentre, targetCentre;

    int halfWindow = windowSize / 2;
    // Taper has halfWindow as its denominator, so a window of 0/1 frames would
    // divide by zero; treat it as an un-tapered (weight 1) window.
    const float taperDenom = (halfWindow > 0) ? static_cast<float>(halfWindow) : 1.0f;

    for (int offset = -halfWindow; offset <= halfWindow; ++offset) {
        int sIdx = sourcePoseIdx + offset;
        int tIdx = targetPoseIdx + offset;

        // Skip out-of-range poses (clamps to nearest)
        sIdx = std::max(0, std::min(sIdx, (int)sourceDB.GetPoseCount() - 1));
        tIdx = std::max(0, std::min(tIdx, (int)targetDB.GetPoseCount() - 1));

        auto sPoints = SampleBonePositionsAtPose(sourceDB, sIdx, skeleton);
        auto tPoints = SampleBonePositionsAtPose(targetDB, tIdx, skeleton);

        if (sPoints.size() != tPoints.size() || sPoints.empty()) continue;
        if (offset == 0) {
            sourceCentre = sPoints;
            targetCentre = tPoints;
        }

        // Weight: taper towards window edges (paper §3.1: "wi may be chosen
        // to taper off towards the end of the window")
        float w = 1.0f - std::abs((float)offset / taperDenom);

        for (size_t b = 0; b < sPoints.size(); ++b) {
            sourceCloud.x.push_back(sPoints[b].x);
            sourceCloud.y.push_back(sPoints[b].y);
            sourceCloud.z.push_back(sPoints[b].z);
            targetCloud.x.push_back(tPoints[b].x);
            targetCloud.y.push_back(tPoints[b].y);
            targetCloud.z.push_back(tPoints[b].z);

            // Higher weight for root/hips, lower for extremities
            float boneW = w;
            if (b == 0) boneW = w * 2.0f;  // root bone double weight
            weights.push_back(boneW);
        }
    }

    if (sourceCloud.x.empty()) {
        outRotation = 0.0f;
        outTranslation = {0.0f, 0.0f};
        return std::numeric_limits<float>::max();
    }

    // Cross-pack scale normalisation. Different Mixamo packs can author the same
    // motion at a different character scale, which scales the whole point cloud
    // and inflates the residual for an otherwise perfect match. Express the
    // TARGET in the SOURCE's units via the measured hips->foot leg reach. When
    // the two agree (the single-skeleton case) this is exactly 1.0, so the
    // tuned thresholds keep their meaning.
    const float targetScale = ComputePackScale(
        LegReachFromCloud(sourceCentre, skeleton),
        LegReachFromCloud(targetCentre, skeleton));
    if (targetScale != 1.0f) {
        for (size_t i = 0; i < targetCloud.x.size(); ++i) {
            targetCloud.x[i] *= targetScale;
            targetCloud.y[i] *= targetScale;
            targetCloud.z[i] *= targetScale;
        }
    }

    // Compute optimal 2D alignment (equations 2-4).
    ComputeOptimal2DAlignmentSoA(sourceCloud, targetCloud, weights,
                                 outRotation, outTranslation);

    // Apply alignment and compute the weighted squared residual (AVX2 pass with
    // a scalar twin - the two produce the same value up to fp summation order).
    const float c = std::cos(outRotation);
    const float s = std::sin(outRotation);
    float totalW = 0.0f;
    return WeightedResidual(sourceCloud, targetCloud, weights,
                            c, s, outTranslation.x, outTranslation.y, totalW);
}

TransitionClipGenerator::TransitionSpec
TransitionClipGenerator::FindBestTransitionPoint(
    const MotionDatabase& sourceDB,
    const MotionDatabase& targetDB,
    const Skeleton* skeleton,
    int windowSize,
    int maxSearchPoses) {

    TransitionSpec bestSpec;
    float bestDist = std::numeric_limits<float>::max();
    const float kEarlyTermThreshold = 0.01f;  // Stop early if we find a near-perfect match
    const float kBaseSpeedTol = 0.5f;         // Speed compatibility tolerance (m/s in model units)
    const float kBaseMaxDistance = 0.15f;     // Baseline kinematic-passes gate
    const float kMaxSeparation = 0.5f;        // Same-clip identity-trap horizon (s)

    const int srcCount = (int)sourceDB.GetPoseCount();
    const int tgtCount = (int)targetDB.GetPoseCount();
    if (srcCount == 0 || tgtCount == 0) return bestSpec;

    // ---- Precompute target pose speeds for binary-search prefiltering ----
    // Sort target poses by speed so we can binary-search for candidates
    // within [sourceSpeed - tol, sourceSpeed + tol]. This reduces the
    // inner loop from O(M) to O(log M + candidates).
    struct SortedTarget {
        float speed;
        int index;
    };
    std::vector<SortedTarget> sortedTargets;
    sortedTargets.reserve(tgtCount);
    for (int i = 0; i < tgtCount; ++i) {
        sortedTargets.push_back({targetDB.GetPose(i).features.speed, i});
    }
    std::sort(sortedTargets.begin(), sortedTargets.end(),
              [](const SortedTarget& a, const SortedTarget& b) {
                  return a.speed < b.speed;
              });

    // ---- Determine search stride ----
    // Subsample source poses to keep total evaluations bounded.
    int sourceStride = 1;
    if (srcCount > maxSearchPoses) {
        sourceStride = srcCount / maxSearchPoses;
    }

    // ---- TIERED RELAXATION ----
    // Raw Mixamo packs routinely fail the baseline gates for reasons that are
    // not the character's fault: swapped / slightly out-of-phase foot labels,
    // and clip pairs that only just miss the distance gate. Dropping the edge
    // (or keeping the "least bad" frame) leaves the motion graph full of holes,
    // so each tier widens the search BEFORE giving up:
    //   tier 0: the original strict gates.
    //   tier 1: stop requiring identical foot-plant labels.
    //   tier 2: double the distance gate and widen the speed/root tolerances.
    // A tier stops the whole search as soon as it finds any acceptable pair.
    constexpr int kTiers = 3;
    for (int tier = 0; tier < kTiers; ++tier) {
        const bool enforceFootPlants = (tier == 0);
        float maxAcceptableDistance = kBaseMaxDistance;
        float speedTol = kBaseSpeedTol;
        if (tier >= 2) {
            maxAcceptableDistance = kBaseMaxDistance * 2.0f;
            speedTol = kBaseSpeedTol * 1.5f;
        }

        // ---- Search source poses ----
        for (int si = 0; si < srcCount; si += sourceStride) {
            const PoseSample& sPose = sourceDB.GetPose(si);
            float srcSpeed = sPose.features.speed;

            // Binary-search for target poses with compatible speed
            float lo = srcSpeed - speedTol;
            float hi = srcSpeed + speedTol;

            // Lower bound: first target with speed >= lo
            auto cmp = [](const SortedTarget& a, float val) { return a.speed < val; };
            auto itLo = std::lower_bound(sortedTargets.begin(), sortedTargets.end(), lo, cmp);
            if (itLo == sortedTargets.end()) continue;

            // Upper bound: last target with speed <= hi
            auto itHi = std::upper_bound(itLo, sortedTargets.end(), hi,
                [](float val, const SortedTarget& a) { return val < a.speed; });

            // Iterate only over speed-compatible target candidates
            for (auto it = itLo; it != itHi; ++it) {
                int ti = it->index;
                const PoseSample& tPose = targetDB.GetPose(ti);

                // CRITICAL CORRECTION: PHASE-MATCHING SYSTEM GATE
                // Prevent cross-domain foot popping (e.g., Left Foot down snapping to
                // Right Foot down) by requiring identical foot-plant states.
                // Relaxed from tier 1: Mixamo packs mislabel foot phases often
                // enough that a strict gate costs whole contexts their edges.
                if (enforceFootPlants &&
                    (sPose.leftFootPlanted != tPose.leftFootPlanted ||
                     sPose.rightFootPlanted != tPose.rightFootPlanted)) {
                    continue;
                }

                // IDENTITY-TRAP GUARD: a target pose from a clip with the SAME
                // registered name, only a few milliseconds away, is not a
                // transition - it is the same animation snapping to a
                // neighbouring frame, which the matcher then re-selects every
                // frame (micro-stutter / "same animation" picks). Require a
                // real separation instead. Uses the REGISTERED clip name, not
                // anim->name: Mixamo channel names are all "mixamo.com".
                const std::string srcClip = sourceDB.GetAnimationName(sPose.animationIndex);
                const std::string tgtClip = targetDB.GetAnimationName(tPose.animationIndex);
                if (!srcClip.empty() && srcClip == tgtClip &&
                    std::fabs(sPose.timeInSeconds - tPose.timeInSeconds) < kMaxSeparation) {
                    continue;
                }

                // Additional prefilter: root position proximity in 2D (XZ).
                // If the roots are too far apart, the windowed point-cloud
                // distance will be large — skip the expensive eval. The gate is
                // expressed from the (tier-dependent) speed tolerance so it
                // relaxes with the rest of the tier; at tier 0 it equals the
                // previous fixed 0.25 squared-model-units threshold.
                float dx = sPose.rootPosition.x - tPose.rootPosition.x;
                float dz = sPose.rootPosition.z - tPose.rootPosition.z;
                float rootDistSq = dx * dx + dz * dz;
                if (rootDistSq > speedTol * 0.5f) continue;

                float rotation;
                glm::vec2 translation;
                float dist = WindowPointCloudDistance(
                    sourceDB, targetDB,
                    si, ti,
                    windowSize, skeleton,
                    rotation, translation);

                // CRITICAL CORRECTION (forAnime.txt 964-973): DISTANCE THRESHOLD
                // GATE + trajectory forward-look-ahead penalty. Aggregate the
                // pose structural error alongside the long-term momentum path
                // error so the graph selects transitions for both instant pose
                // precision and future intent — eliminates "Transition
                // Blindness" (e.g. a straight sprint vs a banking stop beyond
                // the horizon).
                float trajectoryPenalty =
                    CalculateAdvancedTrajectoryPenalty(sPose, tPose, sourceDB, targetDB,
                                                       1.0f / 120.0f);
                float totalDeformationCost =
                    dist + trajectoryPenalty * sourceDB.GetWeights().trajectoryWeight;
                if (totalDeformationCost > maxAcceptableDistance) {
                    continue; // Reject jagged transitions safely
                }

                if (totalDeformationCost < bestDist) {
                    bestDist = totalDeformationCost;
                    bestSpec.sourcePoseIndex = si;
                    bestSpec.targetPoseIndex = ti;
                    bestSpec.alignmentRotation = rotation;
                    bestSpec.alignmentTranslation = translation;
                    bestSpec.distance = totalDeformationCost;

                    // Early termination: near-perfect match found
                    if (totalDeformationCost < kEarlyTermThreshold) return bestSpec;
                }
            }
        }

        // Accept this tier's result and stop relaxing.
        if (bestSpec.sourcePoseIndex >= 0) {
            if (tier > 0) {
                std::cout << "[TransitionClipGenerator] Tier " << tier
                          << " fallback succeeded (distance=" << bestSpec.distance << ")\n";
            }
            break;
        }
    }

    if (bestSpec.sourcePoseIndex < 0) {
        std::cerr << "[TransitionClipGenerator] WARNING: No valid transition point found\n";
    }

    return bestSpec;
}

float TransitionClipGenerator::CalculateAdvancedTrajectoryPenalty(
    const PoseSample& srcPose,
    const PoseSample& tgtPose,
    const MotionDatabase& sourceDB,
    const MotionDatabase& targetDB,
    float dt) {
    // forAnime.txt 888-961: velocity-scaled horizon, exponential temporal
    // discounting, and angular curvature alignment. Rejects transitions
    // that match pose-wise but diverge in long-term momentum
    // ("Transition Blindness" — e.g. a straight sprint vs a banking stop
    // beyond the horizon).

    // 1. Extract baseline velocity and speed characteristics
    float srcSpeed = glm::length(srcPose.features.rootVelocity);

    // 2. Dynamic Horizon Scaling: fast gaits need a deeper look-ahead; slow
    //    crouches check only the immediate frames.
    float baseHorizonSeconds = 0.5f;
    float scaledHorizon = glm::clamp(baseHorizonSeconds * (1.0f + srcSpeed * 0.15f), 0.3f, 1.5f);

    // Convert time horizon to frame steps (dt = 1/120s => 120fps parity)
    int maxStepsToCompare = glm::clamp(static_cast<int>(scaledHorizon / dt), 4, kTrajectorySteps);

    int srcPoints = std::min(srcPose.trajectory.localNumPoints, maxStepsToCompare);
    int tgtPoints = std::min(tgtPose.trajectory.localNumPoints, maxStepsToCompare);
    int cmpSteps  = std::min(srcPoints, tgtPoints);

    if (cmpSteps <= 0) return 0.0f;

    float accumulatedScore = 0.0f;
    float totalWeightMat    = 0.0f;

    // Exponential decay multiplier: lambda = 2.5 gives sharp immediate prioritization
    const float lambda = 2.5f;

    const int srcCount = static_cast<int>(sourceDB.GetPoseCount());
    const int tgtCount = static_cast<int>(targetDB.GetPoseCount());

    // 3. Vectorized Trajectory Feature Evaluation Pass
    for (int k = 0; k < cmpSteps; ++k) {
        float timeOffset = (k + 1) * kTrajectoryStepTime;

        // Compute exponential discount weight factor (Beta^k)
        float temporalWeight = std::exp(-lambda * timeOffset);

        // Position alignment delta
        glm::vec3 srcPos = srcPose.trajectory.localPositions[k];
        glm::vec3 tgtPos = tgtPose.trajectory.localPositions[k];
        float posError   = glm::distance(srcPos, tgtPos);

        // Velocity vector alignment delta (guard against OOB frame access)
        int srcFrame = srcPose.frameNumber + k;
        int tgtFrame = tgtPose.frameNumber + k;
        glm::vec3 srcVel = (srcFrame >= 0 && srcFrame < srcCount)
            ? sourceDB.GetPose(static_cast<size_t>(srcFrame)).features.rootVelocity
            : glm::vec3(0.0f);
        glm::vec3 tgtVel = (tgtFrame >= 0 && tgtFrame < tgtCount)
            ? targetDB.GetPose(static_cast<size_t>(tgtFrame)).features.rootVelocity
            : glm::vec3(0.0f);
        float velError   = glm::distance(srcVel, tgtVel);

        // 4. Curvature Verification Pass (Angular Centripetal Alignment)
        float curveError = 0.0f;
        if (k > 0 && k < cmpSteps - 1) {
            glm::vec3 srcDirPrev = glm::normalize(srcPose.trajectory.localPositions[k] -
                                                  srcPose.trajectory.localPositions[k - 1]);
            glm::vec3 srcDirNext = glm::normalize(srcPose.trajectory.localPositions[k + 1] -
                                                  srcPose.trajectory.localPositions[k]);
            glm::vec3 tgtDirPrev = glm::normalize(tgtPose.trajectory.localPositions[k] -
                                                  tgtPose.trajectory.localPositions[k - 1]);
            glm::vec3 tgtDirNext = glm::normalize(tgtPose.trajectory.localPositions[k + 1] -
                                                  tgtPose.trajectory.localPositions[k]);

            float srcKappa = glm::length(glm::cross(srcDirPrev, srcDirNext));
            float tgtKappa = glm::length(glm::cross(tgtDirPrev, tgtDirNext));
            curveError = std::abs(srcKappa - tgtKappa);
        }

        // Aggregate weighted terms into total score
        accumulatedScore += temporalWeight * (posError * 1.5f + velError * 0.8f + curveError * 3.0f);
        totalWeightMat += temporalWeight;
    }

    if (totalWeightMat <= 0.0f) return 0.0f;

    // Return normalized look-ahead cost scalar
    return accumulatedScore / totalWeightMat;
}

std::shared_ptr<Animation> TransitionClipGenerator::GenerateTransitionClip(
    const MotionDatabase& sourceDB,
    const MotionDatabase& targetDB,
    const TransitionSpec& spec,
    const Skeleton* skeleton,
    int transitionFrames,
    float fps) {

    if (spec.sourcePoseIndex < 0 || spec.targetPoseIndex < 0 || !skeleton) {
        return nullptr;
    }

    const PoseSample& sourcePose = sourceDB.GetPose(spec.sourcePoseIndex);
    const PoseSample& targetPose = targetDB.GetPose(spec.targetPoseIndex);
    auto sourceAnim = sourceDB.GetAnimation(sourcePose.animationIndex);
    auto targetAnim = targetDB.GetAnimation(targetPose.animationIndex);

    if (!sourceAnim || !targetAnim) {
        return nullptr;
    }

    // Create transition animation
    float duration = transitionFrames / fps;
    auto transitionClip = std::make_shared<Animation>("Transition", duration, fps);

    int numBones = (int)skeleton->bones.size();

    // Pre-sample source and target poses at their respective times, plus
    // surrounding frames for the window. We sample the source at times
    // [sourceTime + offset] and target at [targetTime + offset] for offset
    // in [0, transitionFrames/fps].
    float sourceT0 = sourcePose.timeInSeconds;
    float targetT0 = targetPose.timeInSeconds;

    // For each transition frame p (0 ≤ p < k):
    //   sourceSample = sourceTime + (p - offset) * frameDt
    //   targetSample = targetTime + (p - 0) * frameDt
    // where offset shifts the source forward so the blend starts from the
    // source pose and ends at the target pose.
    float frameDt = 1.0f / fps;

    // We need bone names. Build a reverse map: bone index -> bone name.
    std::vector<std::string> boneNames(numBones);
    for (const auto& [name, idx] : skeleton->boneMapping) {
        if (idx >= 0 && idx < numBones) {
            boneNames[idx] = name;
        }
    }

    // For each bone, collect keyframes
    for (int b = 0; b < numBones; ++b) {
        if (boneNames[b].empty()) continue;

        BoneAnimation transBone;
        transBone.boneName = boneNames[b];

        for (int p = 0; p < transitionFrames; ++p) {
            // Paper equation 7: α(p) = 2((p+1)/k)³ − 3((p+1)/k)² + 1
            // At p=0: α≈1 (source dominant), at p=k-1: α≈0 (target dominant)
            float alpha = SmoothstepC1((float)p, (float)transitionFrames);

            // Sample source pose at sourceTime + (p - 0) * dt
            // Sample target pose at targetTime + (p - 0) * dt
            // (The window in the distance function was used to FIND the right
            //  pose pair; the blend itself is a straightforward slerp between
            //  the two poses at matching temporal offsets.)
            float sourceSampleT = sourceT0 + (float)p * frameDt;
            float targetSampleT = targetT0 + (float)p * frameDt;

            // Wrap times to animation duration
            float sourceDur = sourceAnim->duration;
            float targetDur = targetAnim->duration;
            sourceSampleT = fmod(sourceSampleT, sourceDur);
            if (sourceSampleT < 0) sourceSampleT += sourceDur;
            targetSampleT = fmod(targetSampleT, targetDur);
            if (targetSampleT < 0) targetSampleT += targetDur;

            // Sample source bone TRS
            glm::vec3 srcPos(0.0f), srcScale(1.0f);
            glm::quat srcRot(1.0f, 0.0f, 0.0f, 0.0f);
            const BoneAnimation* srcBone = sourceAnim->GetBoneAnimation(boneNames[b]);
            if (srcBone) {
                srcPos = srcBone->InterpolatePosition(sourceSampleT);
                srcRot = srcBone->InterpolateRotation(sourceSampleT);
                srcScale = srcBone->InterpolateScale(sourceSampleT);
            }

            // Sample target bone TRS
            glm::vec3 tgtPos(0.0f), tgtScale(1.0f);
            glm::quat tgtRot(1.0f, 0.0f, 0.0f, 0.0f);
            const BoneAnimation* tgtBone = targetAnim->GetBoneAnimation(boneNames[b]);
            if (tgtBone) {
                tgtPos = tgtBone->InterpolatePosition(targetSampleT);
                tgtRot = tgtBone->InterpolateRotation(targetSampleT);
                tgtScale = tgtBone->InterpolateScale(targetSampleT);
            }

            // For the ROOT bone: align using the 2D transform (equation 1)
            // For ALL bones: slerp rotation (equation 6), lerp position/scale
            glm::vec3 blendPos, blendScale;
            glm::quat blendRot;

            if (b == 0) {
                // Root: apply 2D alignment to target position, then linear interp
                glm::vec3 alignedTargetPos = Apply2DTransform(
                    tgtPos, spec.alignmentRotation, spec.alignmentTranslation);
                blendPos = glm::mix(srcPos, alignedTargetPos, 1.0f - alpha);
            } else {
                blendPos = glm::mix(srcPos, tgtPos, 1.0f - alpha);
            }

            blendScale = glm::mix(srcScale, tgtScale, 1.0f - alpha);

            // Slerp rotation (equation 6: qip = slerp(qi+p, qj-k+1+p, α(p)))
            // Ensure quaternion hemisphere consistency
            if (glm::dot(srcRot, tgtRot) < 0.0f) {
                tgtRot = -tgtRot;
            }
            blendRot = glm::slerp(srcRot, tgtRot, 1.0f - alpha);

            // Add keyframes
            double keyTime = (double)p * frameDt;

            transBone.positionTimes.push_back(keyTime);
            transBone.positionValues.push_back(blendPos);

            transBone.rotationTimes.push_back(keyTime);
            transBone.rotationValues.push_back(blendRot);

            transBone.scaleTimes.push_back(keyTime);
            transBone.scaleValues.push_back(blendScale);
        }

        transitionClip->AddBoneAnimation(transBone);
    }

    return transitionClip;
}
