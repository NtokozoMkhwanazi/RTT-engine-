#version 330 core
out vec4 FragColor;
in vec3 FragPos;
in vec3 Color;

uniform vec3 cameraPos;

void main() {
    // Tree color with variation
    vec3 treeColor = Color;
    
    // Simple lighting
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    vec3 normal = normalize(cross(dFdx(FragPos), dFdy(FragPos)));
    float diff = max(dot(normal, lightDir), 0.0);
    
    vec3 ambient = 0.4 * treeColor;
    vec3 diffuse = diff * treeColor;
    
    // Distance fog
    float dist = distance(cameraPos, FragPos);
    float fogDensity = 0.003;
    float fogFactor = 1.0 / exp(dist * dist * fogDensity * fogDensity);
    fogFactor = clamp(fogFactor, 0.0, 1.0);
    
    vec3 fogColor = vec3(0.7, 0.75, 0.8);
    vec3 finalColor = mix(fogColor, ambient + diffuse, fogFactor);
    
    // Alpha for transparency around tree shape
    float alpha = 1.0;
    
    FragColor = vec4(finalColor, alpha);
}
