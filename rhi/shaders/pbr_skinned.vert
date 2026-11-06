#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
// ============================================================================
// Skinned (LBS) PBR mesh vertex stage (Vulkan RHI, GLSL 4.6)
// ----------------------------------------------------------------------------
// Twin of pbr_mesh.vert that applies a 4-bone linear-blend skinning (LBS)
// transform BEFORE the instance model matrix. This is what lets the play
// character render as a skinned ECS entity on the Vulkan path (parity with
// the GL editor's PlayModeController + skinned Model).
//
// Vertex fetch is still the SSBO convention (gl_VertexIndex into set=0 binding
// 1, no vertex attributes). The bone data lives in a SEPARATE storage buffer
// (set=1, binding 0) so the shared bindless layout (set=0, 4 bindings) is left
// untouched — only pbr_skinned reads set=1. The bone palette (mat4[]) is
// set=1 binding 1; the per-mesh palette base is pushed as `boneOffset`
// (a mat4 index). Texture albedo is read by pbr_skinned.frag, which is just
// pbr_mesh.frag — so the lit Cook-Torrance composite is IDENTICAL to the
// unskinned path (only the vertex stage knows about bones).
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
    vec3 camPos;
    float fogDensity;
    vec3 fogColor;
    float pad;
    layout(offset = 96) mat4 prevViewProj;
    layout(offset = 160) vec2 jitterOffset;
} cam;

layout(set = 0, binding = 1, scalar) readonly buffer VertexBuffer {
    Vertex vertices[];
} vb;

layout(set = 0, binding = 2, scalar) readonly buffer InstanceBuffer {
    Instance instances[];
} ib;

layout(set = 0, binding = 3) uniform sampler2D globalTextures[];

// Per-mesh bone data (set=1 — skinning-only set, absent from the bindless layout).
// boneIdx is global (parallel to the position VertexBuffer), so gl_VertexIndex
// indexes it directly — cubes/solid meshes never touch this buffer.
layout(set = 1, binding = 0, scalar) readonly buffer SkinVertexBuffer {
    uvec4 boneIdx;
    vec4 boneWeight;
} skinVerts[];
layout(set = 1, binding = 1, scalar) readonly buffer BonePalette {
    mat4 bones[];
} palette;

layout(push_constant) uniform PushConstants {
    uint textureId;     // @0  (frag reads — identical to pbr_mesh)
    uint boneOffset;    // @4  (mat4 index into BonePalette for this mesh)
    uint firstVertex;   // @8  (vertex id of this mesh's first vertex)
    uint pad;
} pc;

layout(location = 8) out vec4 vColor;
layout(location = 9) out vec3 vNormal;
layout(location = 10) out vec2 vUV;
layout(location = 11) out vec3 vWorldPos;
layout(location = 12) out vec2 vCurrNdc;
layout(location = 13) out vec4 vPrevPos;

void main() {
    Vertex v   = vb.vertices[gl_VertexIndex];
    Instance inst = ib.instances[gl_InstanceIndex];

    // Local vertex index within the mesh (the skin VB is per-mesh, offset 0).
    // pc.firstVertex is the mesh's firstVertex in the global vertex SSBO, so
    // gl_VertexIndex - firstVertex yields this vertex's local skin index.
    uint localV = gl_VertexIndex - pc.firstVertex;

    uvec4 bIdx   = skinVerts[localV].boneIdx;
    vec4  bW     = skinVerts[localV].boneWeight;

    // 4-bone LBS (matches pbr_mesh.vert's model-space output convention).
    mat4 skinM = mat4(0.0);
    skinM += palette.bones[pc.boneOffset + bIdx.x] * bW.x;
    skinM += palette.bones[pc.boneOffset + bIdx.y] * bW.y;
    skinM += palette.bones[pc.boneOffset + bIdx.z] * bW.z;
    skinM += palette.bones[pc.boneOffset + bIdx.w] * bW.w;

    vec4 skinnedPos   = skinM * vec4(v.pos, 1.0);
    vec3 skinnedN     = mat3(skinM) * v.normal;

    vColor      = inst.color;
    vUV         = v.uv;
    vWorldPos   = vec3(inst.model * skinnedPos);
    vNormal     = mat3(transpose(inverse(inst.model))) * skinnedN;
    gl_Position = cam.viewProj * vec4(vWorldPos, 1.0);
    // FSR3 Phase 2: sub-pixel temporal jitter (NDC units).
    gl_Position.xy += cam.jitterOffset * gl_Position.w;
    // Phase 3b: previous-frame clip position (jitter-free for velocity).
    vPrevPos   = cam.prevViewProj * inst.prevModel * skinnedPos;
    vec4 strippedPos = gl_Position;
    strippedPos.xy -= cam.jitterOffset * strippedPos.w;
    vCurrNdc   = strippedPos.xy / strippedPos.w;
}
