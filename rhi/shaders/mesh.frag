#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
// ============================================================================
// Bindless Mesh Fragment Shader (Vulkan RHI / GLSL 4.6)
// ============================================================================
// Same lambert lighting + exponential-squared fog math as the original
// shader (and the GL backend).  The albedo texture comes from the bindless
// globalTextures[] array, indexed by the push constant textureId.
// ============================================================================

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outVelocity;
layout(location = 8) in vec4 vColor;
layout(location = 9) in vec3 vNormal;
layout(location = 10) in vec2 vUV;
layout(location = 11) in vec3 vWorldPos;
layout(location = 12) in vec2 vCurrNdc;
layout(location = 13) in vec4 vPrevPos;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 viewProj;
    vec3 camPos;
    float fogDensity;
    vec3 fogColor;
    float pad;
} cam;

// Bindless texture array — all mesh textures + white default
layout(set = 0, binding = 3) uniform sampler2D globalTextures[];

layout(push_constant) uniform PushConstants {
    uint textureId;
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

// Normalized world-space light direction (0.4, 0.8, 0.3) — must match GL backend.
const vec3 kLightDir = normalize(vec3(0.4, 0.8, 0.3));

void main() {
    vec3 n = normalize(vNormal);
    float diff = max(dot(n, kLightDir), 0.0);
    // Sample from the bindless texture array using the push constant index.
    vec3 base = texture(globalTextures[nonuniformEXT(pc.textureId)], vUV).rgb;
    vec3 shaded = base * vColor.rgb * (0.5 + 0.5 * diff);
    // Exponential-squared distance fog (identical formula on both backends).
    float dist = distance(vWorldPos, cam.camPos);
    float fogF = clamp(1.0 - exp(-cam.fogDensity * cam.fogDensity * dist * dist), 0.0, 1.0);
    outColor = vec4(mix(shaded, cam.fogColor, fogF), vColor.a);
    // Phase 3b: screen-space motion vector (currNDC - prevNDC). Identical
    // expression in the GL inline shaders -> GL<->Vulkan velocity parity.
    outVelocity = vCurrNdc - (vPrevPos.xy / vPrevPos.w);
}
