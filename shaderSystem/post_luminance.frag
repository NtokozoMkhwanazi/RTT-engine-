#version 430 core
// ============================================================================
// Luminance Extract — log2(scene luminance) per pixel into a GL_R32F target.
// ============================================================================
// Used by PostProcess auto-exposure. Mipmap reduction (glGenerateTextureMipmap
// with GL_LINEAR_MIPMAP_LINEAR) turns this into the log-average (= log2 of the
// geometric-mean) scene luminance in the 1x1 coarsest level, sampled via a
// large lod in post_composite.frag.
// ============================================================================

layout (location = 0) out float FragLum;

uniform sampler2D uScene;
const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);  // Rec. 709 luma

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(uScene, 0));
    vec3 c  = texture(uScene, uv).rgb;
    float l = max(dot(c, LUMA), 1e-4);
    FragLum = log2(l);
}
