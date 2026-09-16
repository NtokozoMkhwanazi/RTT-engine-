#version 430 core
// Procedural water vertex shader - animates a flat XZ grid into gentle waves.
// No textures required (the surface detail comes from the fragment shader's
// procedural normal), so it works with the existing asset set.
layout (location = 0) in vec3 aPos;   // x, 0, z on the grid plane
layout (location = 2) in vec2 aTex;

uniform mat4 uMVP;      // projection * view * model (model = water-level lift)
uniform mat4 uModel;    // world matrix (translation to water_level)
uniform vec3 uViewPos;
uniform float uTime = 0.0;

out vec2 vUV;
out vec3 vWorldPos;
out vec3 vViewDir;

void main()
{
    vUV = aTex;

    vec3 p = aPos;
    // Low-frequency vertex waves so the surface has real geometric motion near
    // the camera (the fragment normal refines detail in the distance).
    float wave = sin(p.x * 0.25 + uTime) * 0.04
               + cos(p.z * 0.19 + uTime * 1.7) * 0.03
               + sin(p.x * 0.11 + p.z * 0.13 + uTime * 0.6) * 0.02;
    p.y += wave;

    vWorldPos = (uModel * vec4(p, 1.0)).xyz;
    vViewDir  = uViewPos - vWorldPos;
    gl_Position = uMVP * vec4(p, 1.0);
}
