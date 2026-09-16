#version 330 core
// ============================================================================
// G-Buffer Terrain Fragment Shader — deferred path
// ============================================================================
// Same material logic as the forward terrain shader (RVT atlas or live blend),
// but outputs to 4 MRT targets instead of computing final lighting:
//   - gPosition:   view-space position (RGBA16F)
//   - gNormal:     view-space normal (RGBA16F)
//   - gAlbedoMetal: albedo RGB + metallic in A (RGBA8)
//   - gRoughAOEmissive: roughness R + AO G + emissive B (RGBA8)
//
// The vertex shader is the SAME as the forward terrain shader (GPU heightmap
// displacement). Only the fragment output changes.
// ============================================================================

in float vHeight;
in vec3 vWorldPos;
in vec2 vTexCoord;

layout(location = 0) out vec4 gPosition;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gAlbedoMetal;
layout(location = 3) out vec4 gRoughAOEmissive;

uniform mat4 uView;
uniform mat4 uProjection;
uniform vec3 uViewPos;
uniform float uWaterLevel;

uniform sampler2D uMaterialAtlas;
uniform sampler2D uMaterialPbrAtlas;
uniform vec2 uAtlasPage;
uniform vec2 uChunkOrigin;
uniform float uChunkSize;
uniform float uUseAtlas;

// getTerrainPBR is provided by kTerrainMaterialGLSL (prepended before this body)
// It sets: albedo, normal, roughness, ao

void main() {
    vec3 geomNormal = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));

    vec3 albedo;
    vec3 normal;
    float roughness;
    float ao;

    if (uUseAtlas > 0.5) {
        vec2 local = (vWorldPos.xz - uChunkOrigin) / uChunkSize;
        vec2 uv = uAtlasPage + local * (128.0 / 4096.0);
        albedo = texture(uMaterialAtlas, uv).rgb;
        vec4 pbr = texture(uMaterialPbrAtlas, uv);
        normal = normalize(pbr.rgb * 2.0 - 1.0);
        roughness = clamp(pbr.a, 0.1, 1.0);
        ao = clamp(0.7 + 0.3 * normal.y, 0.35, 1.0);
    } else {
        float dist = distance(vWorldPos, uViewPos);
        float macroMix = smoothstep(40.0, 160.0, dist);
        getTerrainPBR(vHeight, geomNormal, vWorldPos, uWaterLevel,
                      macroMix, albedo, normal, roughness, ao);
    }

    // Transform to view space
    vec3 viewPos = (uView * vec4(vWorldPos, 1.0)).xyz;
    vec3 viewNormal = normalize(mat3(uView) * normal);

    // Output to G-Buffer
    gPosition = vec4(viewPos, 1.0);
    gNormal = vec4(viewNormal, 0.0);
    gAlbedoMetal = vec4(albedo, 0.0);  // terrain is dielectric (metallic=0)
    gRoughAOEmissive = vec4(roughness, ao, 0.0, 0.0);  // no emissive
}
