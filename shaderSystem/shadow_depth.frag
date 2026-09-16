#version 450 core
// Shadow depth fragment shader - depth-only pass
// Writes gl_FragDepth automatically (no color output needed)

void main() {
    // Depth is written automatically by the fixed-function pipeline.
    // gl_FragDepth = gl_FragCoord.z;  // implicit
}
