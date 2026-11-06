#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
// ============================================================================
// Deferred G-Buffer fragment stage (Vulkan RHI, GLSL 4.6)
// ----------------------------------------------------------------------------
// Writes MATERIAL properties to a 2-attachment G-Buffer (no lighting):
//   loc0 -> gAlbedo      (RGBA8)        : base color (albedo * instance color)
//   loc1 -> gNormalRough (RGBA16f)      : world-space normal (RGB) + roughness (A)
// The lighting pass (lighting.comp/frag) samples these and illuminates. This
// mirrors how shaderSystem's forward shaders derive albedo/normal/roughness,
// but defers the Cook-Torrance + ACES composite to a separate pass — closing
// the "Vulkan is forward-only" gap vs the GL deferred RenderPipeline.
//
// Geometry inputs / layout match pbr_mesh.vert exactly (same CameraUBO, same
// bindless textures, same 32-byte push constant: textureId@0).
// ============================================================================

// APU safety: clamp bindless texture indices before nonuniformEXT sampling.
#define K_MAX_BINDLESS_TEXTURES 1024

layout(location = 0) out vec4 gAlbedo;       // RGBA8
layout(location = 1) out vec4 gNormalRough;  // RGBA16f (RGB=world normal, A=roughness)

layout(location = 8) in vec4 vColor;
layout(location = 9) in vec3 vNormal;
layout(location = 10) in vec2 vUV;
layout(location = 11) in vec3 vWorldPos;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 viewProj;
    vec3 camPos;
    float fogDensity;
    vec3 fogColor;
    float pad;
    layout(offset = 96) mat4 prevViewProj;
    layout(offset = 160) vec2 jitterOffset;
} cam;

layout(set = 0, binding = 3) uniform sampler2D globalTextures[];

layout(push_constant) uniform PushConstants {
    uint textureId;
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

void main() {
    // Material albedo (matches pbr_mesh.frag: texture * instance color).
    uint safeAlbedoTex = min(pc.textureId, K_MAX_BINDLESS_TEXTURES - 1u);
    vec3 albedo = texture(globalTextures[nonuniformEXT(safeAlbedoTex)], vUV).rgb * vColor.rgb;

    // World-space geometric normal (pbr_mesh.frag line 137 baseline).
    vec3 N = normalize(vNormal);

    // Roughness derived identically to pbr_mesh.frag (line 142) so the
    // deferred composite lights a surface with the SAME roughness the forward
    // path would have used. (Material only — no lighting here.)
    float lum = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
    float roughness = clamp(0.4 + (1.0 - lum) * 0.4, 0.04, 1.0);

    gAlbedo      = vec4(albedo, 1.0);
    gNormalRough = vec4(N, roughness);
}
