#ifndef EDITOR_MESH_BUILDER_H
#define EDITOR_MESH_BUILDER_H

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

// ============================================================================
// Procedural Mesh Generation
// ============================================================================
namespace MeshBuilder {

struct MeshData {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    GLsizei indexCount = 0;
};

// Initialize all procedural meshes
void InitAll();

// Cleanup all procedural meshes
void CleanupAll();

// Individual mesh creation functions
MeshData CreateCube();
MeshData CreateSphere(int rings = 24, int segments = 32, float radius = 0.5f);
MeshData CreatePlane();
MeshData CreateCylinder(int radialSegments = 24, float radius = 0.5f, float height = 1.0f);
MeshData CreateCone(int radialSegments = 24, float radius = 0.5f, float height = 1.0f);
MeshData CreateTorus(int majorSegments = 32, int minorSegments = 24,
                     float majorRadius = 0.35f, float minorRadius = 0.15f);

// CPU vertex data (the RHI bridge source-of-truth). Every Create*() below
// generates its vertices via the matching Create*Verts() helper first and then
// uploads them to GL — so the offscreen/ECS MeshComponent primitives and the GL
// VAOs are built from the EXACT same vertex math (no formula drift between the
// GL editor path and the RHI renderFrameScene path). `vertices` is interleaved
// [pos.xyz][normal.xyz][uv] (8 floats/vertex), matching OffscreenMesh + the
// pbr_mesh/gbuffer vertex conventions.
struct Verts {
    std::vector<float> vertices;     // 8 floats/vertex (pos3, normal3, uv2)
    std::vector<uint32_t> indices;
};

// CPU-only vertex data (source-of-truth for the RHI bridge). Create*() call
// these and then upload to GL; RenderPipeline::toOffscreenScene() calls them
// directly to build OffscreenScene::Mesh without touching the GL VAOs.
Verts CreateCubeVerts();
Verts CreateSphereVerts(int rings = 24, int segments = 32, float radius = 0.5f);
Verts CreatePlaneVerts();
Verts CreateCylinderVerts(int radialSegments = 24, float radius = 0.5f, float height = 1.0f);
Verts CreateConeVerts(int radialSegments = 24, float radius = 0.5f, float height = 1.0f);
Verts CreateTorusVerts(int majorSegments = 32, int minorSegments = 24,
                       float majorRadius = 0.35f, float minorRadius = 0.15f);

// Upload a Verts blob to a fresh GL VAO/VBO/EBO (shared helper for Create*()).
MeshData UploadVerts(const Verts& v);

// Cleanup individual meshes
void CleanupCube(MeshData& mesh);
void CleanupSphere(MeshData& mesh);
void CleanupPlane(MeshData& mesh);
void CleanupCylinder(MeshData& mesh);
void CleanupCone(MeshData& mesh);
void CleanupTorus(MeshData& mesh);

// Create procedural texture (checkerboard)
GLuint CreateProceduralTexture();

// Getters for global mesh instances
const MeshData& GetCube();
const MeshData& GetSphere();
const MeshData& GetPlane();
const MeshData& GetCylinder();
const MeshData& GetCone();
const MeshData& GetTorus();
GLuint GetProceduralTexture();

} // namespace MeshBuilder

#endif // EDITOR_MESH_BUILDER_H
