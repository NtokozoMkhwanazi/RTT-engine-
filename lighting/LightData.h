#pragma once
// ============================================================================
// LightData.h — GPU-aligned light transport types (suggestions.txt #1)
// ============================================================================
// `GPULightData` is the single, flat, 16-byte-aligned struct the engine packs
// light data into before a *single* buffer upload to the GPU (GL SSBO via
// std430, or Vulkan storage buffer). One std430 vec3 + its trailing scalar fill
// exactly one 16-byte slot, so each light is 4 such slots = 64 bytes — matching
// GLSL `layout(std430) buffer` byte-for-byte. The `static_assert`s are the
// real guarantee: if GLM is ever rebuilt with SIMD flags or a field order
// drifts, the build breaks instead of silently mis-rendering.
//
// The lighting/math fields (direction, constant, linear, quadratic, cutOff,
// outerCutOff) are carried alongside so a future shader pass can implement
// proper attenuation + spot cones without another layout change; the current
// `deferred_lighting` shader still reads only position.xyz / type / color /
// intensity (identical to the old `uLightPositions`/`uLightColors` uniforms).
// ============================================================================
#include <cstdint>
#include <glm/glm.hpp>

// Must match `GPULightData` in shaderSystem/deferred_lighting.frag exactly
// (member order + types) — std430 packs each (vec3, scalar) tail into 16B.
struct alignas(16) GPULightData {
    glm::vec3 position;      // xyz: point/spot apex, or surface->sun for directional
    uint32_t  type;          // 0 = directional, 1 = point, 2 = spot  (was uLightPositions.w)
    glm::vec3 direction;     // unit cone axis (spot) / sun vector (directional)
    float     intensity;     // (was uLightColors.a)
    glm::vec3 color;         // linear RGB       (was uLightColors.rgb)
    float     constant;      // attenuation constant
    float     linear;        // attenuation linear
    float     quadratic;     // attenuation quadratic
    float     cutOff;        // inner cone (cos, radians)
    float     outerCutOff;   // outer cone (cos, radians)
};

// Compile-time guarantee: CPU layout == GLSL std430 layout for GPULightData.
static_assert(sizeof(GPULightData) == 64, "GPULightData must be 64 bytes (4 x vec4)");
static_assert(offsetof(GPULightData, position)  ==  0);
static_assert(offsetof(GPULightData, type)      == 12);
static_assert(offsetof(GPULightData, direction) == 16);
static_assert(offsetof(GPULightData, intensity)  == 28);
static_assert(offsetof(GPULightData, color)     == 32);
static_assert(offsetof(GPULightData, constant)  == 44);
static_assert(offsetof(GPULightData, linear)     == 48);
static_assert(offsetof(GPULightData, quadratic)  == 52);
static_assert(offsetof(GPULightData, cutOff)     == 56);
static_assert(offsetof(GPULightData, outerCutOff)== 60);

// Max lights carried in a single SSBO upload (matches the shader's hard cap).
constexpr uint32_t kMaxGpuLights = 8;

// CPU mirror of `layout(std430) buffer LightBlock { uint lightCount; GPULightData lights[]; }`:
// the `uint` count sits at offset 0 (4 bytes + 12 std430 padding) so lights[0]
// lands on a 16-byte boundary — matching the GPU, and validated at compile time.
struct LightUBO {
    uint32_t    lightCount;                 // @0
    uint8_t     _pad0[12];                  // std430 scalar tail padding -> lights @16
    GPULightData lights[kMaxGpuLights];        // @16, stride 64
};
static_assert(offsetof(LightUBO, lights) == 16, "LightUBO.lights must align to 16");
static_assert(sizeof(LightUBO) == 16 + kMaxGpuLights * sizeof(GPULightData));

// --- Packers (shared by LightingSystem + DeferredRenderer -> single source) ---
// Defaults mirror Light's constructor so callers with partial data still emit a
// well-formed light. `type` is the raw numeric enum (0/1/2).
inline GPULightData makeGpuLight(uint32_t type,
                                 const glm::vec3& position,
                                 const glm::vec3& direction,
                                 const glm::vec3& color,
                                 float intensity,
                                 float constant     = 1.0f,
                                 float linear       = 0.09f,
                                 float quadratic    = 0.032f,
                                 float cutOff       = glm::cos(glm::radians(12.5f)),
                                 float outerCutOff  = glm::cos(glm::radians(15.0f))) {
    GPULightData d{};
    d.type          = type;
    d.position      = position;
    d.direction     = direction;
    d.intensity     = intensity;
    d.color         = color;
    d.constant      = constant;
    d.linear        = linear;
    d.quadratic     = quadratic;
    d.cutOff        = cutOff;
    d.outerCutOff   = outerCutOff;
    return d;
}

// Directional (sun) light: position.xyz and direction both carry the surface->sun
// vector, matching the old `uLightPositions[i].xyz` used for the key light.
inline GPULightData makeDirectionalLight(const glm::vec3& sunDirection,
                                         const glm::vec3& color,
                                         float intensity) {
    return makeGpuLight(0u, sunDirection, sunDirection, color, intensity);
}
