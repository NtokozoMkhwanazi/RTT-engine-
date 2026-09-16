#version 430 core
// --- Forward+ tile lighting (suggestions.txt #2) ---
// Per-fragment light loop over the cluster list in the BinResult SSBOs.
// GPULightData mirrors lighting/LightData.h (std430, 64-byte slots) so the
// same CPU light data feeds the deferred SSBO (#1) and this forward path.
layout(location = 0) out vec4 FragColor;

// Shading point (fullscreen quad -> a single fixed world space + normal).
layout(location = 0) uniform vec3 uWorldPos;
layout(location = 1) uniform vec3 uNormal;
layout(location = 2) uniform vec3 uAlbedo;
layout(location = 3) uniform float uAmbient;

struct GPULightData {
    vec3 position;       // point/spot position, or directional sun vector
    uint type;           // 0 = directional, 1 = point, 2 = spot
    vec3 direction;
    float intensity;
    vec3 color;
    float constant;
    float linear;
    float quadratic;
    float cutOff;
    float outerCutOff;
};

// SSBO 0: light storage (matches LightUBO in lighting/LightData.h).
layout(std430, binding = 0) buffer LightBlock {
    uint        lightCount;
    GPULightData lights[8];
};
// SSBO 1/2/3: per-cluster light lists (BinResult: offsets/counts/indices).
layout(std430, binding = 1) buffer TileCounts  { uint tileCounts[1]; };
layout(std430, binding = 2) buffer TileOffsets { uint tileOffsets[1]; };
layout(std430, binding = 3) buffer IndexBuffer  { uint lightIndices[64]; };

void main() {
    const uint tile  = 0u;
    const uint off   = tileOffsets[tile];
    const uint count = min(tileCounts[tile], 64u);

    vec3 lighting = vec3(0.0);
    for (uint k = 0u; k < count; ++k) {
        const uint li = lightIndices[off + k];
        if (li >= lightCount) break;
        const GPULightData L = lights[li];

        if (L.type == 0u) {                              // directional
            const vec3 Ld = normalize(-L.direction);
            lighting += L.color * L.intensity * max(dot(uNormal, Ld), 0.0);
        } else {                                         // point / spot: attenuation
            const vec3 toLight   = L.position - uWorldPos;
            const float dist      = length(toLight);
            const vec3  Ld        = toLight / dist;
            const float attenuation = 1.0 / (L.constant + L.linear * dist + L.quadratic * dist * dist);
            lighting += L.color * L.intensity * attenuation * max(dot(uNormal, Ld), 0.0);
        }
    }

    const vec3 color = uAlbedo * (uAmbient + lighting);
    FragColor = vec4(color, 1.0);
}
