#version 430 core
// ============================================================================
// Deferred Lighting Fragment Shader — Multi-Light + SSAO + SSR + Light Accum
// ============================================================================
// Reads the G-Buffer and computes PBR Cook-Torrance for up to 8 lights.
// Supports optional shadow maps, SSAO, and SSR compositing.
// ============================================================================

out vec4 FragColor;

in vec2 TexCoords;

// G-Buffer samplers
uniform sampler2D gPosition;        // view-space position (RGBA16F)
uniform sampler2D gNormal;          // view-space normal (RGBA16F)
uniform sampler2D gAlbedoMetal;     // RGB = albedo, A = metallic (RGBA8)
uniform sampler2D gRoughAOEmissive; // R = roughness, G = AO, B = emissive (RGBA8)

// Shadow map (CSM cascade array)
uniform sampler2DArray uShadowMapArray;
uniform mat4  uLightSpaceMatrix[3];
uniform float uCascadeSplits[3];
uniform int   uShadowEnabled;

// SSAO
uniform sampler2D uSSAO;
uniform int uSSAOEnabled;

// --- Ambient / sky-light (driven by LightingEnvironment) ---------------------
uniform vec3  uGroundBounce;       // warm earth hemispheric tint
uniform vec3  uSkyTint;            // muted sky hemispheric tint
uniform float uAmbientStrength;    // ambient floor (ao/ssao lift toward full)
uniform vec3  uSunDirectionWS;    // world-space sun dir (surface -> sun)
uniform float uSkyLightStrength;   // warm fill on shaded sides

// SSR
uniform sampler2D uSSRTexture;
uniform int uSSREnabled;
uniform float uReflectionStrength;

// Light accumulation: single std430 SSBO (suggestions.txt #1). One buffer bind
// + one glBufferSubData replaces the old per-light glUniform4f glGetUniformLocation
// lookups. The GLSL struct mirrors GPULightData in lighting/LightData.h byte-for-byte
// (layout validated by static_assert on the C++ side); position.xyz / type /
// color / intensity carry exactly the values the old uLightPositions / uLightColors
// uniforms did, so the lighting math is unchanged (see render loop below).
struct GPULightData {
    vec3 position;   // point/spot apex, or surface->sun for directional
    uint type;       // 0 = directional, 1 = point, 2 = spot
    vec3 direction;  // cone axis (carried for the physical-cone pass; unused here)
    float intensity;
    vec3 color;      // linear RGB
    float constant;  // attenuation coeffs (carried; unused by this pass)
    float linear;
    float quadratic;
    float cutOff;
    float outerCutOff;
};
layout(std430, binding = 0) buffer LightBlock {
    uint lightCount;
    GPULightData lights[];
};

uniform vec3 uCameraPos;
uniform mat4 uProjection;
uniform mat4 uView;

const float PI = 3.14159265359;

// ---- GGX Normal Distribution Function --------------------------------------
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

// ---- Schlick-GGX Geometry Function -----------------------------------------
float GeometrySchlickGGX(float NdotV, float roughness) {
    float k = (roughness + 1.0);
    k = (k * k) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

// ---- Fresnel (Schlick) -----------------------------------------------------
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ---- CSM Shadow Calculation ------------------------------------------------
float ShadowCalculationCSM(vec3 fragPosWorld, vec3 N, vec3 L) {
    vec4 fragPosView4 = uView * vec4(fragPosWorld, 1.0);
    float fragDepth = -fragPosView4.z;

    int cascadeIndex = 2;
    for (int i = 0; i < 3; ++i) {
        if (fragDepth < uCascadeSplits[i]) {
            cascadeIndex = i;
            break;
        }
    }

    vec4 fragPosLightSpace = uLightSpaceMatrix[cascadeIndex] * vec4(fragPosWorld, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 1.0) return 0.0;

    float currentDepth = projCoords.z;
    float bias = max(0.005 * (1.0 - dot(N, L)), 0.001);

    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(uShadowMapArray, 0));
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            float pcfDepth = texture(uShadowMapArray,
                vec3(projCoords.xy + vec2(x, y) * texelSize, float(cascadeIndex))).r;
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
        }
    }
    shadow /= 25.0;
    return shadow;
}

void main() {
    // Sample G-Buffer
    vec3 fragPos = texture(gPosition, TexCoords).xyz;
    vec3 normal  = normalize(texture(gNormal, TexCoords).xyz);

    vec4 albedoMetal = texture(gAlbedoMetal, TexCoords);
    vec3 albedo  = albedoMetal.rgb;
    float metallic = albedoMetal.a;

    vec4 roughAOEmissive = texture(gRoughAOEmissive, TexCoords);
    float roughness = roughAOEmissive.r;
    float ao = roughAOEmissive.g;
    vec3 emissive = vec3(roughAOEmissive.b);

    // Background pixels
    if (length(normal) < 0.1) {
        discard;
    }

    // View direction (in view space, camera is at origin)
    vec3 V = normalize(-fragPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // ---- Accumulate direct lighting from all lights ------------------------
    vec3 Lo = vec3(0.0);
    int count = int(clamp(lightCount, 1u, 8u)); // CPU guarantees >=1 via fallback

    for (int i = 0; i < count; ++i) {
        uint lightType = lights[i].type;
        float lightIntensity = lights[i].intensity;
        vec3 lightColor = lights[i].color * lightIntensity;

        vec3 L;
        vec3 radiance;
        float shadow = 0.0;

        if (lightType == 0u) {
            // Directional light (type = 0). lights[i].position.xyz is the
            // canonical surface->sun direction (== LightingEnvironment sun),
            // so L = +that vector: flat horizontal surfaces now catch light
            // (previously negated to sunshine, leaving flat mesh tops dark).
            L = normalize(lights[i].position.xyz);
            radiance = lightColor;

            // CSM shadow for the main directional light
            if (uShadowEnabled == 1) {
                shadow = ShadowCalculationCSM(fragPos, normal, L);
            }
        } else {
            // Point light (type = 1) or spot light (type = 2)
            vec3 lightVec = lights[i].position.xyz - fragPos;
            float distance = length(lightVec);
            L = normalize(lightVec);

            // Inverse-square attenuation with radius falloff
            float attenuation = 1.0 / (distance * distance + 1.0);
            attenuation *= lightIntensity;
            radiance = lightColor * attenuation;

            // Spot light cone attenuation
            if (lightType == 2u) {
                float spotCos = dot(L, normalize(lights[i].position.xyz));
                float spotAtt = smoothstep(0.9, 0.5, 1.0 - spotCos);
                radiance *= spotAtt;
            }
        }

        vec3 H = normalize(V + L);

        // Cook-Torrance BRDF
        float D = DistributionGGX(normal, H, roughness);
        float G = GeometrySmith(normal, V, L, roughness);
        vec3  F = fresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3 numerator   = D * G * F;
        float denominator = 4.0 * max(dot(normal, V), 0.0) * max(dot(normal, L), 0.0) + 0.0001;
        vec3 specular = numerator / denominator;

        vec3 kS = F;
        vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

        float NdotL = max(dot(normal, L), 0.0);

        Lo += (kD * albedo / PI + specular) * radiance * NdotL * (1.0 - shadow);
    }

    // ---- IBL Hemisphere Ambient with SSAO ----------------------------------
    float ssao = 1.0;
    if (uSSAOEnabled == 1) {
        ssao = texture(uSSAO, TexCoords).r;
        ssao = clamp(ssao, 0.0, 1.0);
    }

    // Hemisphere: environment-driven ground bounce + muted sky (the same
    // LightingEnvironment the terrain reads), so meshes and terrain share one
    // ambient definition instead of drifting apart.
    vec3 skyColor = uSkyTint;
    vec3 groundColor = uGroundBounce;
    float upFactor = normal.y * 0.5 + 0.5;
    vec3 iblAmbient = mix(groundColor, skyColor, upFactor) *
                      (uAmbientStrength + (1.0 - uAmbientStrength) * ao * ssao);
    // Warm sky-light fill on the shaded side backs (keeps mesh undersides from
    // going flat-blue; mirrors the terrain splat shader's fill). Driven by the
    // canonical world-space sun direction (surface -> sun), so it is always
    // consistent with the key light regardless of how uLightPositions is packed.
    float meshShade = 1.0 - max(dot(normal, uSunDirectionWS), 0.0);
    vec3 skyLight = vec3(0.60, 0.52, 0.42) * meshShade * uSkyLightStrength;
    iblAmbient += skyLight;

    vec3 ambient = fresnelSchlick(max(dot(normal, V), 0.0), F0) * iblAmbient * ao * ssao;
    vec3 color = ambient + Lo + emissive;

    // ---- SSR Compositing ---------------------------------------------------
    if (uSSREnabled == 1) {
        vec4 ssr = texture(uSSRTexture, TexCoords);
        vec3 reflection = ssr.rgb;
        float ssrIntensity = ssr.a * uReflectionStrength;
        // Blend SSR based on metallic (metals have stronger reflections)
        float metallicReflection = mix(0.04, 1.0, metallic);
        color += reflection * ssrIntensity * metallicReflection;
    }

    FragColor = vec4(color, 1.0);
}
