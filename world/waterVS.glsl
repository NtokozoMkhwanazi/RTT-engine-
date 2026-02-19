#version 330 core
layout(location=0) in vec3 aPos;

out vec3 FragPos;
out vec2 TexCoord;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main() {
    vec4 worldPos = model * vec4(aPos, 1.0);
    FragPos = worldPos.xyz;
    TexCoord = aPos.xz * 0.05;  // Tiling for normal map
    gl_Position = projection * view * worldPos;
}
