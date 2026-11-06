#version 460
#extension GL_EXT_scalar_block_layout : require
// ============================================================================
// CSM shadow-map rasterizer (Vulkan, GLSL 4.6) — depth-only.
// ----------------------------------------------------------------------------
// Renders the scene's meshes, from the SUN's view, into a 3-layer depth array
// (one layer per cascade). Only the vertex position + the per-cascade light
// space matrix are consumed; there is no fragment color output (the pipeline
// is created with no color attachments).
//
// Descriptor layout mirrors the bindless scene set (m_sceneLayout):
//   set=0  binding=1  : VertexBuffer  { Vertex pos/normal/uv }   (read-only)
//   set=0  binding=2  : InstanceBuffer{ mat4 model; ... }        (read-only)
//   set=1  binding=0  : ShadowUBO     { mat4 lightSpaceMatrices[3]; ... }
// Push constant: uint cascadeIndex (which layer + matrix to rasterize).
// ============================================================================
struct Vertex {
    vec3 pos;
    vec3 normal;
    vec2 uv;
};
struct Instance {
    mat4 model;
    vec4 color;
    mat4 prevModel;
};
layout(set = 0, binding = 1, scalar) readonly buffer VertexBuffer {
    Vertex vertices[];
} vb;
layout(set = 0, binding = 2, scalar) readonly buffer InstanceBuffer {
    Instance instances[];
} ib;
layout(set = 1, binding = 0, scalar) uniform ShadowUBO {
    mat4 lightSpaceMatrices[3];
    vec4 cascadeSplits;   // x,y,z = near/mid/far split distances
    vec4 sunDirection;
} sh;
layout(push_constant) uniform PushConstants {
    uint cascadeIndex;
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

void main() {
    uint vi = gl_VertexIndex;
    uint ii = gl_InstanceIndex;
    Vertex v     = vb.vertices[vi];
    Instance inst = ib.instances[ii];
    vec3 worldPos = vec3(inst.model * vec4(v.pos, 1.0));
    gl_Position = sh.lightSpaceMatrices[min(pc.cascadeIndex, 2u)]
                  * vec4(worldPos, 1.0);
}
