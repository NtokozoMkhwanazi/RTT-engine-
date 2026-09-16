#version 430 core
// ============================================================================
// SSAO Blur Fragment Shader — separable Gaussian blur for SSAO
// ============================================================================

out float FragColor;

in vec2 TexCoords;

uniform sampler2D uImage;
uniform vec2 uDirection;  // (1/w, 0) for horizontal, (0, 1/h) for vertical

void main() {
    float result = 0.0;

    // 9-tap Gaussian kernel
    float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    result += texture(uImage, TexCoords).r * weights[0];

    for (int i = 1; i < 5; ++i) {
        vec2 offset = uDirection * float(i);
        result += texture(uImage, TexCoords + offset).r * weights[i];
        result += texture(uImage, TexCoords - offset).r * weights[i];
    }

    FragColor = result;
}
