#version 330 core
out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoords;
in vec3 WorldFragPos;

uniform vec3 cameraPos;
uniform vec3 floorColor;
uniform float floorHeight;
uniform float gridSpacing;
uniform int showGrid;

void main() {
    // Grid pattern
    vec3 color = floorColor;
    
    if (showGrid == 1) {
        float gridThickness = 0.02;
        
        // Calculate grid lines
        vec2 grid = abs(fract(WorldFragPos.xz / gridSpacing - 0.5) - 0.5);
        vec2 derivative = fwidth(WorldFragPos.xz / gridSpacing);
        vec2 line = smoothstep(derivative, vec2(0.0), grid);
        
        // Combine lines
        float gridFactor = 1.0 - min(line.x, line.y);
        
        // Distance fade for grid
        float dist = length(WorldFragPos.xz - cameraPos.xz);
        float fade = 1.0 - smoothstep(20.0, 80.0, dist);
        
        // Apply grid with fade
        color = mix(color, vec3(0.4, 0.4, 0.45), gridFactor * fade * 0.6);
    }
    
    // Simple lighting
    vec3 lightDir = normalize(vec3(10.0, 10.0, 10.0));
    vec3 norm = normalize(Normal);
    float diff = max(dot(norm, lightDir), 0.0);
    
    vec3 ambient = 0.3 * color;
    vec3 diffuse = diff * color;
    
    // Distance fog
    float dist = length(WorldFragPos - cameraPos);
    float fogFactor = 1.0 - exp(-dist * 0.015);
    vec3 fogColor = vec3(0.15, 0.15, 0.2);
    
    vec3 finalColor = mix(vec3(ambient + diffuse), fogColor, fogFactor);
    
    FragColor = vec4(finalColor, 1.0);
}
