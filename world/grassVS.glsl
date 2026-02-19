#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;

out vec3 FragPos;
out vec3 Color;
out vec2 TexCoord;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform float time;

void main() {
    vec4 worldPos = model * vec4(aPos, 1.0);
    FragPos = worldPos.xyz;
    Color = aColor;
    TexCoord = aPos.xz * 0.1;
    
    // Simple wind animation for grass
    float wind = sin(time * 2.0 + aPos.x * 0.5) * 0.02;
    worldPos.x += wind;
    worldPos.z += wind * 0.5;
    
    gl_Position = projection * view * worldPos;
}
