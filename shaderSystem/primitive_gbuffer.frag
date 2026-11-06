#version 430 core
// ============================================================================
// Instanced G-Buffer Fragment Shader — 4-MRT for the deferred lighting pass.
// Layout mirrors shaderSystem/gbuffer_mesh.frag (gPosition/gNormal/gAlbedoMetal/
// gRoughAOEmissive); material arrives as per-instance varyings from
// primitive_gbuffer.vert. Only used for spawned ECS primitives (the terrain /
// model batch renderer uses its own shaders), so this is an isolated draw.
// ============================================================================

layout(location = 0) out vec4 gPosition;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gAlbedoMetal;      // RGB = albedo, A = metallic
layout(location = 3) out vec4 gRoughAOEmissive;  // R = roughness, G = ambient-occlusion

in vec3 FragPosView;
in vec3 NormalView;
in vec2 TexCoords;
in vec3 vColor;
in float vMetallic;
in float vRoughness;

void main() {
    // View-space position (w = 1 for depth comparison in SSAO)
    gPosition = vec4(FragPosView, 1.0);

    // Normalized view-space normal
    gNormal = vec4(normalize(NormalView), 0.0);

    // Flat material for the deferred lighting pass. AO defaults to 1.0
    // (procedural primitives have no AO map); the forward overlay still drives
    // the authored albedo/metallic/roughness on top of the deferred result.
    gAlbedoMetal     = vec4(vColor, vMetallic);
    gRoughAOEmissive = vec4(vRoughness, 1.0, 0.0, 0.0);
}
