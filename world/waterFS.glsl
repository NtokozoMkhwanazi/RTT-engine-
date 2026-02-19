#version 330 core
out vec4 FragColor;
in vec3 FragPos;
in vec2 TexCoord;

uniform vec3 cameraPos;
uniform float time;
uniform vec3 waterColor;
uniform float waterLevel;

// Simple noise function
float noise(vec2 st) {
    return fract(sin(dot(st, vec2(12.9898, 78.233))) * 43758.5453123);
}

// Smooth noise
float smoothNoise(vec2 st) {
    vec2 i = floor(st);
    vec2 f = fract(st);
    f = f * f * (3.0 - 2.0 * f);
    
    float a = noise(i);
    float b = noise(i + vec2(1.0, 0.0));
    float c = noise(i + vec2(0.0, 1.0));
    float d = noise(i + vec2(1.0, 1.0));
    
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// FBM for wave detail
float fbm(vec2 st) {
    float value = 0.0;
    float amplitude = 0.5;
    
    for (int i = 0; i < 4; i++) {
        value += amplitude * smoothNoise(st);
        st *= 2.0;
        amplitude *= 0.5;
    }
    return value;
}

void main() {
    // Animated water surface
    vec2 uv = TexCoord;
    
    // Multiple wave layers
    float wave1 = fbm(uv * 2.0 + time * 0.5);
    float wave2 = fbm(uv * 4.0 - time * 0.3);
    float wave3 = fbm(uv * 8.0 + time * 0.2);
    
    float waveHeight = (wave1 + wave2 * 0.5 + wave3 * 0.25) / 1.75;
    
    // Fresnel effect
    vec3 viewDir = normalize(cameraPos - FragPos);
    vec3 normal = normalize(vec3(
        dFdx(FragPos.y + waveHeight * 0.5),
        1.0,
        dFdy(FragPos.y + waveHeight * 0.5)
    ));
    
    float fresnel = pow(1.0 - abs(dot(viewDir, normal)), 3.0);
    
    // Water color with depth variation
    vec3 deepColor = waterColor * 0.5;
    vec3 shallowColor = waterColor * 1.2;
    vec3 finalColor = mix(deepColor, shallowColor, waveHeight);
    
    // Add specular highlight
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);
    
    // Combine
    vec3 result = finalColor;
    result += vec3(0.8) * spec * 0.5;  // Sun specular
    result += fresnel * vec3(0.3);  // Fresnel rim
    
    FragColor = vec4(result, 0.85);  // Slightly transparent
}
