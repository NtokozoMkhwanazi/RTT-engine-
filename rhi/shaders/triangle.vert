#version 460
// ============================================================================
// Triangle Vertex Shader (Vulkan RHI / GLSL 4.6)
// ============================================================================
// Renders a centered triangle in NDC using a push constant for vertex
// positions — no vertex input attributes, no buffers needed.  This exercises
// the complete Vulkan render stack (SPIR-V compile, pipeline, command buffer,
// submit, readback) headlessly.
// ============================================================================
layout(push_constant) uniform PushConstants {
    vec2 pos0;
    vec2 pos1;
    vec2 pos2;
} pc;

void main() {
    vec2 pos;
    if (gl_VertexIndex == 0)      pos = pc.pos0;
    else if (gl_VertexIndex == 1) pos = pc.pos1;
    else                          pos = pc.pos2;
    gl_Position = vec4(pos, 0.0, 1.0);
}
