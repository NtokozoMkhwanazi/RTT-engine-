#version 430 core
// ============================================================================
// G-Buffer Mesh Fragment Shader — deferred path for general meshes
// ============================================================================
// Outputs to 4 MRT targets for the deferred lighting pass.
// Used with the PBR vertex shader (same vertex layout).
// ============================================================================

layout(location = 0) out vec4 gPosition;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gAlbedoMetal;
layout(location = 3) out vec4 gRoughAOEmissive;

in vec2 TexCoords;
in vec3 FragPos;
in vec3 Normal;
in vec4 InstanceColor;

uniform sampler2D texture_diffuse1;
uniform sampler2D texture_normal1;
uniform sampler2D texture_metallic1;
uniform sampler2D texture_roughness1;
uniform sampler2D texture_ao1;

uniform mat4 view;
uniform mat4 projection;

void main() {
    // Albedo from texture or instance color
    vec3 albedo = texture(texture_diffuse1, TexCoords).rgb;
    if (albedo.r < 0.01 && albedo.g < 0.01 && albedo.b < 0.01)
        albedo = vec3(0.8);
    albedo *= InstanceColor.rgb;

    // Normal mapping
    vec3 N = normalize(Normal);
    vec3 normalSample = texture(texture_normal1, TexCoords).rgb;
    if (length(normalSample) > 0.1) {
        N = normalize(normalSample * 2.0 - 1.0);
    }

    // Material properties
    float metallic = 0.0;
    vec4 metallicSample = texture(texture_metallic1, TexCoords);
    if (metallicSample.r > 0.01) metallic = metallicSample.r;

    float roughness = 0.5;
    vec4 roughnessSample = texture(texture_roughness1, TexCoords);
    if (roughnessSample.r > 0.01) roughness = roughnessSample.r;
    roughness = clamp(roughness, 0.04, 1.0);

    float ao = 1.0;
    vec4 aoSample = texture(texture_ao1, TexCoords);
    if (aoSample.r > 0.01) ao = aoSample.r;

    // View-space transforms
    vec3 viewPos = (view * vec4(FragPos, 1.0)).xyz;
    vec3 viewNormal = normalize(mat3(view) * N);

    // Output to G-Buffer
    gPosition = vec4(viewPos, 1.0);
    gNormal = vec4(viewNormal, 0.0);
    gAlbedoMetal = vec4(albedo, metallic);
    gRoughAOEmissive = vec4(roughness, ao, 0.0, 0.0);
}
