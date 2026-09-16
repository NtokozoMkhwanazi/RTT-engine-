#version 460
#extension GL_EXT_scalar_block_layout : require
// ============================================================================
// Bindless Scene Vertex Shader (instanced cubes / GLSL 4.6)
// ============================================================================
// Cube vertex positions and per-instance model/color fetched from global
// SSBOs.  No vertex input attributes — vertex input state is empty; all data
// comes from gl_VertexIndex / gl_InstanceIndex into the SSBOs, with offsets
// supplied by vkCmdDraw's firstVertex / firstInstance params.
// ============================================================================

struct Vertex {
    vec3 pos;
    vec3 normal;
    vec2 uv;
};

struct Instance {
    mat4 model;
    vec4 color;
    mat4 prevModel; // Phase 3: previous-frame model (motion vectors), offset 80
};

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 viewProj;
    layout(offset = 160) vec2 jitterOffset;
    layout(offset = 96) mat4 prevViewProj;
} cam;

layout(set = 0, binding = 1, scalar) readonly buffer VertexBuffer {
    Vertex vertices[];
} vb;

layout(set = 0, binding = 2, scalar) readonly buffer InstanceBuffer {
    Instance instances[];
} ib;

layout(push_constant) uniform PushConstants {
    uint textureId;
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

layout(location = 6) out vec4 vColor;

void main() {
    Vertex v   = vb.vertices[gl_VertexIndex];
    Instance inst = ib.instances[gl_InstanceIndex];

    vColor = inst.color;
    gl_Position = cam.viewProj * inst.model * vec4(v.pos, 1.0);
    // FSR3 Phase 2: sub-pixel temporal jitter (NDC units).
    gl_Position.xy += cam.jitterOffset * gl_Position.w;
}
