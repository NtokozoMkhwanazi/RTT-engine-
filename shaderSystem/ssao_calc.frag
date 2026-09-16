#version 430 core
// ============================================================================
// SSAO Fragment Shader — hemisphere kernel occlusion sampling
// ============================================================================

out float FragColor;

in vec2 TexCoords;

uniform sampler2D gPosition;    // view-space position (from G-Buffer)
uniform sampler2D gNormal;      // view-space normal (from G-Buffer)
uniform sampler2D texNoise;     // 4×4 random rotation noise

uniform vec3  samples[64];      // hemisphere kernel
uniform int   kernelSize;
uniform float radius;           // sampling radius in view space
uniform float bias;             // depth bias to avoid self-occlusion
uniform vec2  noiseScale;      // screen dimensions / noise texture size

uniform mat4 projection;       // for re-projection

void main() {
    vec3 fragPos   = texture(gPosition, TexCoords).xyz;
    vec3 normal    = normalize(texture(gNormal, TexCoords).xyz);
    vec3 randomVec = normalize(texture(texNoise, TexCoords * noiseScale).xyz);

    // Create TBN matrix (Gram-Schmidt orthogonalization)
    vec3 tangent   = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN       = mat3(tangent, bitangent, normal);

    // Accumulate occlusion
    float occlusion = 0.0;
    for (int i = 0; i < kernelSize; ++i) {
        // Get sample position in view space
        vec3 samplePos = TBN * samples[i];
        samplePos = fragPos + samplePos * radius;

        // Project sample to screen space to get texture coordinates
        vec4 offset = projection * vec4(samplePos, 1.0);
        offset.xyz /= offset.w;
        offset.xyz = offset.xyz * 0.5 + 0.5;

        // Sample depth at the projected position
        float sampleDepth = texture(gPosition, offset.xy).z;

        // Range check + occlusion test
        float rangeCheck = smoothstep(0.0, 1.0, radius / abs(fragPos.z - sampleDepth));
        occlusion += (sampleDepth >= samplePos.z + bias ? 1.0 : 0.0) * rangeCheck;
    }

    occlusion = 1.0 - (occlusion / float(kernelSize));
    FragColor = occlusion;
}
