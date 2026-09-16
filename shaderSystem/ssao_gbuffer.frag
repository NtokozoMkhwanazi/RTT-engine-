#version 430 core
// ============================================================================
// G-Buffer Fragment Shader — outputs position + normal to MRT
// ============================================================================

layout(location = 0) out vec4 gPosition;
layout(location = 1) out vec4 gNormal;

in vec3 FragPosView;
in vec3 NormalView;
in vec2 TexCoords;

uniform sampler2D texture_diffuse1;

void main() {
    // Store view-space position (w=1 for depth comparison in SSAO)
    gPosition = vec4(FragPosView, 1.0);

    // Store view-space normal (normalized)
    gNormal = vec4(normalize(NormalView), 0.0);
}
