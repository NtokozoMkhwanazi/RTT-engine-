#version 460
// ============================================================================
// Deferred lighting — fullscreen vertex stage (Vulkan RHI, GLSL 4.6)
// ----------------------------------------------------------------------------
// Emits a single fullscreen triangle (3 vertices, no vertex buffers — the
// position/UV are derived from gl_VertexIndex) so one vkCmdDraw(3) covers the
// whole target. The fragment stage samples the G-Buffer written by the
// geometry pass and composites a directional light + ambient term.
// ============================================================================
layout(location = 0) out vec2 fUV;

void main() {
    // gl_VertexIndex in [0,2] -> full-screen right-tri coverage.
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    fUV = p;
}
