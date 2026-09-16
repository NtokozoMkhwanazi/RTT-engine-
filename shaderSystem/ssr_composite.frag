#version 430 core
// ============================================================================
// SSR Composite Fragment Shader
// ============================================================================
// Composites screen-space reflections onto the scene color.
// The SSR texture contains: RGB = reflection color, A = reflection intensity.
//
// Final = scene + reflection * intensity * reflectionStrength
// ============================================================================

out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D uSceneColor;    // original scene (HDR, from deferred lighting)
uniform sampler2D uSSRTexture;    // SSR result (RGBA: rgb=reflection, a=intensity)
uniform float uReflectionStrength; // global SSR intensity multiplier [0..1]

void main() {
    vec3 scene = texture(uSceneColor, TexCoords).rgb;
    vec4 ssr = texture(uSSRTexture, TexCoords);

    vec3 reflection = ssr.rgb;
    float intensity = ssr.a * uReflectionStrength;

    // Blend reflection using screen-space fresnel from the SSR pass
    vec3 result = scene + reflection * intensity;

    FragColor = vec4(result, 1.0);
}
