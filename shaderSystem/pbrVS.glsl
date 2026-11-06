// ============================================================================
// pbrVS.glsl - Forward PBR vertex shader (OpenGL port of rhi/shaders/pbr_mesh)
// ============================================================================
// Vertex layout driven by test_pbr_forward_gl.cpp:
//   loc 0: aPos        (vec3)
//   loc 1: aNormal    (vec3)
//   loc 2: aTexCoords (vec2)
//   loc 3: aInstancePos (vec3, padded to vec4)
//   loc 4: aInstanceColor (vec4)
//   loc 5-8: aModel   (mat4)
// Uniforms: view, projection, uCameraPos.
// Builds a TBN basis + tangent-space view vector for the FS normal map / POM.
// ============================================================================
#version 430 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 3) in vec3 aInstancePos;
layout(location = 4) in vec4 aInstanceColor;
layout(location = 5) in mat4 aModel;   // consumes locations 5,6,7,8

uniform mat4 view;
uniform mat4 projection;
uniform vec3 uCameraPos;

out VS_OUT {
    vec3 FragPos;
    vec3 Normal;
    vec2 TexCoord;
    vec4 InstanceColor;
    mat3 TBN;            // world-space tangent basis (columns = T, B, N)
    vec3 ViewDirTS;      // view (camera->fragment) dir in tangent space
} vs_out;

void main() {
    vs_out.InstanceColor = aInstanceColor;
    vs_out.TexCoord = aTexCoords;

    vec4 worldPos = aModel * vec4(aPos, 1.0);
    vs_out.FragPos = worldPos.xyz;

    mat3 normalMat = transpose(inverse(mat3(aModel)));
    vec3 N = normalize(normalMat * aNormal);
    vs_out.Normal = N;

    // Reconstruct a tangent frame (test meshes carry no tangent attribute).
    vec3 T = normalize(normalMat * vec3(1.0, 0.0, 0.0));
    vec3 B = normalize(cross(N, T));
    T = normalize(cross(B, N));
    vs_out.TBN = mat3(T, B, N);

    // View direction in tangent space (transpose(TBN) == inverse for orthonormal).
    vec3 viewDirW = uCameraPos - vs_out.FragPos;
    vs_out.ViewDirTS = transpose(vs_out.TBN) * viewDirW;

    gl_Position = projection * view * worldPos;
}
