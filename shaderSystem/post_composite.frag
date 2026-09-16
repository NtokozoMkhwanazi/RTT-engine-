#version 430 core
// ============================================================================
// Composite Fragment Shader — scene + bloom + SSAO → adaptive exposure →
//                             ACES tone map  (linear → linear LDR)
// ============================================================================
// The output is linear display-referred LDR in [0,1]. The single sRGB encode
// to the screen is performed by glEnable(GL_FRAMEBUFFER_SRGB) on the default
// framebuffer (see test.cpp), so this shader must NOT gamma-encode (a previous
// pow(1/2.2) here caused a double-gamma washout). Exposure is adaptive when
// uAutoExposure is true (see PostProcess auto-exposure), else manual uExposure.
// ============================================================================

out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D uScene;          // HDR scene color
uniform sampler2D uBloom;          // bloom blur texture
uniform sampler2D uSSAO;           // SSAO occlusion (optional, half-res)
uniform sampler2D uAvgLuminance;   // log-avg luminance (1x1 coarsest mip)

uniform float uBloomStrength;
uniform float uExposure;           // manual exposure (used when uAutoExposure == false)
uniform float uMinLum;             // floor on averaged luminance (auto path)
uniform float uMaxLum;             // ceiling on averaged luminance (auto path)
uniform bool  uSSAOEnabled;
uniform bool  uAutoExposure;

void main() {
    vec3 sceneColor = texture(uScene, TexCoords).rgb;
    vec3 bloomColor = texture(uBloom, TexCoords).rgb;

    // Composite bloom
    vec3 color = sceneColor + bloomColor * uBloomStrength;

    // Apply SSAO if available
    if (uSSAOEnabled) {
        vec2 ssaoUV = TexCoords;  // already half-res mapped
        float ssao = texture(uSSAO, ssaoUV).r;
        ssao = clamp(ssao, 0.0, 1.0);
        // Darken ambient; leave specular highlights untouched
        color *= mix(0.3, 1.0, ssao);
    }

    // Adaptive auto-exposure (Reinhard "auto key" over the log-average
    // luminance). The coarsest mip of the log-luminance texture holds the
    // log2 of the geometric-mean scene luminance; a near-infinite lod is
    // clamped by the GPU to that 1x1 level.
    float exposure;
    if (uAutoExposure) {
        float avgLogLum = textureLod(uAvgLuminance, TexCoords, 99.0).r;
        float avgLum    = exp2(clamp(avgLogLum, -16.0, 16.0));
        avgLum          = clamp(avgLum, uMinLum, uMaxLum);
        exposure        = 1.0 / (avgLum + 1.0);
    } else {
        exposure = uExposure;
    }
    color *= exposure;

    // ACES tone mapping (fitted curve — much better than Reinhard).
    // Output is LINEAR display-referred LDR in [0,1]. We deliberately do NOT
    // apply a gamma (pow 1/2.2) here: the final present to the default
    // framebuffer already runs with glEnable(GL_FRAMEBUFFER_SRGB) (see
    // test.cpp), which performs the single, correct sRGB encode. Applying
    // gamma here as well would double-encode and wash the image out.
    color = color * (2.51 * color + 0.03) / (color * (2.43 * color + 0.59) + 0.14);
    color = clamp(color, 0.0, 1.0);

    FragColor = vec4(color, 1.0);
}
