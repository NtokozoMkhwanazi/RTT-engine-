/**
 * Skinning.h - CPU skinning for the RHI mesh path.
 *
 * The RHI scene renderer draws skinned characters by CPU-skinning their
 * vertices every frame and re-uploading the mesh (the Vulkan viewport does
 * this; the GL path GPU-skins the same data). This helper is the SINGLE
 * skinning implementation shared by the editor and the tests, kept
 * byte-identical to the GL GPU skinning in shaderSystem/VS.glsl:
 *
 *   - weights are NORMALIZED (aWeights / sum) before blending - the GL
 *     shader does this, so unnormalized weights can't diverge between
 *     backends;
 *   - the skin matrix is the weighted sum of the animator's final bone
 *     matrices (Animator::GetFinalBoneMatrices - each already includes the
 *     bone's offset matrix);
 *   - vertices with no bone weights (sum < 0.001) keep their bind pose;
 *   - positions/normals are transformed, uvs pass through unchanged.
 *
 * IMPORTANT: `out` must NOT alias `bindVertices`. Skinning in place - reading
 * the very buffer being overwritten - makes frame N skin frame N-1's output,
 * so the bone transforms compound frame over frame (p_n = T_n * ... * T_1 *
 * p_bind) and the mesh deforms into a mess within seconds.
 */
#pragma once

#include <vector>

#include <glm/glm.hpp>

// AVX2 intrinsics for the fast skinning pass (SkinVerticesSIMD). Guarded so the
// header stays portable on non-x86; the dispatcher falls back to scalar there.
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace Skinning {

// Interleaved vertex format of the RHI mesh path (rhi/shaders/mesh.vert):
// [pos.xyz][normal.xyz][uv] = 8 floats per vertex.
constexpr int kFloatsPerVertex = 8;

// Skin `bindVertices` through `boneMatrices` into `out`. `boneIds[i]` /
// `boneWeights[i]` are the per-vertex bone indices / weights (global skeleton
// indices - the same space Animator::GetFinalBoneMatrices is indexed by).
// `out` is resized to match and must not alias `bindVertices`.
// Scalar skinning pass (reference; also the non-AVX fallback).
inline void SkinVerticesScalar(const std::vector<float>& bindVertices,
                         const std::vector<glm::ivec4>& boneIds,
                         const std::vector<glm::vec4>& boneWeights,
                         const std::vector<glm::mat4>& boneMatrices,
                         std::vector<float>& out) {
    const size_t vertCount = boneIds.size();
    if (bindVertices.size() != vertCount * kFloatsPerVertex) {
        out.clear();
        return;  // layout mismatch - nothing sane to do
    }
    out.resize(bindVertices.size());
    const float* src = bindVertices.data();
    float* dst = out.data();
    for (size_t i = 0; i < vertCount; ++i) {
        const glm::ivec4& ids = boneIds[i];
        const glm::vec4& wts = boneWeights[i];
        const float* s = src + i * kFloatsPerVertex;
        float* d = dst + i * kFloatsPerVertex;
        const float wsum = wts.x + wts.y + wts.z + wts.w;
        if (wsum < 0.001f) {
            // Static vertex - keep the bind pose.
            for (int k = 0; k < kFloatsPerVertex; ++k) d[k] = s[k];
            continue;
        }
        // Normalize weights exactly like the GL shader (VS.glsl) so CPU and
        // GPU skinning agree even when the authored weights don't sum to 1.
        const glm::vec4 wn = wts / wsum;
        glm::mat4 t(0.0f);
        bool any = false;
        if (wn.x > 0.001f && ids.x >= 0 && ids.x < (int)boneMatrices.size()) { t += wn.x * boneMatrices[ids.x]; any = true; }
        if (wn.y > 0.001f && ids.y >= 0 && ids.y < (int)boneMatrices.size()) { t += wn.y * boneMatrices[ids.y]; any = true; }
        if (wn.z > 0.001f && ids.z >= 0 && ids.z < (int)boneMatrices.size()) { t += wn.z * boneMatrices[ids.z]; any = true; }
        if (wn.w > 0.001f && ids.w >= 0 && ids.w < (int)boneMatrices.size()) { t += wn.w * boneMatrices[ids.w]; any = true; }
        if (!any) {
            // No in-range bone contributed - the GL shader falls back to
            // identity (validSkin == false -> skin = mat4(1.0)), i.e. bind pose.
            for (int k = 0; k < kFloatsPerVertex; ++k) d[k] = s[k];
            continue;
        }
        const glm::vec3 p = glm::vec3(t * glm::vec4(s[0], s[1], s[2], 1.0f));
        const glm::vec3 n = glm::normalize(glm::mat3(t) * glm::vec3(s[3], s[4], s[5]));
        d[0] = p.x; d[1] = p.y; d[2] = p.z;
        d[3] = n.x; d[4] = n.y; d[5] = n.z;
        d[6] = s[6]; d[7] = s[7];   // uv is untouched by skinning
    }
}

// ---------------------------------------------------------------------------
// AVX2 skinning pass: identical math to SkinVerticesScalar, but the static
// (no-weight / no-in-range-bone) vertex copy uses one unaligned 256-bit
// load/store (8 floats = 1 interleaved vertex) instead of an 8-iteration scalar
// loop. Compiled with AVX2 ISA via the target attribute (no global -mavx2
// needed). Output is bit-identical to SkinVerticesScalar.
// ---------------------------------------------------------------------------
#if defined(__GNUC__) && defined(__x86_64__)
__attribute__((target("avx2")))
#endif
inline void SkinVerticesSIMD(const std::vector<float>& bindVertices,
                             const std::vector<glm::ivec4>& boneIds,
                             const std::vector<glm::vec4>& boneWeights,
                             const std::vector<glm::mat4>& boneMatrices,
                             std::vector<float>& out) {
    const size_t vertCount = boneIds.size();
    if (bindVertices.size() != vertCount * kFloatsPerVertex) {
        out.clear();
        return;
    }
    out.resize(bindVertices.size());
    const float* src = bindVertices.data();
    float* dst = out.data();
    for (size_t i = 0; i < vertCount; ++i) {
        const glm::ivec4& ids = boneIds[i];
        const glm::vec4& wts = boneWeights[i];
        const float* s = src + i * kFloatsPerVertex;
        float* d = dst + i * kFloatsPerVertex;
        const float wsum = wts.x + wts.y + wts.z + wts.w;
        if (wsum < 0.001f) {
            // Static vertex - keep the bind pose.
#if defined(__GNUC__) && defined(__x86_64__)
            __m256 v = _mm256_loadu_ps(s);   // unaligned - safe for std::vector<float>
            _mm256_storeu_ps(d, v);
#else
            for (int k = 0; k < kFloatsPerVertex; ++k) d[k] = s[k];
#endif
            continue;
        }
        // Normalize weights exactly like the GL shader (VS.glsl).
        const glm::vec4 wn = wts / wsum;
        glm::mat4 t(0.0f);
        bool any = false;
        if (wn.x > 0.001f && ids.x >= 0 && ids.x < (int)boneMatrices.size()) { t += wn.x * boneMatrices[ids.x]; any = true; }
        if (wn.y > 0.001f && ids.y >= 0 && ids.y < (int)boneMatrices.size()) { t += wn.y * boneMatrices[ids.y]; any = true; }
        if (wn.z > 0.001f && ids.z >= 0 && ids.z < (int)boneMatrices.size()) { t += wn.z * boneMatrices[ids.z]; any = true; }
        if (wn.w > 0.001f && ids.w >= 0 && ids.w < (int)boneMatrices.size()) { t += wn.w * boneMatrices[ids.w]; any = true; }
        if (!any) {
            // No in-range bone contributed - the GL shader falls back to
            // identity (validSkin == false -> skin = mat4(1.0)), i.e. bind pose.
#if defined(__GNUC__) && defined(__x86_64__)
            __m256 v = _mm256_loadu_ps(s);
            _mm256_storeu_ps(d, v);
#else
            for (int k = 0; k < kFloatsPerVertex; ++k) d[k] = s[k];
#endif
            continue;
        }
        const glm::vec3 p = glm::vec3(t * glm::vec4(s[0], s[1], s[2], 1.0f));
        const glm::vec3 n = glm::normalize(glm::mat3(t) * glm::vec3(s[3], s[4], s[5]));
        d[0] = p.x; d[1] = p.y; d[2] = p.z;
        d[3] = n.x; d[4] = n.y; d[5] = n.z;
        d[6] = s[6]; d[7] = s[7];   // uv is untouched by skinning
    }
}

// Dispatcher: use the AVX2 pass at runtime when the CPU supports it, else the
// scalar fallback. Callers are unchanged (Skinning::SkinVertices) so the
// editor, GL/Vulkan and test paths get the faster pass transparently.
inline void SkinVertices(const std::vector<float>& bindVertices,
                         const std::vector<glm::ivec4>& boneIds,
                         const std::vector<glm::vec4>& boneWeights,
                         const std::vector<glm::mat4>& boneMatrices,
                         std::vector<float>& out) {
#if defined(__GNUC__) && defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) {
        SkinVerticesSIMD(bindVertices, boneIds, boneWeights, boneMatrices, out);
        return;
    }
#endif
    SkinVerticesScalar(bindVertices, boneIds, boneWeights, boneMatrices, out);
}

} // namespace Skinning
