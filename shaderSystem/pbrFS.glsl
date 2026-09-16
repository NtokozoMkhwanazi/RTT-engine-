#version 430 core
// ============================================================================
// PBR Fragment Shader — Cook-Torrance BRDF + CSM + Material Maps + SSAO + POM
// ============================================================================

out vec4 FragColor;

in vec2 TexCoords;
in vec3 FragPos;
in vec3 Normal;
in vec4 InstanceColor;
in vec4 FragPosLightSpace;
in mat4 FragModel;

// ---- Material textures (bound per-mesh by the renderer) --------------------
uniform sampler2D texture_diffuse1;      // albedo
uniform sampler2D texture_normal1;       // normal map (RGB) + Height map (A)
uniform sampler2D texture_metallic1;     // metallic map (R channel)
uniform sampler2D texture_roughness1;    // roughness map (G channel)
uniform sampler2D texture_ao1;           // AO map (R channel)
uniform sampler2D texture_emissive1;     // emissive map

// ---- Shadow maps (CSM: 3-cascade array texture) ---------------------------
uniform sampler2DArray uShadowMapArray;  
uniform float uCascadeSplits[3];         
uniform mat4  uLightSpaceMatrix[3];      

// ---- SSAO -----------------------------------------------------------------
uniform sampler2D uSSAOMap;             

// ---- Ambient / sky-light (driven by LightingEnvironment) --------------------
uniform vec3  uGroundBounce;       
uniform vec3  uSkyTint;            
uniform float uAmbientStrength;    
uniform vec3  uSunDirectionWS;    
uniform float uSkyLightStrength;   

// ---- Material UBO (binding = 2) -------------------------------------------
layout(std140, binding = 2) uniform MaterialBlock {
    vec4  albedo;
    vec4  emissive;
    float metallic;
    float roughness;
    float ao;
    float padding;
} material;

uniform mat4 view;
uniform mat4 projection;
uniform vec3 uCameraPos;

// ---- Light buffer: shared GPULightData SSBO -------------------------------
struct GPULightData {
    vec3 position;       
    uint type;           
    vec3 direction;      
    float intensity;
    vec3 color;          
    float constant;      
    float linear;        
    float quadratic;     
    float cutOff;        
    float outerCutOff;   
};
layout(std430, binding = 0) buffer LightBlock {
    uint        lightCount;
    GPULightData lights[8];
};

const float PI = 3.14159265359;

// ---- Parallax Occlusion Mapping Ray-March Loop -----------------------------
vec2 CalculatePOM(vec2 initialUV, vec3 viewDirTS) {
    // Dynamically adjust ray count based on looking angle
    const float minLayers = 12.0;
    const float maxLayers = 48.0;
    float numLayers = mix(maxLayers, minLayers, abs(dot(vec3(0.0, 0.0, 1.0), viewDirTS)));
    
    float layerDepth = 1.0 / numLayers;
    float currentLayerDepth = 0.0;
    
    // Scale height factor (Adjust to fine-tune depth amplitude)
    const float displacementScale = 0.04; 
    vec2 p = viewDirTS.xy / viewDirTS.z * displacementScale;
    vec2 deltaUV = p / numLayers;
    
    vec2 currentUV = initialUV;
    // Samples height map from the alpha channel of your normal texture
    float currentDepthMapValue = 1.0 - texture(texture_normal1, currentUV).a;
    
    while(currentLayerDepth < currentDepthMapValue) {
        currentUV -= deltaUV;
        currentDepthMapValue = 1.0 - texture(texture_normal1, currentUV).a;
        currentLayerDepth += layerDepth;
    }
    
    // Linear interpolation for silky-smooth surface gradients
    vec2 prevUV = currentUV + deltaUV;
    float nextDepth = currentDepthMapValue - currentLayerDepth;
    float prevDepth = (1.0 - texture(texture_normal1, prevUV).a) - (currentLayerDepth - layerDepth);
    
    float weight = nextDepth / (nextDepth - prevDepth);
    return mix(currentUV, prevUV, weight);
}

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom  = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * vec3(GeometrySchlickGGX(NdotL, roughness)).r;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float ShadowCalculationCSM(vec3 fragPosWorld, vec3 N, vec3 L) {
    vec4 fragPosView4 = view * vec4(fragPosWorld, 1.0);
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
            float pcfDepth = texture(uShadowMapArray, vec3(projCoords.xy + vec2(x, y) * texelSize, float(cascadeIndex))).r;
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
        }
    }
    shadow /= 25.0;

    if (cascadeIndex > 0) {
        float blendDist = uCascadeSplits[cascadeIndex] - uCascadeSplits[cascadeIndex - 1];
        float blendFactor = (uCascadeSplits[cascadeIndex] - fragDepth) / blendDist;
        blendFactor = clamp(blendFactor, 0.0, 1.0);

        vec4 prevLightSpace = uLightSpaceMatrix[cascadeIndex - 1] * vec4(fragPosWorld, 1.0);
        vec3 prevProj = prevLightSpace.xyz / prevLightSpace.w;
        prevProj = prevProj * 0.5 + 0.5;

        float prevShadow = 0.0;
        for (int x = -2; x <= 2; ++x) {
            for (int y = -2; y <= 2; ++y) {
                float pcfDepth = texture(uShadowMapArray, vec3(prevProj.xy + vec2(x, y) * texelSize, float(cascadeIndex - 1))).r;
                prevShadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
            }
        }
        prevShadow /= 25.0;

        shadow = mix(prevShadow, shadow, blendFactor);
    }

    return shadow;
}

void main() {
    // 1. Reconstruct Tangent Space via Screen Derivatives for Material Alignment
    vec3 dX = dFdx(FragPos);
    vec3 dY = dFdy(FragPos);
    vec2 dT = dFdx(TexCoords);
    vec2 dB = dFdy(TexCoords);

    vec3 N_geom = normalize(Normal);
    vec3 T = normalize(dX * dB.t - dY * dT.t);
    vec3 B = -normalize(cross(N_geom, T));
    mat3 TBN = mat3(T, B, N_geom);

    // Transform camera view direction to texturing tangent space
    vec3 viewDirWorld = normalize(uCameraPos - FragPos);
    vec3 viewDirTS = normalize(transpose(TBN) * viewDirWorld);

    // 2. Compute displaced UV tracking coordinates via POM loop
    vec2 displacedUV = CalculatePOM(TexCoords, viewDirTS);

    // 3. Extract all PBR material maps using the new displaced UV coordinates
    vec3 albedo = texture(texture_diffuse1, displacedUV).rgb;
    if (albedo.r < 0.01 && albedo.g < 0.01 && albedo.b < 0.01)
        albedo = material.albedo.rgb;
    albedo *= InstanceColor.rgb;

    float metallic = material.metallic;
    vec4 metallicSample = texture(texture_metallic1, displacedUV);
    if (metallicSample.r > 0.01) metallic = metallicSample.r;

    float roughness = material.roughness;
    vec4 roughnessSample = texture(texture_roughness1, displacedUV);
    if (roughnessSample.r > 0.01) roughness = roughnessSample.r;
    roughness = clamp(roughness, 0.04, 1.0);

    float ao = material.ao;
    vec4 aoSample = texture(texture_ao1, displacedUV);
    if (aoSample.r > 0.01) ao = aoSample.r;

    vec3 emissive = material.emissive.rgb;
    vec3 emissiveTex = texture(texture_emissive1, displacedUV).rgb;
    if (length(emissiveTex) > 0.01) emissive = emissiveTex;

    // Normal mapping integration
    vec3 N = N_geom;
    vec3 normalSample = texture(texture_normal1, displacedUV).rgb;
    if (length(normalSample) > 0.1) {
        // Correctly orient normal map components using the full TBN matrix context
        vec3 tangentNormal = normalSample * 2.0 - 1.0;
        N = normalize(TBN * tangentNormal);
    }

    vec3 V = viewDirWorld;
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // ---- Accumulate direct lighting from all lights (GPULightData SSBO) ------
    vec3 Lo = vec3(0.0);
    int count = int(min(lightCount, 8u));
    for (int i = 0; i < count; ++i) {
        const GPULightData Ld = lights[i];
        vec3 lightColor = Ld.color * Ld.intensity;

        vec3 L; vec3 radiance; float shadow = 0.0;
        if (Ld.type == 0u) {
            L = normalize(Ld.position);
            radiance = lightColor;
            shadow = ShadowCalculationCSM(FragPos, N, L);
        } else {
            vec3 toLight = Ld.position - FragPos;
            float dist  = length(toLight);
            L = toLight / dist;
            float attenuation = 1.0 / (Ld.constant + Ld.linear * dist + Ld.quadratic * dist * dist);
            radiance = lightColor * attenuation;
            if (Ld.type == 2u) {
                float cosTheta = dot(normalize(Ld.direction), normalize(FragPos - Ld.position));
                radiance *= smoothstep(Ld.outerCutOff, Ld.cutOff, cosTheta);
            }
        }

        vec3 H = normalize(V + L);

        float D = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, L, roughness);
        vec3  F = fresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3 numerator   = D * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;

        vec3 specular = numerator / denominator;
        float NdotL = max(dot(N, L), 0.0);

        // Energy conservation: kD is the fraction of light that is NOT
        // reflected specularly (dielectrics reflect ~4%, metals reflect
        // ~all via F, so kD -> 0 for metals).
        vec3 kD = vec3(1.0) - fresnelSchlick(max(dot(H, V), 0.0), F0);
        kD *= 1.0 - metallic;

        Lo += (kD * albedo / PI + specular) * radiance * NdotL * (1.0 - shadow);
    }

    // ---- Ambient (sky / ground / SSAO) ----
    vec3 ambient = uAmbientStrength * (uGroundBounce + uSkyTint * uSkyLightStrength) * ao;
    vec3 color = ambient + Lo;

    // Tone-map (Reinhard) + gamma correct
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    FragColor = vec4(color, 1.0);
}

