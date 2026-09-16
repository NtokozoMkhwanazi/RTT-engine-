#version 430 core
// ============================================================================
// Gaussian Blur — 13-tap separable blur for bloom
// ============================================================================

out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D uImage;
uniform vec2 uDirection;  // (1/w, 0) for horizontal, (0, 1/h) for vertical

const float weight[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

void main() {
    vec3 result = texture(uImage, TexCoords).rgb * weight[0];
    vec2 tex_offset = uDirection;
    for (int i = 1; i < 5; ++i) {
        result += texture(uImage, TexCoords + tex_offset * i).rgb * weight[i];
        result += texture(uImage, TexCoords - tex_offset * i).rgb * weight[i];
    }
    FragColor = vec4(result, 1.0);
}
