// B4 Skybox — vertex pass. The skybox is a ±1 cube at world origin, rendered
// from OUTSIDE (the camera sits just inside the cube, like a sky-dome): the
// perspective projection gives each face a distinct depth so the 6 faces never
// collapse/overlap the way an un-projected NDC cube (z==w for every vert) does.
// viewProj projects the cube to ~NDC ±0.866; z is pinned to w so the whole cube
// sits on the far plane (depth==1.0) and is always occluded by nearer geometry.
// The cube's POSITION is the cubemap sampling direction (passed to frag via
// v_dir), so the fragment shader samples the cubemap straight, no inverse
// viewProj reconstruction needed.
#version 450
#extension GL_ARB_separate_shader_objects : enable
layout(location = 0) in vec3 pos;
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 viewProj;
};
layout(location = 0) out vec3 v_dir;
void main() {
    vec3 dir = pos * 2.0;   // kCubeVerts is [-0.5,0.5] -> ±1 cube (also sampling dir)
    v_dir = dir;
    vec4 p = viewProj * vec4(dir, 1.0);
    p.z = p.w;            // skybox -> far plane (depth == 1.0)
    gl_Position = p;
}
