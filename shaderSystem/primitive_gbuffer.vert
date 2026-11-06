#version 430 core
// ============================================================================
// Instanced G-Buffer Vertex Shader for spawned ECS primitives (forRenderer.txt
// Part 2). Emits view-space position + normal + flat material to the 4-MRT
// deferred G-Buffer so a whole MeshType can be drawn with ONE
// glDrawElementsInstanced call.
//
// Per-instance model + material arrive as vertex attributes (divisor = 1):
//   layout(location = 3) in mat4  aInstanceModel    -> spans 3,4,5,6
//   layout(location = 7) in vec4  aInstanceMaterial   (rgb = albedo, a = metallic)
//   layout(location = 8) in vec4  aInstanceRoughness  (r = roughness)
// Attribute locations are chosen to avoid colliding with the procedural mesh
// VAO (aPos=0, aNormal=1, aTexCoords=2) and with shadow_depth.vert's aModel@6
// and the batch renderer's instanceMatrix@7-10 (see render_pipeline.cpp).
// ============================================================================

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;

// Per-instance (divisor = 1). A mat4 input spans 4 consecutive locations.
layout(location = 3) in mat4 aInstanceModel;
layout(location = 7) in vec4 aInstanceMaterial;   // rgb = albedo, a = metallic
layout(location = 8) in vec4 aInstanceRoughness;  // r = roughness

uniform mat4 view;
uniform mat4 projection;

out vec3 FragPosView;
out vec3 NormalView;
out vec2 TexCoords;
out vec3 vColor;
out float vMetallic;
out float vRoughness;

void main() {
    vec4 viewPos = view * aInstanceModel * vec4(aPos, 1.0);
    FragPosView = viewPos.xyz;

    // Normal matrix in view space
    NormalView = mat3(transpose(inverse(view * aInstanceModel))) * aNormal;

    TexCoords = aTexCoords;
    vColor      = aInstanceMaterial.rgb;
    vMetallic   = aInstanceMaterial.a;
    vRoughness  = aInstanceRoughness.r;

    gl_Position = projection * viewPos;
}
