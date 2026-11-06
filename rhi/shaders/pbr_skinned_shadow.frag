#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
// ============================================================================
// Skinned (LBS) PBR fragment + CSM shadow (Vulkan GLSL 4.6)
// ----------------------------------------------------------------------------
// pbr_mesh_shadow.frag adapted for the SKINNED pipeline: identical PBR/POM
// composite, but the shadow UBO + CSM depth atlas live on set=2 (set=1 is the
// skin bone/palette data, driven by pbr_skinned.vert). pbr_skinned.vert emits
// vWorldPos@11 (post-skin), so — like the static forward path — no depth
// reconstruction is needed for the shadow lookup.
//
// set=0 : bindless (CameraUBO b0, VertexBuffer b1, InstanceBuffer b2, tex b3)
// set=1 : per-mesh skin (boneIdx/Weight SSBO b0 + palette SSBO b1) — VERT only
// set=2 : ShadowUBO b0 (lightSpaceMatrices[3] + splits + sunDir) + atlas CIS b1
//         (the B1 shadow set, shared with the static forward shadow path).
// push  : {textureId@0} (FRAG) — matches pbr_mesh_shadow / pbr_skinned push.
// Forward-safe: only this fragment is shadowed; skinned meshes still route to
// the skinned vert stage unchanged.
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

// Skin-shadow set (set=2): ShadowUBO + the sun's depth atlas.
layout(set = 2, binding = 0, scalar) uniform ShadowUBO {
    mat4 lightSpaceMatrices[3];
    vec4 cascadeSplits;
    vec4 sunDirection;
} sh;
layout(set = 2, binding = 1) uniform sampler2DArrayShadow shadowMap;

layout(push_constant) uniform PushConstants {
    uint textureId;
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

const float PI = 3.14159265359;
const vec3 kLightDir = normalize(vec3(0.5, 1.0, 0.3));
const vec3 kLightColor = vec3(1.0, 0.98, 0.92);

vec2 CalculatePOM(vec2 initialUV, vec3 viewDirTS, uint texId, vec2 dx, vec2 dy) {
    const float minLayers = 4.0;
    const float maxLayers = 16.0;
    float numLayers = mix(maxLayers, minLayers, abs(dot(vec3(0.0, 0.0, 1.0), viewDirTS)));
    uint safeTexId = min(texId, K_MAX_BINDLESS_TEXTURES - 1u);
    float layerDepth = 1.0 / numLayers;
    float currentLayerDepth = 0.0;
    const float displacementScale = 0.04;
    vec2 p = viewDirTS.xy / viewDirTS.z * displacementScale;
    vec2 deltaUV = p / numLayers;
    vec2 currentUV = initialUV;
    float currentDepthMapValue = 1.0 - textureGrad(globalTextures[nonuniformEXT(safeTexId)], currentUV, dx, dy).a;
    while(currentLayerDepth < currentDepthMapValue) {
        currentUV -= deltaUV;
        currentDepthMapValue = 1.0 - textureGrad(globalTextures[nonuniformEXT(safeTexId)], currentUV, dx, dy).a;
        currentLayerDepth += layerDepth;
    }
    vec2 prevUV = currentUV + deltaUV;
    float nextDepth = currentDepthMapValue - currentLayerDepth;
    float prevDepth = (1.0 - textureGrad(globalTextures[nonuniformEXT(safeTexId)], prevUV, dx, dy).a) - (currentLayerDepth - layerDepth);
    float weight = nextDepth / (nextDepth - prevDepth);
    return mix(currentUV, prevUV, weight);
}

float DistributionGGX(vec3 N, vec3 H, float r) {
    float a = r * r; float a2 = a * a;
    float d = max(dot(N, H), 0.0); d = d * d;
    return a2 / (PI * (d * (a2 - 1.0) + 1.0) * (d * (a2 - 1.0) + 1.0));
}
float GeometrySchlickGGX(float NdotV, float r) {
    float k = (r + 1.0); k = k * k / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
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

    uint safeAlbedoTex = min(pc.textureId, K_MAX_BINDLESS_TEXTURES - 1u);
    vec2 displacedUV = CalculatePOM(vUV, viewDirTS, pc.textureId, dx, dy);

    vec3 albedo = texture(globalTextures[nonuniformEXT(safeAlbedoTex)], displacedUV).rgb * vColor.rgb;
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
    vec3 spec = (D * G * F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 Lo = (kD * albedo / PI + spec) * kLightColor * max(dot(N, L), 0.0);

    // --- B1-forward CSM (cascade 0) — same as pbr_mesh_shadow, set=2 here. ---
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
