#ifndef EDITOR_MESH_BUILDER_H
#define EDITOR_MESH_BUILDER_H

#include <glad/glad.h>
#include <glm/glm.hpp>

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
