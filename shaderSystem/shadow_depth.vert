#version 430 core
// ============================================================================
// Shadow Map Depth Pass — renders scene from the light's perspective
// ============================================================================

layout(location = 0) in vec3 aPos;
layout(location = 6) in mat4 aModel;   // per-instance (or uniform)

uniform mat4 uLightSpaceMatrix;

void main() {
    gl_Position = uLightSpaceMatrix * aModel * vec4(aPos, 1.0);
}
