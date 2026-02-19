#version 330 core
out vec4 FragColor;
in vec3 FragPos;
in vec3 Color;
in vec2 TexCoord;

uniform vec3 cameraPos;

void main() {
    // Grass color variation
    vec3 grassColor = Color;
    
    // Add some noise/variation
    float noise = fract(sin(dot(TexCoord, vec2(12.9898, 78.233))) * 43758.5453);
    grassColor *= (0.8 + noise * 0.4);
    
    // Distance fog
    float dist = distance(cameraPos, FragPos);
    float fogDensity = 0.003;
    float fogFactor = 1.0 / exp(dist * dist * fogDensity * fogDensity);
    fogFactor = clamp(fogFactor, 0.0, 1.0);
    
    vec3 fogColor = vec3(0.7, 0.75, 0.8);
    vec3 finalColor = mix(fogColor, grassColor, fogFactor);
    
    // Alpha for grass blade
    float alpha = 0.85;
    
    FragColor = vec4(finalColor, alpha);
}
