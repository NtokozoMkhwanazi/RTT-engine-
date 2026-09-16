#version 430 core

out vec4 FragColor;
in vec2 TexCoords;
in vec3 FragPos;
in vec3 Normal;
in vec3 vTangent;
in vec3 vBitangent;
in float DebugInfo;
in vec4 InstanceColor;

uniform sampler2D texture_diffuse1;
uniform sampler2D texture_normal1;       // flat (0.5,0.5,1.0) default -> no perturbation
uniform sampler2D texture_roughness1;    // grey (0.5) default
uniform sampler2D texture_ao1;           // white (1.0) default -> no darkening
uniform vec3 lightPos = vec3(10.0, 10.0, 10.0);
uniform vec3 viewPos;
uniform int uShowDebug;
// Ambient term (default 0.2 matches the old hardcoded value). Characters can
// raise it (AnimatedCharacter sets uAmbient) so dark materials stay visible.
uniform float uAmbient = 0.2f;

// Subsurface foliage scattering: sunlight bleeding through the thin backs of
// leaves. uSubsurface == 0 (default) leaves the legacy lit path untouched, so
// rocks / characters / UI geometry render exactly as before. >0 is set per
// leafy-plant model by SimpleWorldRenderer (tied to the wind-strength flag).
uniform float uSubsurface = 0.0f;
uniform vec3 uSubsurfaceColor = vec3(0.40, 0.24, 0.12);  // warm leaf transmission
uniform float uSubsurfacePower = 2.0f;

void main()
{
    // DEBUG: Show position as color to verify rendering
    vec3 debugColor = (FragPos + 5.0) / 10.0;  // Map -5 to 5 -> 0 to 1
    vec3 color = debugColor;
    
    // Simple lighting for actual rendering (per-pixel). Tangent-space normal
    // mapping: perturb the interpolated normal by the normal map. texture_normal1
    // is always bound (flat 0.5/0.5/1.0 default when a mesh has no real normal
    // map) -> sampled local (0,0,1) -> N unchanged, so legacy flat shading is
    // reproduced exactly. This breaks up the facetted "Minecraft" look on trees
    // and rocks using the normal maps the models ALREADY load.
    mat3 TBN = mat3(vTangent, vBitangent, normalize(Normal));
    vec3 sampledNormal = texture(texture_normal1, TexCoords).rgb;
    vec3 norm = normalize(TBN * (sampledNormal * 2.0 - 1.0));

    vec3 lightDir = normalize(lightPos - FragPos);
    vec3 viewDir  = normalize(viewPos - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);

    vec3 texColor = texture(texture_diffuse1, TexCoords).rgb;
    if (texColor.r < 0.01 && texColor.g < 0.01 && texColor.b < 0.01) {
        texColor = vec3(0.8, 0.6, 0.4);
    }

    // Per-instance tint (vegetation color variation). White when instancing
    // is disabled (characters) or when no tint is set.
    texColor *= InstanceColor.rgb;

    vec3 litColor = (diff + uAmbient) * texColor;

    // Ambient occlusion (white/1.0 default when no map -> no change) darkens
    // crevices, and a tight roughness-driven Blinn-Phong specular catches the
    // canonical sun on wet coastal rocks / glossy leaves. Both are mild on the
    // default grey roughness map, so legacy geometry is not noticeably darker.
    float ao = texture(texture_ao1, TexCoords).r;
    litColor *= mix(1.0, ao, 0.30);
    float rough = texture(texture_roughness1, TexCoords).r;
    float NdotH = max(dot(norm, normalize(lightDir + viewDir)), 0.0);
    float spec = pow(NdotH, 4.0 * (1.0 - rough) + 4.0) * (1.0 - rough) * 0.2;
    litColor += spec;

    // Subsurface scattering: sunlight bleeds through the thin BACKS of leaves.
    // Strongest where the normal faces AWAY from the light (back of leaf toward
    // the sun) and the eye looks through the leaf toward the light (view-
    // dependent). Gated so uSubsurface == 0 reproduces the legacy lit colour
    // exactly (no regression for non-foliage geometry).
    if (uSubsurface > 0.0) {
        float backTerm    = max(0.0, -dot(norm, lightDir));  // 1.0 at leaf back
        float viewGloss   = max(0.0, dot(viewDir, lightDir)); // 1.0 toward sun
        float thickness   = pow(backTerm, uSubsurfacePower);
        litColor += uSubsurface * thickness * viewGloss * uSubsurfaceColor;
    }

    // Use lit color normally, debug color for visualization
    if (uShowDebug == 1) {
        color = debugColor;
    } else {
        color = litColor;
    }

    FragColor = vec4(color, 1.0);
}

