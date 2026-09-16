#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
// ============================================================================
// Bindless PBR Mesh Vertex Shader (Vulkan editor mode / GLSL 4.6)
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

layout(push_constant) uniform PushConstants {
    uint textureId;
    uint pad0;
    uint pad1;
    uint pad2;
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

    vColor      = inst.color;
    vUV         = v.uv;
    vWorldPos   = vec3(inst.model * vec4(v.pos, 1.0));
    vNormal     = mat3(transpose(inverse(inst.model))) * v.normal;
    gl_Position = cam.viewProj * vec4(vWorldPos, 1.0);
    // FSR3 Phase 2: sub-pixel temporal jitter (NDC units).
    gl_Position.xy += cam.jitterOffset * gl_Position.w;
    // Phase 3b: previous-frame clip position using PREVIOUS viewProj + prevModel.
    // Jitter is NOT applied here — motion vectors must be jitter-free so the
    // temporal reproject matches the jitter-canceled EASU output.
    vPrevPos   = cam.prevViewProj * inst.prevModel * vec4(v.pos, 1.0);
    // Strip jitter from current NDC so velocity = pure geometric motion.
    vec4 strippedPos = gl_Position;
    strippedPos.xy -= cam.jitterOffset * strippedPos.w;
    vCurrNdc   = strippedPos.xy / strippedPos.w;
}
