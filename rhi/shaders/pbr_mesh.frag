#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
// ============================================================================
// Vulkan PBR Mesh Fragment Shader with Parallax Occlusion Mapping (GLSL 4.6)
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
    layout(offset = 96) mat4 prevViewProj;
} cam;

layout(set = 0, binding = 3) uniform sampler2D globalTextures[];

layout(push_constant) uniform PushConstants {
    uint textureId;
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

const float PI = 3.14159265359;
const vec3 kLightDir = normalize(vec3(0.5, 1.0, 0.3));
const vec3 kLightColor = vec3(1.0, 0.98, 0.92);

// --- Parallax Occlusion Mapping Function (POM mip-selection shimmer fix) ---
// Uses textureGrad with baseline derivatives captured BEFORE ray-marching to
// prevent hardware automatic LOD from selecting tiny mips when UVs jump during
// the height-search loop.
vec2 CalculatePOM(vec2 initialUV, vec3 viewDirTS, uint texId, vec2 dx, vec2 dy) {
    // Control layer depth density based on looking angle
    const float minLayers = 12.0;
    const float maxLayers = 48.0;
    float numLayers = mix(maxLayers, minLayers, abs(dot(vec3(0.0, 0.0, 1.0), viewDirTS)));
    
    float layerDepth = 1.0 / numLayers;
    float currentLayerDepth = 0.0;
    
    // Scale height factor (Tweak this to control displacement amplitude)
    const float displacementScale = 0.04; 
    vec2 p = viewDirTS.xy / viewDirTS.z * displacementScale;
    vec2 deltaUV = p / numLayers;
    
    vec2 currentUV = initialUV;
    // Reads height map stored in the texture alpha channel (1.0 - alpha for inverse depth)
    float currentDepthMapValue = 1.0 - textureGrad(globalTextures[nonuniformEXT(texId)], currentUV, dx, dy).a;
    
    // Ray-march the layered heights
    while(currentLayerDepth < currentDepthMapValue) {
        currentUV -= deltaUV;
        currentDepthMapValue = 1.0 - textureGrad(globalTextures[nonuniformEXT(texId)], currentUV, dx, dy).a;
        currentLayerDepth += layerDepth;
    }
    
    // Linear interpolation step to wipe out micro-stepping artifacts
    vec2 prevUV = currentUV + deltaUV;
    float nextDepth = currentDepthMapValue - currentLayerDepth;
    float prevDepth = (1.0 - textureGrad(globalTextures[nonuniformEXT(texId)], prevUV, dx, dy).a) - (currentLayerDepth - layerDepth);
    
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
    // 1. Compute Tangent Space Vector using Screen-Space Derivatives
    vec3 dX = dFdx(vWorldPos);
    vec3 dY = dFdy(vWorldPos);
    vec2 dT = dFdx(vUV);
    vec2 dB = dFdy(vUV);

    // Capture baseline UV derivatives BEFORE POM warps coordinates
    vec2 dx = dFdx(vUV);
    vec2 dy = dFdy(vUV);

    vec3 N_geom = normalize(vNormal);
    vec3 T = normalize(dX * dB.t - dY * dT.t);
    vec3 B = -normalize(cross(N_geom, T));
    mat3 TBN = mat3(T, B, N_geom);

    // Transform camera view vector into texture tangent space
    vec3 viewDirWorld = normalize(cam.camPos - vWorldPos);
    vec3 viewDirTS = normalize(transpose(TBN) * viewDirWorld);

    // 2. Perform the POM UV offset displacement calculation
    vec2 displacedUV = CalculatePOM(vUV, viewDirTS, pc.textureId, dx, dy);

    // 3. Extract your material properties using the newly displaced UV coordinates
    vec3 albedo = texture(globalTextures[nonuniformEXT(pc.textureId)], displacedUV).rgb * vColor.rgb;
    vec3 N = N_geom; // Smooth mesh vertex normal baseline
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

    vec3 skyCol = vec3(0.4, 0.5, 0.7), groundCol = vec3(0.3, 0.2, 0.1);
    vec3 ambient = fresnelSchlick(max(dot(N, V), 0.0), F0) * mix(groundCol, skyCol, N.y * 0.5 + 0.5) * 0.3;
    vec3 color = ambient + Lo;

    float dist = distance(vWorldPos, cam.camPos);
    float fogF = clamp(1.0 - exp(-cam.fogDensity * cam.fogDensity * dist * dist), 0.0, 1.0);
    color = mix(color, cam.fogColor, fogF);

    color = ACESFilm(color);
    color = pow(color, vec3(1.0 / 2.2));
    outColor = vec4(color, vColor.a);
    
    // MOTION VECTORS: Compute POM-aware velocity.  The raw vertex-interpolated
    // vCurrNdc/vPrevPos do NOT account for the parallax UV displacement, so
    // the texture shifts on screen but the motion vector says "flat" — FSR3
    // reprojects to the wrong history pixel, leaving a black/transparent trail
    // where the displacement moved the surface.  We recompute the screen-space
    // delta using the displaced UVs projected back through world/clip space.
    vec2 uvDisplacement = displacedUV - vUV;
    vec3 worldSpaceDisplacement = T * uvDisplacement.x + B * uvDisplacement.y;
    vec4 clipPosCurr = cam.viewProj * vec4(vWorldPos + worldSpaceDisplacement, 1.0);
    vec4 clipPosPrev = cam.prevViewProj * vec4(vWorldPos + worldSpaceDisplacement, 1.0);
    vec2 accurateCurrNdc = clipPosCurr.xy / clipPosCurr.w;
    vec2 accuratePrevNdc = clipPosPrev.xy / clipPosPrev.w;
    outVelocity = accurateCurrNdc - accuratePrevNdc;
}

