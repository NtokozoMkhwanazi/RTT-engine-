/**
 * FSR3 EASU + RCAS pipeline tests.
 *
 * Verifies the AMD FSR3 constant math (ffxFsrPopulateEasuConstants /
 * FsrRcasCon) that is shared between the Vulkan and OpenGL RHI backends.
 * The constants were previously anonymous static functions inside
 * RHIVulkan.cpp methods and therefore unreachable from the test runner.
 * They have been extracted to rhi/RHIMath.h (RHI::makeEasuCon /
 * RHI::makeRcasCon) so the bit-exact constant layout can be asserted
 * WITHOUT a Vulkan device or swapchain.
 *
 * Run with: make test  (or gtest_filter="FSR3*")
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <cmath>

#include "../rhi/RHIMath.h"

// --- Helpers -----------------------------------------------------------------

// Decode the engine's float-as-uint32 packing used by the compute shaders.
static float asFloat(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// AMD's reference: renderW and renderH map to themselves when outW==renderW
// (1x), the ratio is 1.0; for 2x it is 0.5 (half).
// con0 = {renderW/outW, renderH/outH}
// con1 = {(rw/ow - 1)*0.5, (rh/oh - 1)*0.5}  — "F" tap center offset
// con2 = {1/rw, 1/rh}
// con3.x = 0, con3.y = {4/rh}, con3.zw unused

// =============================================================================
// EASU constant tests
// =============================================================================

/**
 * 1x passthrough (no upscale): renderW==outW.
 * con0 should be {1.0, 1.0}, con1 should be {0,0}, con2 should be {1/W, 1/H}.
 */
TEST(FSR3Easu, Passthrough_1x) {
    constexpr uint32_t W = 128, H = 128;
    RHI::EasuCon k = RHI::makeEasuCon(W, H, W, H, 0.0f, 0.0f);

    EXPECT_FLOAT_EQ(asFloat(k.c[0]), 1.0f);          // renderW/outW = 1
    EXPECT_FLOAT_EQ(asFloat(k.c[1]), 1.0f);          // renderH/outH = 1
    EXPECT_FLOAT_EQ(asFloat(k.c[2]), 0.0f);          // 0.5*1 - 0.5 = 0
    EXPECT_FLOAT_EQ(asFloat(k.c[3]), 0.0f);          // 0.5*1 - 0.5 = 0
    EXPECT_FLOAT_EQ(asFloat(k.c[4]), 1.0f / W);      // 1/renderW
    EXPECT_FLOAT_EQ(asFloat(k.c[5]), 1.0f / H);      // 1/renderH
    EXPECT_FLOAT_EQ(asFloat(k.c[6]), 1.0f / W);      // +1/renderW
    EXPECT_FLOAT_EQ(asFloat(k.c[7]), -1.0f / H);     // -1/renderH
    EXPECT_FLOAT_EQ(asFloat(k.c[8]), -1.0f / W);     // -1/renderW
    EXPECT_FLOAT_EQ(asFloat(k.c[9]),  2.0f / H);     // +2/renderH
    EXPECT_FLOAT_EQ(asFloat(k.c[10]), 1.0f / W);     // +1/renderW
    EXPECT_FLOAT_EQ(asFloat(k.c[11]), 2.0f / H);     // +2/renderH

    // con3 (unused on EASU 32-bit path except .x which is 0)
    EXPECT_FLOAT_EQ(asFloat(k.c[12]), 0.0f);
    EXPECT_FLOAT_EQ(asFloat(k.c[13]), 4.0f / H);
    EXPECT_EQ(k.c[14], 0u);
    EXPECT_EQ(k.c[15], 0u);
}

/**
 * 2x upscale (the canonical QA scenario): 64x64 → 128x128.
 * con0 = {0.5, 0.5}, con1 = {-0.25, -0.25}.
 */
TEST(FSR3Easu, Upscale_2x) {
    constexpr uint32_t RW = 64, RH = 64, OW = 128, OH = 128;
    RHI::EasuCon k = RHI::makeEasuCon(RW, RH, OW, OH, 0.0f, 0.0f);

    EXPECT_FLOAT_EQ(asFloat(k.c[0]), 0.5f);          // 64/128
    EXPECT_FLOAT_EQ(asFloat(k.c[1]), 0.5f);          // 64/128
    EXPECT_FLOAT_EQ(asFloat(k.c[2]), -0.25f);         // 0.5*0.5 - 0.5
    EXPECT_FLOAT_EQ(asFloat(k.c[3]), -0.25f);
    EXPECT_FLOAT_EQ(asFloat(k.c[4]), 1.0f / RW);     // 1/64
    EXPECT_FLOAT_EQ(asFloat(k.c[5]), 1.0f / RH);
    EXPECT_FLOAT_EQ(asFloat(k.c[13]), 4.0f / RH);    // 4/64
}

/**
 * Non-square upscale 1.5x: 100x80 → 150x120.
 * Verifies aspect-aware scaling (con0 != {0.5,0.5}).
 */
TEST(FSR3Easu, NonSquare_1p5x) {
    constexpr uint32_t RW = 100, RH = 80, OW = 150, OH = 120;
    RHI::EasuCon k = RHI::makeEasuCon(RW, RH, OW, OH, 0.0f, 0.0f);

    EXPECT_FLOAT_EQ(asFloat(k.c[0]), 100.0f / 150.0f);   // rw/ow
    EXPECT_FLOAT_EQ(asFloat(k.c[1]), 80.0f / 120.0f);     // rh/oh
    EXPECT_FLOAT_EQ(asFloat(k.c[2]), 0.5f * (100.0f/150.0f) - 0.5f);
    EXPECT_FLOAT_EQ(asFloat(k.c[3]), 0.5f * (80.0f/120.0f)  - 0.5f);
    EXPECT_FLOAT_EQ(asFloat(k.c[4]), 1.0f / RW);
    EXPECT_FLOAT_EQ(asFloat(k.c[5]), 1.0f / RH);
    EXPECT_FLOAT_EQ(asFloat(k.c[13]), 4.0f / RH);
}

/**
 * Bit-cast round-trip: fsr3FloatToU32 / fsr3U32ToFloat must preserve values.
 */
TEST(FSR3Easu, FloatU32RoundTrip) {
    const float vals[] = {0.0f, 1.0f, -0.5f, 0.25f, 3.14159f, -123.456f,
                          1.0f/64.0f, -1.0f/64.0f, 4.0f/64.0f, 1e-10f};
    for (float v : vals) {
        uint32_t u = RHI::fsr3FloatToU32(v);
        float back = RHI::fsr3U32ToFloat(u);
        EXPECT_FLOAT_EQ(back, v) << "round-trip failed for " << v;
    }
}

/**
 * Jitter fields (jitterX, jitterY) are accepted but currently unused in the
 * constant computation — the EASU gather taps are computed from the render
 * resolution alone, and jitter is cancelled at sample time in the shader.
 * Verify the constants are the same with or without jitter for the 1x case.
 */
TEST(FSR3Easu, JitterDoesNotAlterConstants) {
    constexpr uint32_t RW = 100, RH = 80, OW = 150, OH = 120;
    RHI::EasuCon k0 = RHI::makeEasuCon(RW, RH, OW, OH, 0.0f, 0.0f);
    RHI::EasuCon kj = RHI::makeEasuCon(RW, RH, OW, OH, 0.5f, 0.25f);
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(k0.c[i], kj.c[i]) << "jitter altered con slot " << i;
    }
}

/**
 * Structure size must be exactly 64 bytes (4 × uint32vec4) to match the
 * std140 uniform-buffer layout the compute shaders declare at binding 3000.
 */
TEST(FSR3Easu, StructureSizeIs64Bytes) {
    EXPECT_EQ(sizeof(RHI::EasuCon), 64u);
    EXPECT_EQ(sizeof(RHI::EasuCon::c) / sizeof(uint32_t), 16u);
}

// =============================================================================
// RCAS constant tests
// =============================================================================

/**
 * RCAS const3.x = exp2(-sharpnessStops).  sharpness=0 → 1.0 (neutral),
 * sharpness=2 → 0.25, sharpness=4 → 0.0625.
 */
TEST(FSR3Rcas, Sharpness_Exp2) {
    RHI::EasuCon k0 = RHI::makeRcasCon(0.0f);
    EXPECT_FLOAT_EQ(asFloat(k0.c[12]), 1.0f);   // exp2(0)
    EXPECT_EQ(k0.c[0], 0u);  // const0 untouched by RCAS
    EXPECT_EQ(k0.c[1], 0u);

    RHI::EasuCon k2 = RHI::makeRcasCon(2.0f);
    EXPECT_FLOAT_EQ(asFloat(k2.c[12]), 0.25f);  // exp2(-2)

    RHI::EasuCon k4 = RHI::makeRcasCon(4.0f);
    EXPECT_FLOAT_EQ(asFloat(k4.c[12]), 0.0625f); // exp2(-4)
}

/**
 * RCAS must not touch the EASU-only slots (con0..con2, con3.y/z/w).
 * Only const3.x (c[12]) should be non-zero.
 */
TEST(FSR3Rcas, OnlyConst3xIsSet) {
    RHI::EasuCon k = RHI::makeRcasCon(1.5f);
    EXPECT_NE(asFloat(k.c[12]), 0.0f);  // has sharpness value
    for (int i = 0; i < 12; ++i) {
        EXPECT_EQ(k.c[i], 0u) << "RCAS clobbered con slot " << i;
    }
    EXPECT_EQ(k.c[14], 0u);
    EXPECT_EQ(k.c[15], 0u);
}

// =============================================================================
// FSR3 default-disabled assertion
// =============================================================================

/**
 * FSR3 is NOT enabled by default — there is no global FSR3_ENABLED /
 * ENABLE_FSR3 / USE_FSR macro in the codebase (outside the SDK headers).
 * The FSR3 EASU/RCAS/Temporal passes are always AVAILABLE in the Vulkan RHI
 * but are only invoked when the application explicitly calls
 * RHI::renderOffscreenSceneEasu() (Phase 4).  This test documents that fact:
 * a clean build of RHIMath.h must NOT define any FSR3_ON / FSR3_ENABLED
 * symbol that would turn the upscale on implicitly.
 */
TEST(FSR3Default, NotEnabledByDefault) {
#if defined(FSR3_ENABLED) || defined(ENABLE_FSR3) || defined(USE_FSR3) || \
    defined(FSR3_ON) || defined(FSR3_ACTIVE)
#error "FSR3 was enabled by a macro — it should be available but NOT enabled by default"
#endif
    // The constants functions exist and compute correctly (tested above), but
    // that does NOT mean FSR3 upscaling is active.  Verify no enable bit leaks
    // through the EASU constants themselves (con3.x is 0 for EASU, non-zero
    // for RCAS — the shader reads con3.x to know it has valid data, but there
    // is no separate "enable" dword).
    RHI::EasuCon easu = RHI::makeEasuCon(64, 64, 128, 128, 0, 0);
    // con3.x = 0 means EASU data is present (not an enable flag).
    EXPECT_FLOAT_EQ(asFloat(easu.c[12]), 0.0f);
    // No compile-time "FSR3 is on" path: the test compiled successfully, which
    // means RHIMath.h has no unconditional FSR3 activation.
    SUCCEED();
}
