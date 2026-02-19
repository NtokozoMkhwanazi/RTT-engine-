#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;

out vec3 FragPos;
out vec3 Color;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform float time;

void main() {
    vec4 worldPos = model * vec4(aPos, 1.0);
    FragPos = worldPos.xyz;
    Color = aColor;
    
    // Slight wind sway for trees
    float sway = sin(time * 1.5 + aPos.y * 0.3) * (aPos.y * 0.01);
    worldPos.x += sway;
    worldPos.z += sway * 0.5;
    
    gl_Position = projection * view * worldPos;
}
