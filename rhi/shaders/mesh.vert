#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
// ============================================================================
// Bindless Mesh Vertex Shader (Vulkan RHI / GLSL 4.6)
// ============================================================================
// Vertex data is fetched from a global SSBO (not vertex attributes) using
// gl_VertexIndex.  Instance data is fetched from another global SSBO using
// gl_InstanceIndex.  The per-mesh texture is selected via a push constant
// textureId that indexes into the bindless globalTextures[] array.
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

// --- Set 0: global bindless descriptor set ---
// Binding 0: Camera UBO (std140, matches CameraUBOData in RHIMath.h)
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 viewProj;
    vec3 camPos;
    float fogDensity;
    vec3 fogColor;
    float pad;
    layout(offset = 160) vec2 jitterOffset;
} cam;

// Binding 1: global vertex SSBO (all meshes concatenated)
layout(set = 0, binding = 1, scalar) readonly buffer VertexBuffer {
    Vertex vertices[];
} vb;

// Binding 2: global instance SSBO (all meshes' instances concatenated)
layout(set = 0, binding = 2, scalar) readonly buffer InstanceBuffer {
    Instance instances[];
} ib;

// Push constants — textureId selects from the bindless texture array
layout(push_constant) uniform PushConstants {
    uint textureId;
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

// Output varyings to fragment shader
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
    vNormal     = mat3(inst.model) * v.normal;
    vWorldPos   = vec3(inst.model * vec4(v.pos, 1.0));
    gl_Position = cam.viewProj * vec4(vWorldPos, 1.0);
    // FSR3 Phase 2: sub-pixel temporal jitter (NDC units). Shifts NDC.xy by
    // jitterOffset after the perspective divide: NDC += jitterOffset. Same
    // expression in the GL inline shaders -> GL<->Vulkan parity preserved.
    gl_Position.xy += cam.jitterOffset * gl_Position.w;
    // Phase 3b: previous-frame NDC position (same viewProj, prevModel matrix)
    // drives the screen-space motion vector: velocity = currNDC - prevNDC.
    // Camera-motion reprojection (prevViewProj @96) is staged separately;
    // frame-0 velocity is pure object motion, which is headless-verifiable.
    vPrevPos   = cam.viewProj * inst.prevModel * vec4(v.pos, 1.0);
    vCurrNdc   = gl_Position.xy / gl_Position.w;
}
