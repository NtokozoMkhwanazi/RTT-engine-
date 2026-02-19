#version 330 core

out vec4 FragColor;
in vec2 TexCoords;
in vec3 FragPos;
in vec3 Normal;
in float DebugInfo;

uniform sampler2D texture_diffuse1;
uniform vec3 lightPos = vec3(10.0, 10.0, 10.0);
uniform vec3 viewPos;
uniform int uShowDebug;

void main()
{
    // DEBUG: Show position as color to verify rendering
    vec3 debugColor = (FragPos + 5.0) / 10.0;  // Map -5 to 5 -> 0 to 1
    vec3 color = debugColor;
    
    // Simple lighting for actual rendering
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);
    
    vec3 texColor = texture(texture_diffuse1, TexCoords).rgb;
    if (texColor.r < 0.01 && texColor.g < 0.01 && texColor.b < 0.01) {
        texColor = vec3(0.8, 0.6, 0.4);
    }
    
    vec3 litColor = (diff + 0.2) * texColor;
    
    // Use lit color normally, debug color for visualization
    if (uShowDebug == 1) {
        color = debugColor;
    } else {
        color = litColor;
    }

    FragColor = vec4(color, 1.0);
}

