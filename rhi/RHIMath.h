/**
 * RHI math helpers - minimal column-major 4x4 matrix math shared by the RHI
 * backends. The RHI is deliberately dependency-free (no glm), so the handful
 * of matrices the offscreen scene render needs (perspective, look-at, model)
 * live here. Everything is GL-convention: right-handed, camera looking down
 * -Z, NDC z in [-1,1], column-major storage (m[col*4+row]).
 */
#pragma once

#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdint>

namespace RHI {

struct Mat4 {
    // Column-major identity.
    float m[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
};

// Camera + fog uniforms + FSR3 history uniforms shared by BOTH backends.
// std140 layout, column-major. The FSR3 history fields (prevViewProj,
// jitterOffset) are APPENDED after the original 96-byte block so the offsets
// of viewProj/camPos/fogDensity/fogColor/pad are UNCHANGED on both backends —
// this keeps the existing GL<->Vulkan pixel parity (and the offscreen parity
// tests) byte-for-byte identical; the new fields are staged here in Phase 1 and
// only consumed by the shaders in Phase 4 (jitter injection + reprojection).
//
// std140 offsets: mat4 viewProj (0..63), vec3 camPos (64..75), float
// fogDensity (76..79), vec3 fogColor (80..91), float pad (92..95),
// mat4 prevViewProj (96..159), vec2 jitterOffset (160..167), pad2 (168..175).
struct CameraUBOData {
    Mat4 viewProj;                  // 64 bytes
    float camPos[3];                // vec3 (16-byte aligned in std140)
    float fogDensity = 0.0f;
    float fogColor[3] = {0.55f, 0.62f, 0.70f};   // hazy sky blue
    float pad = 0.0f;
    // --- FSR3 (Phase 1: staged, inert until Phase 4 shader wiring) ---
    Mat4 prevViewProj;              // previous frame's viewProj (reprojection)
    float jitterOffset[2] = {0.0f, 0.0f};  // sub-pixel NDC offset (Halton)
    float pad2[2] = {0.0f, 0.0f};   // std140 pad -> sizeof == 176 (16-aligned)
};

inline Mat4 mat4Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            r.m[c * 4 + row] = 0.0f;
            for (int k = 0; k < 4; ++k) {
                r.m[c * 4 + row] += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
        }
    }
    return r;
}

// GL-style symmetric perspective: right-handed, looking down -Z, NDC z in
// [-1,1] (near maps to -1). Identical to glm::perspective(fovY, aspect, near, far).
inline Mat4 mat4Perspective(float fovYDeg, float aspect, float zNear, float zFar) {
    const float f = 1.0f / std::tan(fovYDeg * 0.5f * 3.14159265358979f / 180.0f);
    Mat4 p{};
    p.m[0] = f / aspect;
    p.m[5] = f;
    p.m[10] = (zFar + zNear) / (zNear - zFar);
    p.m[11] = -1.0f;
    p.m[14] = (2.0f * zFar * zNear) / (zNear - zFar);
    p.m[15] = 0.0f;   // projective bottom-right (matches glm::perspective / gluPerspective);
                      // leaving the identity default 1.0f skews w = -z*eye + 1 and diverges
                      // from the engine's GLM cameras (flyCamera/ThirdPersonCamera/Camera).
    return p;
}

// Right-handed look-at (glm::lookAt): builds the view matrix for a camera at
// `eye` looking toward `center` with `up` as the up hint.
inline Mat4 mat4LookAt(const float eye[3], const float center[3], const float up[3]) {
    float f[3] = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl > 1e-8f) { f[0] /= fl; f[1] /= fl; f[2] /= fl; }

    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
                  f[0] * up[1] - f[1] * up[0]};
    const float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (sl > 1e-8f) { s[0] /= sl; s[1] /= sl; s[2] /= sl; }

    const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2],
                        s[0] * f[1] - s[1] * f[0]};

    Mat4 v{};
    v.m[0] = s[0];  v.m[4] = s[1];  v.m[8] = s[2];
    v.m[1] = u[0];  v.m[5] = u[1];  v.m[9] = u[2];
    v.m[2] = -f[0]; v.m[6] = -f[1]; v.m[10] = -f[2];
    v.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    v.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    v.m[14] =  (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
    return v;
}

// Model matrices: T * S with translation (x,y,z) and uniform scale s.
inline Mat4 mat4TranslateScale(float x, float y, float z, float s) {
    Mat4 t{};
    t.m[0] = s; t.m[5] = s; t.m[10] = s;
    t.m[12] = x; t.m[13] = y; t.m[14] = z;
    return t;
}

// 36 vertices of a unit cube centered at the origin (positions only), shared
// by both RHI backends for the offscreen 3D scene render. Culling is off, so
// winding does not matter.
inline constexpr float kCubeVerts[108] = {
    // -Z face
    -0.5f, -0.5f, -0.5f,   0.5f, -0.5f, -0.5f,   0.5f,  0.5f, -0.5f,
    -0.5f, -0.5f, -0.5f,   0.5f,  0.5f, -0.5f,  -0.5f,  0.5f, -0.5f,
    // +Z face
    -0.5f, -0.5f,  0.5f,   0.5f,  0.5f,  0.5f,   0.5f, -0.5f,  0.5f,
    -0.5f, -0.5f,  0.5f,  -0.5f,  0.5f,  0.5f,   0.5f,  0.5f,  0.5f,
    // -X face
    -0.5f, -0.5f, -0.5f,  -0.5f,  0.5f,  0.5f,  -0.5f, -0.5f,  0.5f,
    -0.5f, -0.5f, -0.5f,  -0.5f,  0.5f, -0.5f,  -0.5f,  0.5f,  0.5f,
    // +X face
     0.5f, -0.5f, -0.5f,   0.5f, -0.5f,  0.5f,   0.5f,  0.5f,  0.5f,
     0.5f, -0.5f, -0.5f,   0.5f,  0.5f,  0.5f,   0.5f,  0.5f, -0.5f,
    // -Y face
    -0.5f, -0.5f, -0.5f,   0.5f, -0.5f, -0.5f,   0.5f, -0.5f,  0.5f,
    -0.5f, -0.5f, -0.5f,   0.5f, -0.5f,  0.5f,  -0.5f, -0.5f,  0.5f,
    // +Y face
    -0.5f,  0.5f, -0.5f,   0.5f,  0.5f,  0.5f,   0.5f,  0.5f, -0.5f,
    -0.5f,  0.5f, -0.5f,  -0.5f,  0.5f,  0.5f,   0.5f,  0.5f,  0.5f,
};

// Convert a GL-convention projection matrix to Vulkan NDC so that the SAME
// scene description renders IDENTICAL pixels on both backends. Vulkan differs
// in two ways:
//   - NDC y points DOWN (GL: up). Negating projection row 1, combined with
//     the viewport transform, puts +y back on screen-up like GL.
//   - NDC z is in [0,1] (GL: [-1,1]). Remap z' = 0.5*z + 0.5*w.
// Column-major: "row 1" = m[1],m[5],m[9],m[13]; "row 2" = m[2],m[6],m[10],m[14].
inline Mat4 mat4Inverse(const Mat4& a) {
    // Gauss-Jordan inversion on the augmented matrix [a | I]. a is column-major
    // (m[col*4+row]); we pivot in a row-major working copy. Returns an identity
    // (no-op) Mat4 if the matrix is singular — the caller guards temporal use by
    // frame validity, so a singular inverse (e.g. degenerate first frame) falls
    // back to passthrough instead of dividing by zero.
    float aug[4][8]{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) aug[r][c] = a.m[c * 4 + r];   // col-major -> row-major
        aug[r][4 + r] = 1.0f;
    }
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        float pivVal = std::fabs(aug[col][col]);
        for (int r = col + 1; r < 4; ++r) { float v = std::fabs(aug[r][col]); if (v > pivVal) { pivVal = v; piv = r; } }
        if (piv != col) { for (int c = 0; c < 8; ++c) std::swap(aug[col][c], aug[piv][c]); }
        float pv = aug[col][col];
        if (std::fabs(pv) < 1e-12f) return Mat4{};
        for (int c = 0; c < 8; ++c) aug[col][c] /= pv;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            float f = aug[r][col];
            for (int c = 0; c < 8; ++c) aug[r][c] -= f * aug[col][c];
        }
    }
    Mat4 out{};
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) out.m[c * 4 + r] = aug[r][c + 4];
    return out;
}

// Convert a GL-convention projection matrix to Vulkan NDC so that the SAME
// scene description renders IDENTICAL pixels on both backends. Vulkan differs
// in two ways:
//   - NDC y points DOWN (GL: up). Negating projection row 1, combined with
//     the viewport transform, puts +y back on screen-up like GL.
//   - NDC z is in [0,1] (GL: [-1,1]). Remap z' = 0.5*z + 0.5*w.
// Column-major: "row 1" = m[1],m[5],m[9],m[13]; "row 2" = m[2],m[6],m[10],m[14].
inline Mat4 mat4GlToVulkanProj(const Mat4& gl) {
    Mat4 v = gl;
    v.m[1] = -v.m[1]; v.m[5] = -v.m[5]; v.m[9] = -v.m[9]; v.m[13] = -v.m[13];
    for (int c = 0; c < 4; ++c) {
        const float zc = v.m[c * 4 + 2];
        const float wc = v.m[c * 4 + 3];
        v.m[c * 4 + 2] = 0.5f * zc + 0.5f * wc;
    }
    return v;
}

// ---------------------------------------------------------------------------
// FSR3 sub-pixel jitter (Phase 1: Halton(2,3) sequence).
// A 2D low-discrepancy sample drives per-frame camera-subpixel jitter so the
// temporal upscaler has sub-pixel coverage to reconstruct from. Phase 1 stages
// the sequence + NDC mapping; Phase 2 injects jitterOffset into the
// projection (Vulkan via CameraUBO@160; GL via a uJitter uniform, since
// GL #version 330 lacks layout(offset=N)). Frame 0 -> {0,0} (stable ref).
// ---------------------------------------------------------------------------
struct Vec2 { float x = 0.0f, y = 0.0f; };

// Halton radical inverse: reverse the digits of n in `base`, e.g.
// halton(2,1)=0.5, halton(2,2)=0.25, halton(3,1)=0.3333, halton(3,2)=0.6667.
inline float halton(int base, int n) {
    float result = 0.0f;
    float f = 1.0f;
    int nn = n;
    while (nn > 0) {
        f /= static_cast<float>(base);
        result += f * static_cast<float>(nn % base);
        nn /= base;
    }
    return result;
}

// 2D Halton(2,3) point in [0,1)^2. Frame index is 1-based for the sequence
// (frame 0 -> (0,0), i.e. no jitter, keeps the first frame stable).
inline Vec2 halton23(int frameIndex) {
    if (frameIndex <= 0) return {0.0f, 0.0f};
    return { halton(2, frameIndex), halton(3, frameIndex) };
}

// Map a Halton sample to an NDC sub-pixel offset spanning +/-0.5 px at the
// render resolution. This is what gets written into CameraUBOData.jitterOffset
// and (Phase 4) applied to the projection matrix before rasterization. Frame 0
// (and any non-positive index) maps to {0,0}: the first frame is the FSR
// temporal-history reference, so it must be jitter-free / stable. This also
// keeps the headless offscreen parity scene byte-identical to the un-jittered
// reference, since each backend renders its offscreen scene exactly once
// (frame 0) before any m_frame advance.
inline Vec2 ndcJitter(int frameIndex, int renderWidth, int renderHeight) {
    if (frameIndex <= 0) return {0.0f, 0.0f};
    const Vec2 h = halton23(frameIndex);
    return { (h.x - 0.5f) / static_cast<float>(renderWidth),
             (h.y - 0.5f) / static_cast<float>(renderHeight) };
}

// Flip a row-major RG image (2 floats/pixel) in place, top<->bottom. Used to
// turn the GL/Vulkan bottom-up velocity readback into a top-down buffer for
// the tests. Self-contained (no extra headers).
inline void FlipRows(float* rg, int w, int h) {
    const int rowFloats = w * 2;
    for (int y = 0; y < h / 2; ++y) {
        float* a = rg + y * rowFloats;
        float* b = rg + (h - 1 - y) * rowFloats;
        for (int i = 0; i < rowFloats; ++i) {
            const float tmp = a[i]; a[i] = b[i]; b[i] = tmp;
        }
    }
}

// ---------------------------------------------------------------------------
// FSR3 EASU + RCAS shader constants (Phase 4).
// These reproduce AMD's ffxFsrPopulateEasuConstants (ffx_fsr1.h) and
// FsrRcasCon bit-for-bit.  Extracting them into RHIMath.h makes the
// constant math unit-testable from the test runner WITHOUT a Vulkan device
// (the original copies lived as anonymous static functions inside
// RHIVulkan.cpp methods, making them unreachable from tests).
// ---------------------------------------------------------------------------

// 4 x uint32vec4 (80 bytes) — matches the std140 CB layout the EASU/RCAS
// compute shaders declare at binding 3000.  The shaders only read the first
// 4 floats of const3 (c[12]) for RCAS sharpness and con3.x/z/w (c[12..14])
// for EASU; c[15] = sample0.x/y/z/w (debug supersample, unused by the
// float EASU path).  Zero-initialised so unused slots are clean.
struct EasuCon {
    uint32_t c[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
};

// Bit-cast a float to uint32 without aliasing UB (mirrors ffxAsUInt32).
inline uint32_t fsr3FloatToU32(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(float));
    return u;
}

// Bit-cast a uint32 back to float (mirrors ffxAsFloat).  Used by tests to
// recover the original float values for assertion comparison.
inline float fsr3U32ToFloat(uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof(float));
    return f;
}

// Populate EASU constants for an upscale from renderW×renderH to outW×outH.
// jitterX/jitterY are the NDC sub-pixel offsets applied to the low-res render
// (from ndcJitter); EASU cancels them by shifting the gather taps.  Pass
// {0,0} for a jitter-free reference frame (frame 0).
inline EasuCon makeEasuCon(uint32_t renderW, uint32_t renderH,
                           uint32_t outW, uint32_t outH,
                           float jitterX, float jitterY) {
    EasuCon k{};
    auto rcp = [](float x) { return 1.0f / x; };
    // Output integer position -> pixel position in viewport.
    k.c[0]  = fsr3FloatToU32((float)renderW * rcp((float)outW));
    k.c[1]  = fsr3FloatToU32((float)renderH * rcp((float)outH));
    k.c[2]  = fsr3FloatToU32(0.5f * (float)renderW * rcp((float)outW) - 0.5f);
    k.c[3]  = fsr3FloatToU32(0.5f * (float)renderH * rcp((float)outH) - 0.5f);
    // Viewport pixel position -> normalized image space (upper-left 'F' tap).
    k.c[4]  = fsr3FloatToU32(rcp((float)renderW));
    k.c[5]  = fsr3FloatToU32(rcp((float)renderH));
    // Centers of gather4, first offset from upper-left of 'F'.
    k.c[6]  = fsr3FloatToU32(1.0f * rcp((float)renderW));
    k.c[7]  = fsr3FloatToU32(-1.0f * rcp((float)renderH));
    // These are from (0) instead of 'F'.
    k.c[8]  = fsr3FloatToU32(-1.0f * rcp((float)renderW));
    k.c[9]  = fsr3FloatToU32(2.0f  * rcp((float)renderH));
    k.c[10] = fsr3FloatToU32(1.0f  * rcp((float)renderW));
    k.c[11] = fsr3FloatToU32(2.0f  * rcp((float)renderH));
    k.c[12] = fsr3FloatToU32(0.0f);          // con3.x
    k.c[13] = fsr3FloatToU32(4.0f * rcp((float)renderH));
    k.c[14] = 0;                             // con3.z
    k.c[15] = 0;                             // sample0.x/y/z/w
    return k;
}

// Populate RCAS constants from a sharpness in "stops" (the user-facing
// sharpness slider value).  const3.x = exp2(-stops); the shader only reads
// const3.x on the 32-bit path, so the remaining slots are left zero.
inline EasuCon makeRcasCon(float sharpnessStops) {
    EasuCon k{};
    const float s = std::exp2(-sharpnessStops);
    k.c[12] = fsr3FloatToU32(s);
    return k;
}

} // namespace RHI
