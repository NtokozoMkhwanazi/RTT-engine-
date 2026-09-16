#version 430 core
// Forward+ tile pass: fullscreen quad via gl_VertexID. The fragment shader
// (forward_plus.frag) reads the clustered light lists produced by
// ClusteredForward::binLights (suggestions.txt #2) from SSBOs and accumulates
// per-tile light contributions. No vertex attributes / VAO needed.
void main() {
    const vec2 pos[4] = vec2[4]( vec2(-1.0,-1.0), vec2( 1.0,-1.0),
                               vec2(-1.0, 1.0), vec2( 1.0, 1.0) );
    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
}
