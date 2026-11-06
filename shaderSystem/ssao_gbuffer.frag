#version 430 core
// ============================================================================
// G-Buffer Fragment Shader — outputs position + normal to MRT
// ============================================================================
// Material outputs (locations 2/3) feed the deferred lighting pass's albedo +
// material textures. The SSAO G-Buffer FBO only declares 2 color attachments,
// so rendering this shader there leaves locations 2/3 unwritten (an error-free
// no-op) — SSAO itself only reads position + normal.

layout(location = 0) out vec4 gPosition;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gAlbedoMetal;     // RGB = albedo, A = metallic
layout(location = 3) out vec4 gRoughAOEmissive;  // R = roughness, G = ambient-occlusion

in vec3 FragPosView;
in vec3 NormalView;
in vec2 TexCoords;

uniform sampler2D texture_diffuse1;

// Flat material overrides for procedural primitives. RenderPipeline sets these
// from each MeshComponent's color/metallic/roughness; defaults keep the existing
// SSAO/batch draw calls (which don't set them) rendering a neutral white.
uniform vec3  uAlbedoColor = vec3(1.0);
uniform float uMetallic    = 0.0;
uniform float uRoughness   = 0.5;

void main() {
    // Store view-space position (w=1 for depth comparison in SSAO)
    gPosition = vec4(FragPosView, 1.0);

    // Store view-space normal (normalized)
    gNormal = vec4(normalize(NormalView), 0.0);

    // Material properties for the deferred lighting pass (procedural primitives).
    gAlbedoMetal     = vec4(uAlbedoColor, uMetallic);
    gRoughAOEmissive = vec4(uRoughness, 1.0, 0.0, 0.0);  // AO=1 (no AO map)
}
