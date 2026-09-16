/**
 * Light buffer unit tests (suggestions.txt #1).
 *
 * Verifies the std430 layout contract between CPU (`GPULightData` / `LightUBO`
 * in lighting/LightData.h) and the GLSL `layout(std430) buffer LightBlock` in
 * shaderSystem/deferred_lighting.frag is byte-for-byte correct, and that
 * LightingSystem::buildGpuLightData packs the engine's lights into that
 * buffer (with the directional-sun fallback when no lights are registered).
 *
 * These are GL-free; the GLSL itself is validated separately with `glslc`.
 */
#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cstdint>

#include "lighting/LightData.h"
#include "lighting/LightingSystem.h"
#include "lighting/LightingEnvironment.h"

namespace {
void expectVec3Eq(const glm::vec3& a, const glm::vec3& b) {
    EXPECT_FLOAT_EQ(a.x, b.x);
    EXPECT_FLOAT_EQ(a.y, b.y);
    EXPECT_FLOAT_EQ(a.z, b.z);
}
} // namespace

TEST(LightBuffer, GPULightDataLayoutIsStd430_64Bytes) {
    static_assert(sizeof(GPULightData) == 64, "GPULightData must be 64 bytes");
    static_assert(alignof(GPULightData) == 16, "std430 vec3 base alignment = 16");
    static_assert(offsetof(GPULightData, position)    ==  0);
    static_assert(offsetof(GPULightData, type)        == 12);
    static_assert(offsetof(GPULightData, direction)   == 16);
    static_assert(offsetof(GPULightData, intensity)   == 28);
    static_assert(offsetof(GPULightData, color)       == 32);
    static_assert(offsetof(GPULightData, constant)    == 44);
    static_assert(offsetof(GPULightData, linear)      == 48);
    static_assert(offsetof(GPULightData, quadratic)   == 52);
    static_assert(offsetof(GPULightData, cutOff)      == 56);
    static_assert(offsetof(GPULightData, outerCutOff)  == 60);

    // The UBO header: uint count @0, lights[] @16 (count + 12 pad).
    static_assert(offsetof(LightUBO, lightCount) == 0);
    static_assert(offsetof(LightUBO, lights)    == 16);
    static_assert(sizeof(LightUBO) == 16 + kMaxGpuLights * sizeof(GPULightData));
    SUCCEED();
}

TEST(LightBuffer, MakeGpuLightPacksAllFields) {
    auto pl = makeGpuLight(1u, glm::vec3(2, 3, 4), glm::vec3(0, 1, 0),
                           glm::vec3(1, 0.8f, 0.6f), 3.0f);
    EXPECT_EQ(pl.type, 1u);
    expectVec3Eq(pl.position, glm::vec3(2, 3, 4));
    expectVec3Eq(pl.direction, glm::vec3(0, 1, 0));
    expectVec3Eq(pl.color, glm::vec3(1, 0.8f, 0.6f));
    EXPECT_FLOAT_EQ(pl.intensity, 3.0f);
}

TEST(LightBuffer, MakeDirectionalLightCarriesSunTwice) {
    glm::vec3 sun(0.5f, 1.0f, 0.3f);
    auto d = makeDirectionalLight(sun, glm::vec3(1, 1, 1), 1.0f);
    EXPECT_EQ(d.type, 0u);
    expectVec3Eq(d.position, sun);   // == old uLightPositions[i].xyz
    expectVec3Eq(d.direction, sun);  // == old uLightPositions[i].xyz (directional)
    expectVec3Eq(d.color, glm::vec3(1, 1, 1));
    EXPECT_FLOAT_EQ(d.intensity, 1.0f);
}

TEST(LightBuffer, BuildGpuLightDataPacksLightsAndRespectsCap) {
    LightingSystem ls;
    ls.addLight(Light(LightType::POINT, glm::vec3(1, 2, 3), glm::vec3(0, 1, 0),
                      glm::vec3(1, 1, 1), 5.0f));
    ls.addLight(Light(LightType::SPOT, glm::vec3(4, 5, 6), glm::vec3(0, -1, 0),
                      glm::vec3(1, 0, 0), 2.0f));

    LightingEnvironment& env = LightingEnvironment::Instance();

    auto data = ls.buildGpuLightData(kMaxGpuLights, env); // #3: env passed, not Instance()'d inside
    ASSERT_EQ(data.size(), 2u);
    EXPECT_EQ(data[0].type, 1u);
    expectVec3Eq(data[0].position, glm::vec3(1, 2, 3));
    expectVec3Eq(data[0].color, glm::vec3(1, 1, 1));
    EXPECT_FLOAT_EQ(data[0].intensity, 5.0f);
    EXPECT_FLOAT_EQ(data[0].constant, 1.0f);
    EXPECT_FLOAT_EQ(data[0].linear, 0.09f);
    EXPECT_FLOAT_EQ(data[0].quadratic, 0.032f);
    EXPECT_FLOAT_EQ(data[0].cutOff, glm::cos(glm::radians(12.5f)));
    EXPECT_FLOAT_EQ(data[0].outerCutOff, glm::cos(glm::radians(15.0f)));
    EXPECT_EQ(data[1].type, 2u);
    expectVec3Eq(data[1].position, glm::vec3(4, 5, 6));

    // cap below the light count -> truncated to cap
    auto capped = ls.buildGpuLightData(1, env);
    ASSERT_EQ(capped.size(), 1u);
    EXPECT_EQ(capped[0].type, 1u);

    // cap == 0 means "use the full kMaxLights window" -> all lights returned
    auto noCap = ls.buildGpuLightData(0, env);
    EXPECT_EQ(noCap.size(), 2u);
}

TEST(LightBuffer, BuildGpuLightDataFallbackEmitsDirectionalSun) {
    // The empty-lights fallback reads LightingEnvironment::Instance()'s live
    // sun; snapshot/restore the shared singleton so this test can't pollute
    // other tests in the same process.
    LightingEnvironment& L = LightingEnvironment::Instance();
    const glm::vec3 sunDir  = L.sunDirection;
    const glm::vec3 sunColor= L.sunColor;
    const float     sunInten= L.sunIntensity;

    L.sunDirection = glm::vec3(0, 1, 0);
    L.sunColor     = glm::vec3(0.9f, 0.9f, 1.0f);
    L.sunIntensity = 3.0f;

    LightingSystem ls;                 // no lights registered
    auto data = ls.buildGpuLightData(kMaxGpuLights, L);
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data[0].type, 0u);
    expectVec3Eq(data[0].position, glm::vec3(0, 1, 0));  // == old uLightPositions[0].xyz
    expectVec3Eq(data[0].color, glm::vec3(0.9f, 0.9f, 1.0f)); // == old uLightColors.rgb
    EXPECT_FLOAT_EQ(data[0].intensity, 3.0f);             // == old uLightColors.a

    L.sunDirection = sunDir;
    L.sunColor     = sunColor;
    L.sunIntensity = sunInten;
}

// #3: buildGpuLightData MUST source its empty-lights sun from the env passed
// in, NOT from LightingEnvironment::Instance(). We prove it by handing a
// *private* LightingEnvironment (whose sun differs from the singleton's) and
// asserting the fallback light matches the private env. If the function still
// read Instance() internally, this would emit the singleton's sun instead.
TEST(LightBuffer, BuildGpuLightDataUsesPassedEnvNotSingleton) {
    LightingSystem ls;                 // no lights -> directional fallback

    LightingEnvironment privateEnv;
    privateEnv.sunDirection = glm::vec3(0.0f, 1.0f, 0.0f);
    privateEnv.sunColor     = glm::vec3(1.0f, 0.0f, 0.0f); // red -- not a real sun
    privateEnv.sunIntensity = 7.5f;

    auto data = ls.buildGpuLightData(2, privateEnv);
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data[0].type, 0u);
    expectVec3Eq(data[0].position, glm::vec3(0, 1, 0)); // the PRIVATE env's sun
    expectVec3Eq(data[0].color, glm::vec3(1, 0, 0));    // red, not the singleton's
    EXPECT_FLOAT_EQ(data[0].intensity, 7.5f);
}
