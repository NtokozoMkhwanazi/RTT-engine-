#version 460
// ============================================================================
// Scene Fragment Shader (GLSL 4.6)
// ============================================================================
// Solid color output — no texture, no lighting.  The cube fragment shader just
// outputs the interpolated per-instance vertex color.
// ============================================================================
// outVelocity @ location 1: must be explicitly written so the dual-attachment
// velocity target (VK_FORMAT_R32G32_SFLOAT) receives a clean ZERO vector
// instead of uninitialized VRAM garbage.  Static cubes have no motion.
// ============================================================================
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outVelocity;
layout(location = 6) in vec4 vColor;

void main() {
    outColor = vColor;
    outVelocity = vec2(0.0);
}
