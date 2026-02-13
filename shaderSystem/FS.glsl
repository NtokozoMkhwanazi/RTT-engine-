#version 330 core

out vec4 FragColor;
in vec2 TexCoords;
in vec3 FragPos;
in vec3 Normal;

uniform sampler2D texture_diffuse1;
uniform vec3 lightPos = vec3(10.0, 10.0, 10.0);
uniform vec3 viewPos;

void main()
{
    // Simple diffuse lighting
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);

    vec3 diffuse = diff * texture(texture_diffuse1, TexCoords).rgb;

    // Simple ambient
    vec3 ambient = 0.2 * texture(texture_diffuse1, TexCoords).rgb;

    FragColor = vec4(diffuse + ambient, 1.0);
}

