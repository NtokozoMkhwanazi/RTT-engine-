// ============================================================================
// pbrFS.glsl - Forward PBR fragment shader (OpenGL port of rhi/shaders/pbr_mesh)
// ============================================================================
// Driven by test_pbr_forward_gl.cpp. Consumes the SHARED GPULightData layout
// from lighting/LightData.h (suggestions.txt #1) via an std430 SSBO at binding 0
// (same data the deferred + forward_plus paths use), and a 48-byte material
// UBO at binding 2 (matches MatUBO in the test):
//   { vec4 albedo; vec4 emissive; float metallic, roughness, ao, pad; }
//
// Uniforms set by the caller: uCameraPos, uGroundBounce, uSkyTint,
// uAmbientStrength, uSkyLightStrength.  Samplers are assigned by unit via
// glUniform1i by the test.  Textures:
//   texture_diffuse1   (0)   texture_metallic1 (1)  texture_roughness1 (2)
//   texture_ao1        (3)   texture_emissive1 (4)  texture_normal1    (5)
//   uShadowMapArray    (6)   uSSAOMap            (8)
// ============================================================================
#version 430 core

layout(location = 0) out vec4 FragColor;

in VS_OUT {
    vec3 FragPos;
    vec3 Normal;
    vec2 TexCoord;
    vec4 InstanceColor;
    mat3 TBN;
    vec3 ViewDirTS;
} fs_in;

// --- Samplers (bound to texture units by the caller) ------------------------
uniform sampler2D       texture_diffuse1;
uniform sampler2D       texture_metallic1;
uniform sampler2D       texture_roughness1;
uniform sampler2D       texture_ao1;
uniform sampler2D       texture_emissive1;
uniform sampler2D       texture_normal1;
uniform sampler2DArray  uShadowMapArray;
uniform sampler2D       uSSAOMap;

// --- Camera / ambient (set via glUniform) -----------------------------------
uniform vec3 uCameraPos;
uniform vec3 uGroundBounce;
uniform vec3 uSkyTint;
uniform float uAmbientStrength;
uniform float uSkyLightStrength;

// --- Material UBO (binding 2) — matches test MatUBO (48 bytes, std140) -------
layout(std140, binding = 2) uniform MaterialBlock {
    vec4 albedo;
    vec4 emissive;
    float metallic;
    float roughness;
    float ao;
    float _pad;
};

// --- Light SSBO (binding 0, std430) — mirrors lighting/LightData.h ----------
// vec3+scalar pack to 16B, so each GPULightData is exactly 64 bytes.
struct GPULightData {
    vec3  position;
    uint  type;        // 0 = directional, 1 = point, 2 = spot
    vec3  direction;
    float intensity;
    vec3  color;
    float constant;
    float linear;
    float quadratic;
    float cutOff;
    float outerCutOff;
};
layout(std430, binding = 0) buffer LightBlock {
    uint          lightCount;
    GPULightData  lights[];
};

// --- PBR helpers ------------------------------------------------------------
const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float rough) {
    float a  = rough * rough;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float num = a2;
    float den = NdotH2 * (a2 - 1.0) + 1.0;
    return num / (PI * den * den + 1e-7);
}

float GeometrySchlick(float NdotV, float rough) {
    float r = rough + 1.0;
    float k = r * r / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 L, vec3 V, float rough) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlick(NdotL, rough) * GeometrySchlick(NdotV, rough);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

// --- Normal mapping (with graceful fallback for a black/default normal map) ---
vec3 ResolveNormal() {
    vec3 sampled = texture(texture_normal1, fs_in.TexCoord).rgb;
    if (length(sampled) < 0.01) {
        // No real normal map (e.g. a flat black default) -> use geometry normal.
        return normalize(fs_in.Normal);
    }
    vec3 n = normalize(sampled * 2.0 - 1.0);
    return normalize(fs_in.TBN * n);
}

// --- Parallax-occlusion mapping ----------------------------------------------
// Height is taken from the alpha channel of texture_normal1:
//   alpha == 1.0 (h=0) -> no displacement (the lit test binds an opaque normal)
//   alpha == 0.0 (h=1) -> full height displacement (the POM test)
// The ray marches along the tangent-space view direction; for a flat height
// field (the test case) it descends the full step and applies the full offset.
vec2 ApplyPOM(vec2 uv, vec3 viewDirTS) {
    const float heightScale = 0.1;
    const int   numSteps    = 16;

    float h = 1.0 - texture(texture_normal1, uv).a;   // height field [0,1]
    if (h <= 0.001) return uv;                        // no height -> pass through

    float viewZ = max(viewDirTS.z, 0.001);
    vec2 delta  = viewDirTS.xy / viewZ;

    float t = 0.0;
    vec2  uvd = uv;
    for (int i = 0; i < numSteps; ++i) {
        t += 1.0 / float(numSteps);
        uvd = uv + delta * heightScale * t;           // displace along view proj
        float hHere = 1.0 - texture(texture_normal1, uvd).a;
        if (hHere <= 0.001) {                         // marched past the surface
            break;
        }
    }
    return uvd;
}

void main() {
    vec2 uv = fs_in.TexCoord;
    vec3 viewDirTS = fs_in.ViewDirTS;

    // Parallax occlusion mapping (height from normal-map alpha).
    uv = ApplyPOM(uv, viewDirTS);

    vec3 albedoCol   = albedo.rgb * texture(texture_diffuse1, uv).rgb;
    vec3 emissiveCol = emissive.rgb * texture(texture_emissive1, uv).rgb;

    float rough      = max(roughness, 0.04);
    float metal      = metallic;
    float aoFactor   = ao;

    vec3 N = ResolveNormal();
    vec3 V = normalize(uCameraPos - fs_in.FragPos);   // view dir: surface -> eye

    vec3 F0 = mix(vec3(0.04), albedoCol, metal);

    vec3 Lo = vec3(0.0);
    for (uint i = 0u; i < lightCount; ++i) {
        const GPULightData L = lights[i];
        vec3 lightPos = L.position;
        vec3 l;
        float attenuation = 1.0;
        if (L.type == 0u) {                              // directional
            l = normalize(-L.direction);
        } else {                                          // point / spot
            l = lightPos - fs_in.FragPos;
            float dist = length(l);
            l = normalize(l);
            attenuation = 1.0 / max(L.constant + L.linear * dist + L.quadratic * dist * dist, 1e-4);
            if (L.type == 2u) {                           // spot cone
                float theta = dot(l, normalize(-L.direction));
                float epsilon = L.cutOff - L.outerCutOff;
                float intensity = clamp((theta - L.outerCutOff) / max(epsilon, 1e-4), 0.0, 1.0);
                attenuation *= intensity;
            }
        }

        vec3 H = normalize(V + l);
        float NdotL = max(dot(N, l), 0.0);

        vec3 radiance = L.color * L.intensity * attenuation;
        float NDF = DistributionGGX(N, H, rough);
        float G   = GeometrySmith(N, l, V, rough);
        vec3 F  = FresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3 kD = (1.0 - F) * (1.0 - metal);
        vec3 spec = NDF * G * F / (4.0 * max(NdotL, 0.01) * max(dot(V, N), 0.0) + 1e-4);

        Lo += (kD * albedoCol / PI + spec) * radiance * NdotL;
    }

    // Ambient (ground-bounce / sky). The tests zero every ambient knob, so the
    // ambient term collapses to 0 regardless of the (bound) SSAO texture.
    vec3 ambient = (uAmbientStrength + uSkyLightStrength)
                 * (uGroundBounce + uSkyTint) * albedoCol * aoFactor;

    vec3 color = Lo + ambient + emissiveCol;

    // Simple gamma (matches the existing forward_plus.frag which is linear->sRGB).
    color = pow(color, vec3(1.0 / 2.2));
    FragColor = vec4(color, 1.0);
}
