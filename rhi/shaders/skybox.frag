// B4 Skybox — fragment pass. Samples the scene cubemap along the view ray
// passed from skybox.vert (the cube vertex position).
#version 450
#extension GL_ARB_separate_shader_objects : enable
layout(set = 0, binding = 1) uniform samplerCube u_sky;
layout(location = 0) in vec3 v_dir;
layout(location = 0) out vec4 fragColor;
void main() {
    fragColor = texture(u_sky, v_dir);
}
