#version 460
// ============================================================================
// Deferred lighting — fullscreen composite fragment stage (Vulkan RHI, GLSL 4.6)
// ----------------------------------------------------------------------------
// Samples the G-Buffer (albedo + normal/roughness) produced by gbuffer.frag and
// applies a single directional light + ambient, now with Cascaded Shadow Maps
// (CSM) reconstructed from the scene depth + the sun's light-space matrices.
//
// This is the deferred lighting pass that the Vulkan RHI was missing (it
// rasterized forward-lit pbr_mesh directly; now the architecture mirrors the
// GL RenderPipeline's geometry-pass -> lighting-pass split). The math is kept
// close to pbr_mesh.frag's Lambertian term so deferred and forward agree on
// lighting, preserving the GL->Vulkan pixel-parity contract.
//
// CSM wiring:
//   binding 2 : sampler2DArray shadowMap  (3-layer depth atlas, depth-compare)
//   binding 3 : CameraUBO                  (viewProj for world-pos reconstruction)
//   binding 4 : sampler2D   depthTex      (G-Buffer/depth for reconstruction)
//   binding 5 : ShadowUBO                   (lightSpaceMatrices[3] + splits + sunDir)
// When no shadow atlas is bound (white fallback, offline parity path) the
// compare returns 1.0 -> fully lit, pixel-identical to the pre-CSM lighting.
// ============================================================================
layout(location = 0) in vec2 fUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D gAlbedoTex;
layout(set = 0, binding = 1) uniform sampler2D gNormalRoughTex;
layout(set = 0, binding = 2) uniform sampler2DArrayShadow shadowMap;
layout(set = 0, binding = 3) uniform CameraUBO {
    mat4 viewProj;
    vec3 camPos;
    float _a;
    vec3 _b;
    float _c;
    mat4 prevViewProj;
    vec2 jitterOffset;
} cam;
layout(set = 0, binding = 4) uniform sampler2D depthTex;
layout(set = 0, binding = 5) uniform ShadowUBO {
    mat4 lightSpaceMatrices[3];
    vec4 cascadeSplits;   // x,y,z = near/mid/far
    vec4 sunDirection;    // world-space sun direction (xyz)
} sh;

// Optional screen-space ambient-occlusion factor (Phase B2). Bound to a WHITE
// 1x1 texture (value 1.0) on the offline/parity path so ambient is untouched
// there (60/60 GL<->Vulkan deferred parity preserved); the deferred path binds
// the blurred SSAO output when scene.ssao is set.
layout(set = 0, binding = 6) uniform sampler2D uAo;

const vec3 kLightColor = vec3(1.0, 0.98, 0.92);

void main() {
    vec4 albedo = texture(gAlbedoTex, fUV);
    vec4 nr     = texture(gNormalRoughTex, fUV);
    vec3 N      = normalize(nr.xyz);

    // --- Reconstruct world position from the depth buffer (deferred has no
    // position attachment). depth is in [0,1] NDC -> remap to [-1,1]. ---
    float d     = texture(depthTex, fUV).r;
    vec4 ndc    = vec4(fUV * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    mat4 invVP  = inverse(cam.viewProj);
    vec4 worldW = invVP * ndc;
    vec3 worldPos = worldW.xyz / worldW.w;

    // --- Directional light + Cascaded Shadow Map (nearest cascade only for
    // now — split selection is the first cascade whose clip depth is in range;
    // otherwise the shadow compare resolves to 1.0 = fully lit). ---
    vec3 L = normalize(sh.sunDirection.xyz);
    float NdotL = max(0.0, dot(N, L));

    float shadowFactor = 1.0;
    for (int c = 0; c < 3; ++c) {
        vec4 sc = sh.lightSpaceMatrices[c] * vec4(worldPos, 1.0);
        sc /= sc.w;
        sc = sc * 0.5 + 0.5;              // [0,1] shadow-map UV + depth
        // Only accept the cascade whose projected Z is inside the frustum.
        if (sc.w > 0.0 && sc.z >= 0.0 && sc.z <= 1.0) {
            shadowFactor = texture(shadowMap, vec4(sc.xy, float(c), sc.z));
            break;
        }
    }

    vec3 skyCol    = vec3(0.4, 0.5, 0.7);
    vec3 groundCol = vec3(0.3, 0.2, 0.1);
    vec3 ambient   = mix(groundCol, skyCol, N.y * 0.5 + 0.5) * 0.3;

    // B2 SSAO: darken ambient where geometry is occluded. uAo defaults to
    // white (1.0) so the un-occluded lighting path is byte-identical.
    ambient *= texture(uAo, fUV).r;

    vec3 lit = ambient + albedo.rgb * kLightColor * NdotL * 0.94 * shadowFactor;
    outColor = vec4(lit, albedo.a);
}
