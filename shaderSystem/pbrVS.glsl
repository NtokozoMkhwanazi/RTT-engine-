#version 430 core
// ============================================================================
// PBR Vertex Shader — outputs world position, normal, UV, light-space pos,
// and model matrix (for normal mapping + CSM)
// ============================================================================

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 3) in vec3 aInstancePos;    // per-instance offset
layout(location = 4) in vec4 aInstanceColor;  // per-instance tint
layout(location = 5) in mat4 aModel;          // per-instance transform

uniform mat4 view;
uniform mat4 projection;

out vec2 TexCoords;
out vec3 FragPos;
out vec3 Normal;
out vec4 InstanceColor;
out mat4 FragModel;

void main() {
    vec4 worldPos = aModel * vec4(aPos, 1.0);
    FragPos = worldPos.xyz;
    TexCoords = aTexCoords;
    InstanceColor = aInstanceColor;
    FragModel = aModel;

    // Normal matrix (handles non-uniform scaling)
    Normal = mat3(transpose(inverse(aModel))) * aNormal;

    gl_Position = projection * view * worldPos;
}
