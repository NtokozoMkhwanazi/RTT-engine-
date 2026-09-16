#version 430 core
// ============================================================================
// Fullscreen triangle — single triangle covers the entire screen
// No VAO needed: gl_VertexID drives the vertices.
// ============================================================================

out vec2 TexCoords;

void main() {
    // Generates a single triangle that covers the full viewport
    float x = float((gl_VertexID & 1) << 2);  // 0, 4, 0
    float y = float((gl_VertexID & 2) << 1);  // 0, 0, 4
    TexCoords = vec2(x, y) * 0.5;
    gl_Position = vec4(x - 1.0, y - 1.0, 0.0, 1.0);
}
