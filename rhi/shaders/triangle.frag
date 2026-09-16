#version 460
// ============================================================================
// Triangle Fragment Shader (Vulkan RHI / GLSL 4.6)
// ============================================================================
// Solid green output — the readback test verifies the triangle area is this
// color and the clear area is the clear color, proving the pipeline ran.
// ============================================================================
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(0.0, 1.0, 0.0, 1.0);
}
