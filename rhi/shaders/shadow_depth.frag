#version 460
// ============================================================================
// CSM shadow-map fragment stage (Vulkan, GLSL 4.6) — depth-only.
// ----------------------------------------------------------------------------
// The shadow pipeline is created with ZERO color attachments, so this shader
// produces no fragments; Vulkan still requires a fragment shader module to
// exist for a graphics pipeline. The only output that matters is the depth
// written by the rasterizer into the 2D-array shadow atlas.
// ============================================================================
void main() {
    // Intentionally empty — depth-only pass.
}
