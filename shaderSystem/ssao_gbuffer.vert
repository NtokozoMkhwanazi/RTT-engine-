#version 430 core
// ============================================================================
// G-Buffer Vertex Shader — writes view-space position and normal for SSAO
// ============================================================================

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out vec3 FragPosView;
out vec3 NormalView;
out vec2 TexCoords;

void main() {
    vec4 viewPos = view * model * vec4(aPos, 1.0);
    FragPosView = viewPos.xyz;

    // Normal matrix in view space
    NormalView = mat3(transpose(inverse(view * model))) * aNormal;

    TexCoords = aTexCoords;
    gl_Position = projection * viewPos;
}
