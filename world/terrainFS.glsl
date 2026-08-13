#version 330 core
out vec4 FragColor;
in vec3 FragPos;
in float Height;

uniform vec3 terrainColor;
uniform float waterLevel;
uniform vec3 cameraPos;

void main() {
    // Color based on height
    vec3 color;
    
    if (Height < waterLevel - 2.0f) {
        // Sand/beach
        color = vec3(0.76, 0.70, 0.50);
    } else if (Height < waterLevel + 5.0f) {
        // Grass
        color = vec3(0.2, 0.5, 0.2);
    } else if (Height < waterLevel + 20.0f) {
        // Rock
        color = vec3(0.4, 0.35, 0.3);
    } else {
        // Snow
        color = vec3(0.95, 0.95, 0.95);
    }
    
    // Simple lighting
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    vec3 normal = normalize(cross(dFdx(FragPos), dFdy(FragPos)));
    float diff = max(dot(normal, lightDir), 0.0);
    
    vec3 ambient = 0.3 * color;
    vec3 diffuse = diff * color;
    
    // Distance fog for atmosphere
    float dist = distance(cameraPos, FragPos);
    float fogDensity = 0.5;
    float fogFactor = 1.0 / exp(dist * dist * fogDensity * fogDensity);
    fogFactor = clamp(fogFactor, 0.0, 1.0);
    
    vec3 fogColor = vec3(0.7, 0.75, 0.8);  // Light blue-gray fog
    
    vec3 finalColor = mix(fogColor, ambient + diffuse, fogFactor);
    
    FragColor = vec4(finalColor, 1.0);
}
