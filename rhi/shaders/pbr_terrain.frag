#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
// ============================================================================
// Vulkan Terrain Splat Fragment Shader (RHI port of shaderSystem/terrain_splat's
// 4-layer blend). Blends 4 albedo(+height-alpha) layers by an RGBA control
// map, runs the SAME Cook-Torrance PBR + ACES + gamma as pbr_mesh.frag, and
// Parallax-Occlusion-Maps using the BLENDED layer height — so terrain that
// carries a heightmap in each layer's alpha (see InjectTerrainHeightmapAlpha
// in editor_main.cpp) displaces on the RHI path, just like terrain_splat.
//
// Reuses pbr_mesh.vert (build/rhi/pbr_mesh.vert.spv) for the vertex stage: the
// varyings (vColor/vNormal/vUV/vWorldPos/vCurrNdc/vPrevPos) and CameraUBO are
// identical, so pbr_mesh.vert is the vertex shader here too. The only difference
// from pbr_mesh is the PC layout (5 texture ids) and the 4-layer blend +
// splat-aware POM in main().
// ============================================================================

// APU safety (Ryzen 3700U / Vega 10 UMA): clamp bindless texture indices into
// [0, K_MAX_BINDLESS_TEXTURES) before nonuniformEXT sampling. Matches C++.
#define K_MAX_BINDLESS_TEXTURES 1024

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
    layout(offset = 96) mat4 prevViewProj;
} cam;

layout(set = 0, binding = 3) uniform sampler2D globalTextures[];

// Push constants for the splat path. Offsets mirror pbr_mesh's textureId@0
// (controlTexId reuses that slot); the four layer ids follow. The shared
// pipeline-layout push-constant range is 32 bytes (20 used here).
layout(push_constant) uniform PushConstants {
    uint controlTexId;   // RGBA blend weights for layer0..3 (== mesh.texturePixels)
    uint layer0;
    uint layer1;
    uint layer2;
    uint layer3;
} pc;

const float PI = 3.14159265359;
const vec3 kLightDir = normalize(vec3(0.5, 1.0, 0.3));
const vec3 kLightColor = vec3(1.0, 0.98, 0.92);

// --- PBR BRDF terms (bit-identical to pbr_mesh.frag) ---
float DistributionGGX(vec3 N, vec3 H, float r) {
    float a = r * r; float a2 = a * a;
    float d = max(dot(N, H), 0.0); d = d * d;
    return a2 / (PI * (d * (a2 - 1.0) + 1.0));
}
float GeometrySchlickGGX(float NdotV, float r) {
    float k = (r + 1.0); k = k * k / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-4);
}
float GeometrySmith(vec3 N, vec3 V, vec3 L, float r) {
    return GeometrySchlickGGX(max(dot(N, V), 0.0), r) * GeometrySchlickGGX(max(dot(N, L), 0.0), r);
}
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}
vec3 ACESFilm(vec3 x) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// --- 4-layer blended height (for POM) ---
// alpha=1 -> flat (height 0); alpha=0 -> full height. Mirrors pbr_mesh's
// `1.0 - texture(...).a` convention, extended to the weight-blended layers.
float splatHeight(vec2 uv, vec2 dx, vec2 dy) {
    uint c  = min(pc.controlTexId, K_MAX_BINDLESS_TEXTURES - 1u);
    vec4 w  = textureGrad(globalTextures[nonuniformEXT(c)], uv, dx, dy).rgba;
    vec4 ha = vec4(
        textureGrad(globalTextures[nonuniformEXT(min(pc.layer0, K_MAX_BINDLESS_TEXTURES - 1u))], uv, dx, dy).a,
        textureGrad(globalTextures[nonuniformEXT(min(pc.layer1, K_MAX_BINDLESS_TEXTURES - 1u))], uv, dx, dy).a,
        textureGrad(globalTextures[nonuniformEXT(min(pc.layer2, K_MAX_BINDLESS_TEXTURES - 1u))], uv, dx, dy).a,
        textureGrad(globalTextures[nonuniformEXT(min(pc.layer3, K_MAX_BINDLESS_TEXTURES - 1u))], uv, dx, dy).a);
    return 1.0 - dot(w, ha);
}

vec2 CalculateSplatPOM(vec2 initialUV, vec3 viewDirTS, vec2 dx, vec2 dy) {
    const float minLayers = 4.0;
    const float maxLayers = 8.0;  // bounded for UMA bandwidth (4 layer reads/step)
    float numLayers = mix(maxLayers, minLayers, abs(dot(vec3(0.0, 0.0, 1.0), viewDirTS)));
    float layerDepth   = 1.0 / numLayers;
    float currentLayerDepth = 0.0;
    const float displacementScale = 0.04;
    vec2 p = viewDirTS.xy / viewDirTS.z * displacementScale;
    vec2 deltaUV = p / numLayers;

    vec2 currentUV = initialUV;
    float currentDepthMapValue = splatHeight(currentUV, dx, dy);
    while (currentLayerDepth < currentDepthMapValue) {
        currentUV -= deltaUV;
        currentDepthMapValue = splatHeight(currentUV, dx, dy);
        currentLayerDepth += layerDepth;
    }
    vec2 prevUV = currentUV + deltaUV;
    float nextDepth = currentDepthMapValue - currentLayerDepth;
    float prevDepth = splatHeight(prevUV, dx, dy) - (currentLayerDepth - layerDepth);
    float weight = nextDepth / (nextDepth - prevDepth);
    return mix(currentUV, prevUV, weight);
}

void main() {
    // Tangent space (identical to pbr_mesh) — drives the height ray direction.
    vec3 dX = dFdx(vWorldPos);
    vec3 dY = dFdy(vWorldPos);
    vec2 dT = dFdx(vUV);
    vec2 dB = dFdy(vUV);
    vec2 dx = dFdx(vUV);
    vec2 dy = dFdy(vUV);

    vec3 N_geom = normalize(vNormal);
    vec3 T = normalize(dX * dB.t - dY * dT.t);
    vec3 B = -normalize(cross(N_geom, T));
    mat3 TBN = mat3(T, B, N_geom);

    vec3 viewDirWorld = normalize(cam.camPos - vWorldPos);
    vec3 viewDirTS = normalize(transpose(TBN) * viewDirWorld);

    // --- 4-layer splat ---
    vec2 displacedUV = CalculateSplatPOM(vUV, viewDirTS, dx, dy);
    uint c = min(pc.controlTexId, K_MAX_BINDLESS_TEXTURES - 1u);
    vec4 w = texture(globalTextures[nonuniformEXT(c)], displacedUV).rgba;   // layer weights
    vec3 albedo =
        texture(globalTextures[nonuniformEXT(min(pc.layer0, K_MAX_BINDLESS_TEXTURES - 1u))], displacedUV).rgb * w.x +
        texture(globalTextures[nonuniformEXT(min(pc.layer1, K_MAX_BINDLESS_TEXTURES - 1u))], displacedUV).rgb * w.y +
        texture(globalTextures[nonuniformEXT(min(pc.layer2, K_MAX_BINDLESS_TEXTURES - 1u))], displacedUV).rgb * w.z +
        texture(globalTextures[nonuniformEXT(min(pc.layer3, K_MAX_BINDLESS_TEXTURES - 1u))], displacedUV).rgb * w.w;
    albedo *= vColor.rgb;

    // --- Cook-Torrance PBR (identical to pbr_mesh) ---
    vec3 N = N_geom;
    vec3 V = viewDirWorld;
    float lum = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
    float metallic = clamp(lum * 0.3, 0.0, 0.8);
    float roughness = clamp(0.4 + (1.0 - lum) * 0.4, 0.04, 1.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 L = kLightDir, H = normalize(V + L);
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 spec = (D * G * F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 1e-4);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 Lo = (kD * albedo / PI + spec) * kLightColor * max(dot(N, L), 0.0);
    vec3 skyCol = vec3(0.4, 0.5, 0.7), groundCol = vec3(0.3, 0.2, 0.1);
    vec3 ambient = fresnelSchlick(max(dot(N, V), 0.0), F0) * mix(groundCol, skyCol, N.y * 0.5 + 0.5) * 0.3;
    vec3 color = ambient + Lo;
    float dist = distance(vWorldPos, cam.camPos);
    float fogF = clamp(1.0 - exp(-cam.fogDensity * cam.fogDensity * dist * dist), 0.0, 1.0);
    color = mix(color, cam.fogColor, fogF);
    color = ACESFilm(color);
    color = pow(color, vec3(1.0 / 2.2));
    outColor = vec4(color, vColor.a);

    // Motion vectors (POM-aware, identical to pbr_mesh).
    vec2 uvDisplacement = displacedUV - vUV;
    vec3 worldSpaceDisplacement = T * uvDisplacement.x + B * uvDisplacement.y;
    vec4 clipPosCurr = cam.viewProj * vec4(vWorldPos + worldSpaceDisplacement, 1.0);
    vec4 clipPosPrev = cam.prevViewProj * vec4(vWorldPos + worldSpaceDisplacement, 1.0);
    vec2 accurateCurrNdc = clipPosCurr.xy / clipPosCurr.w;
    vec2 accuratePrevNdc = clipPosPrev.xy / clipPosPrev.w;
    outVelocity = accurateCurrNdc - accuratePrevNdc;
}
