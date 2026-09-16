#version 430 core
// Procedural-sky vertex pass: emits a fullscreen quad (clip-space [-1,1]^2)
// using gl_VertexID, so no vertex buffers / VAO attributes are required.
// The fragment shader evaluates the PhysicalSky model (see lighting/PhysicalSky.h/.cpp)
// as a function of a single view direction + sun direction, so the same
// verified C++ color math drives the rendered sky.
layout(location = 0) out vec2 vUV;

void main() {
    const vec2 pos[4] = vec2[4]( vec2(-1.0,-1.0), vec2( 1.0,-1.0),
                                vec2(-1.0, 1.0), vec2( 1.0, 1.0) );
    const vec2 uv[4]  = vec2[4]( vec2(0.0,0.0), vec2(1.0,0.0),
                                vec2(0.0,1.0), vec2(1.0,1.0) );
    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
    vUV = uv[gl_VertexID];
}
