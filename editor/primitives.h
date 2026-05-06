#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>

namespace Primitives {

struct MeshData {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    GLsizei indexCount = 0;
};

MeshData CreateCube();
MeshData CreateSphere(int rings = 24, int segments = 32, float radius = 0.5f);
MeshData CreatePlane();
MeshData CreateCylinder(int radialSegments = 24, float radius = 0.5f, float height = 1.0f);
MeshData CreateCone(int radialSegments = 24, float radius = 0.5f, float height = 1.0f);
MeshData CreateTorus(int majorSegments = 32, int minorSegments = 24,
                     float majorRadius = 0.35f, float minorRadius = 0.15f);

void Cleanup(MeshData& mesh);

const MeshData& GetCube();
const MeshData& GetSphere();
const MeshData& GetPlane();
const MeshData& GetCylinder();
const MeshData& GetCone();
const MeshData& GetTorus();

void InitAll();
void CleanupAll();

} // namespace Primitives