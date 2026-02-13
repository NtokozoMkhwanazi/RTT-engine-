#version 330 core
in vec3 TexCoords;
out vec4 FragColor;
uniform samplerCube daySkybox;
uniform samplerCube nightSkybox;
uniform float blendFactor; // 0 = day, 1 = night
void main()
{
    vec4 dayColor   = texture(daySkybox, TexCoords);
    vec4 nightColor = texture(nightSkybox, TexCoords);
    FragColor = mix(dayColor, nightColor, blendFactor);
}
