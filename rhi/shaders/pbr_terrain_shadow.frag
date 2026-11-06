#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
// ============================================================================
// Vulkan Terrain Splat Fragment Shader + CSM shadow (GLSL 4.6)
// ----------------------------------------------------------------------------
// pbr_terrain.frag (4-layer blend + splat-POM + Cook-Torrance) verbatim, PLUS
// a cascade-0 sun-shadow factor (multiply Lo). Re-uses pbr_mesh.vert (emits
// vWorldPos@11), so NO depth reconstruction is needed for the shadow lookup.
//
// set=0 : bindless (CameraUBO b0, VertexBuffer b1, InstanceBuffer b2, tex b3)
// set=1 : ShadowUBO b0 (lightSpaceMatrices[3] + splits + sunDir) + atlas CIS b1
// push  : terrain PC {control,layer0..3} (FRAG, 20B — within the shared
//         m_shadowPbrPipelineLayout 32B push range, same as the static shadow).
// Forward-safe: only bound when scene.shadows is on; the default splat path
// (m_pbrTerrainPipeline + pbr_terrain.frag) is untouched -> 60/60 parity holds.
// ============================================================================

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

// B1-forward CSM (set=1, shared shadow set): ShadowUBO + sun depth atlas.
layout(set = 1, binding = 0, scalar) uniform ShadowUBO {
    mat4 lightSpaceMatrices[3];
    vec4 cascadeSplits;
    vec4 sunDirection;
} sh;
layout(set = 1, binding = 1) uniform sampler2DArrayShadow shadowMap;

layout(push_constant) uniform PushConstants {
    uint controlTexId;
    uint layer0;
    uint layer1;
    uint layer2;
    uint layer3;
} pc;

const float PI = 3.14159265359;
const vec3 kLightDir = normalize(vec3(0.5, 1.0, 0.3));
const vec3 kLightColor = vec3(1.0, 0.98, 0.92);

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
    const float maxLayers = 8.0;
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
    vec3 dX = dFdx(vWorldPos); vec3 dY = dFdy(vWorldPos);
    vec2 dT = dFdx(vUV); vec2 dB = dFdy(vUV);
    vec2 dx = dFdx(vUV); vec2 dy = dFdy(vUV);

    vec3 N_geom = normalize(vNormal);
    vec3 T = normalize(dX * dB.t - dY * dT.t);
    vec3 B = -normalize(cross(N_geom, T));
    mat3 TBN = mat3(T, B, N_geom);
    vec3 viewDirWorld = normalize(cam.camPos - vWorldPos);
    vec3 viewDirTS = normalize(transpose(TBN) * viewDirWorld);

    vec2 displacedUV = CalculateSplatPOM(vUV, viewDirTS, dx, dy);
    uint c = min(pc.controlTexId, K_MAX_BINDLESS_TEXTURES - 1u);
    vec4 w = texture(globalTextures[nonuniformEXT(c)], displacedUV).rgba;
    vec3 albedo =
        texture(globalTextures[nonuniformEXT(min(pc.layer0, K_MAX_BINDLESS_TEXTURES - 1u))], displacedUV).rgb * w.x +
        texture(globalTextures[nonuniformEXT(min(pc.layer1, K_MAX_BINDLESS_TEXTURES - 1u))], displacedUV).rgb * w.y +
        texture(globalTextures[nonuniformEXT(min(pc.layer2, K_MAX_BINDLESS_TEXTURES - 1u))], displacedUV).rgb * w.z +
        texture(globalTextures[nonuniformEXT(min(pc.layer3, K_MAX_BINDLESS_TEXTURES - 1u))], displacedUV).rgb * w.w;
    albedo *= vColor.rgb;

    vec3 N = N_geom; vec3 V = viewDirWorld;
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

    // --- B1-forward CSM (cascade 0). vWorldPos is interpolated by the vert. ---
    float shadowFactor = 1.0;
    vec4 sc = sh.lightSpaceMatrices[0] * vec4(vWorldPos, 1.0);
    sc /= sc.w; sc = sc * 0.5 + 0.5;
    if (sc.w > 0.0 && sc.z >= 0.0 && sc.z <= 1.0)
        shadowFactor = texture(shadowMap, vec4(sc.xy, 0.0, sc.z));
    Lo *= shadowFactor;

    vec3 skyCol = vec3(0.4, 0.5, 0.7), groundCol = vec3(0.3, 0.2, 0.1);
    vec3 ambient = fresnelSchlick(max(dot(N, V), 0.0), F0) * mix(groundCol, skyCol, N.y * 0.5 + 0.5) * 0.3;
    vec3 color = ambient + Lo;
    float dist = distance(vWorldPos, cam.camPos);
    float fogF = clamp(1.0 - exp(-cam.fogDensity * cam.fogDensity * dist * dist), 0.0, 1.0);
    color = mix(color, cam.fogColor, fogF);
    color = ACESFilm(color);
    color = pow(color, vec3(1.0 / 2.2));
    outColor = vec4(color, vColor.a);

    vec2 uvDisplacement = displacedUV - vUV;
    vec3 worldSpaceDisplacement = T * uvDisplacement.x + B * uvDisplacement.y;
    vec4 clipPosCurr = cam.viewProj * vec4(vWorldPos + worldSpaceDisplacement, 1.0);
    vec4 clipPosPrev = cam.prevViewProj * vec4(vWorldPos + worldSpaceDisplacement, 1.0);
    vec2 accurateCurrNdc = clipPosCurr.xy / clipPosCurr.w;
    vec2 accuratePrevNdc = clipPosPrev.xy / clipPosPrev.w;
    outVelocity = accurateCurrNdc - accuratePrevNdc;
}
