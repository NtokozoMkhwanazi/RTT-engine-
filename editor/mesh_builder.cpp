#include "mesh_builder.h"
#include <iostream>
#include <vector>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace MeshBuilder {

static MeshData g_cube;
static MeshData g_sphere;
static MeshData g_plane;
static MeshData g_cylinder;
static MeshData g_cone;
static MeshData g_torus;
static GLuint g_proceduralTexture = 0;

static void CleanupMesh(MeshData& mesh) {
    if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
    if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
    if (mesh.ebo) glDeleteBuffers(1, &mesh.ebo);
    mesh.vao = 0; mesh.vbo = 0; mesh.ebo = 0;
    mesh.indexCount = 0;
}

// Upload a Verts blob to a fresh GL VAO/VBO/EBO. Shared by every Create*() so
// the vertex math lives in exactly one place (the Create*Verts() helpers) and
// the GL handle path cannot drift from the CPU-verts path the RHI bridge reads.
MeshData UploadVerts(const Verts& v) {
    MeshData mesh;
    mesh.indexCount = (GLsizei)v.indices.size();
    glGenVertexArrays(1, &mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    glGenBuffers(1, &mesh.ebo);
    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 v.vertices.size() * sizeof(float), v.vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 v.indices.size() * sizeof(uint32_t), v.indices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    return mesh;
}

// ---------------------------------------------------------------------------
// CPU-only vertex generators (source-of-truth). Create*() call these; the RHI
// bridge (RenderPipeline::toOffscreenScene) calls them directly. Both paths are
// therefore built from the identical vertex math → GL/Vulkan pixel parity.
// ---------------------------------------------------------------------------
Verts CreateCubeVerts() {
    Verts cube;
    float v[] = {
        // positions          // normals           // texcoords
        -0.5f,-0.5f,-0.5f,  0,0,-1,  0,0,   0.5f,-0.5f,-0.5f,  0,0,-1,  1,0,   0.5f,0.5f,-0.5f,  0,0,-1,  1,1,
         0.5f,0.5f,-0.5f,  0,0,-1,  1,1,  -0.5f,0.5f,-0.5f,  0,0,-1,  0,1,  -0.5f,-0.5f,-0.5f,  0,0,-1,  0,0,
        -0.5f,-0.5f, 0.5f,  0,0, 1,  0,0,   0.5f,-0.5f, 0.5f,  0,0, 1,  1,0,   0.5f, 0.5f, 0.5f,  0,0, 1,  1,1,
         0.5f, 0.5f, 0.5f,  0,0, 1,  1,1,  -0.5f, 0.5f, 0.5f,  0,0, 1,  0,1,  -0.5f,-0.5f, 0.5f,  0,0, 1,  0,0,
        -0.5f, 0.5f, 0.5f, -1,0, 0,  0,0,  -0.5f, 0.5f,-0.5f, -1,0, 0,  1,0,  -0.5f,-0.5f,-0.5f, -1,0, 0,  1,1,
        -0.5f,-0.5f,-0.5f, -1,0, 0,  0,1,  -0.5f,-0.5f, 0.5f, -1,0, 0,  0,1,  -0.5f, 0.5f, 0.5f, -1,0, 0,  0,0,
         0.5f, 0.5f, 0.5f,  1,0, 0,  0,0,   0.5f, 0.5f,-0.5f,  1,0, 0,  1,0,   0.5f,-0.5f,-0.5f,  1,0, 0,  1,1,
         0.5f,-0.5f,-0.5f,  1,0, 0,  1,1,   0.5f,-0.5f, 0.5f,  1,0, 0,  0,1,   0.5f, 0.5f, 0.5f,  1,0, 0,  0,0,
        -0.5f,-0.5f,-0.5f,  0,-1, 0,  0,0,   0.5f,-0.5f,-0.5f,  0,-1, 0,  1,0,   0.5f,-0.5f, 0.5f,  0,-1, 0,  1,1,
         0.5f,-0.5f, 0.5f,  0,-1, 0,  1,1,  -0.5f,-0.5f, 0.5f,  0,-1, 0,  0,1,  -0.5f,-0.5f,-0.5f,  0,-1, 0,  0,0,
        -0.5f, 0.5f,-0.5f,  0, 1, 0,  0,0,   0.5f, 0.5f,-0.5f,  0, 1, 0,  1,0,   0.5f, 0.5f, 0.5f,  0, 1, 0,  1,1,
         0.5f, 0.5f, 0.5f,  0, 1, 0,  1,1,  -0.5f, 0.5f, 0.5f,  0, 1, 0,  0,1,  -0.5f, 0.5f,-0.5f,  0, 1, 0,  0,0
    };
    uint32_t idx[] = {
        0,  1,  2,  3,  4,  5,      // -Z face
        6,  7,  8,  9, 10, 11,      // +Z face
        12, 13, 14, 15, 16, 17,     // -X face
        18, 19, 20, 21, 22, 23,     // +X face
        24, 25, 26, 27, 28, 29,     // -Y face
        30, 31, 32, 33, 34, 35      // +Y face
    };
    cube.vertices.assign(v, v + sizeof(v) / sizeof(float));
    cube.indices.assign(idx, idx + sizeof(idx) / sizeof(uint32_t));
    return cube;
}

Verts CreateSphereVerts(int rings, int segments, float radius) {
    Verts s;
    for (int r = 0; r <= rings; r++) {
        float phi = M_PI * r / rings;
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);
        for (int i = 0; i <= segments; i++) {
            float theta = 2.0f * M_PI * i / segments;
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);
            float x = radius * sinPhi * cosTheta;
            float y = radius * cosPhi;
            float z = radius * sinPhi * sinTheta;
            float nx = sinPhi * cosTheta;
            float ny = cosPhi;
            float nz = sinPhi * sinTheta;
            float u = (float)i / segments;
            float v = (float)r / rings;
            s.vertices.insert(s.vertices.end(), {x, y, z, nx, ny, nz, u, v});
        }
    }
    for (int r = 0; r < rings; r++) {
        for (int i = 0; i < segments; i++) {
            uint32_t current = r * (segments + 1) + i;
            uint32_t next = current + segments + 1;
            s.indices.insert(s.indices.end(), {
                current, next, current + 1,
                current + 1, next, next + 1
            });
        }
    }
    return s;
}

Verts CreatePlaneVerts() {
    Verts p;
    float v[] = {
        // positions          // normals           // texcoords
        -0.5f, 0.0f, -0.5f,  0, 1, 0,  0, 0,
         0.5f, 0.0f, -0.5f,  0, 1, 0,  1, 0,
         0.5f, 0.0f,  0.5f,  0, 1, 0,  1, 1,
        -0.5f, 0.0f,  0.5f,  0, 1, 0,  0, 1,
    };
    uint32_t idx[] = { 0, 1, 2, 2, 3, 0 };
    p.vertices.assign(v, v + sizeof(v) / sizeof(float));
    p.indices.assign(idx, idx + sizeof(idx) / sizeof(uint32_t));
    return p;
}

Verts CreateCylinderVerts(int radialSegments, float radius, float height) {
    Verts c;
    float halfH = height * 0.5f;
    for (int i = 0; i <= radialSegments; i++) {
        float theta = 2.0f * M_PI * i / radialSegments;
        float cosT = std::cos(theta);
        float sinT = std::sin(theta);
        float x = radius * cosT;
        float z = radius * sinT;
        c.vertices.insert(c.vertices.end(), {x, -halfH, z, cosT, 0, sinT, (float)i/radialSegments, 0});
        c.vertices.insert(c.vertices.end(), {x,  halfH, z, cosT, 0, sinT, (float)i/radialSegments, 1});
    }
    for (int i = 0; i < radialSegments; i++) {
        uint32_t bottom0 = i * 2;
        uint32_t bottom1 = (i + 1) * 2;
        uint32_t top0 = bottom0 + 1;
        uint32_t top1 = bottom1 + 1;
        c.indices.insert(c.indices.end(), {bottom0, top0, bottom1, bottom1, top0, top1});
    }
    // Top cap
    uint32_t topCenter = c.vertices.size() / 8;
    c.vertices.insert(c.vertices.end(), {0, halfH, 0, 0, 1, 0, 0.5f, 0.5f});
    uint32_t topRingStart = c.vertices.size() / 8;
    for (int i = 0; i <= radialSegments; i++) {
        float theta = 2.0f * M_PI * i / radialSegments;
        float cosT = std::cos(theta);
        float sinT = std::sin(theta);
        c.vertices.insert(c.vertices.end(), {radius * cosT, halfH, radius * sinT, 0, 1, 0, cosT*0.5f+0.5f, sinT*0.5f+0.5f});
    }
    for (int i = 0; i < radialSegments; i++) {
        c.indices.insert(c.indices.end(), {topCenter, topRingStart + i + 1, topRingStart + i});
    }
    // Bottom cap
    uint32_t bottomCenter = c.vertices.size() / 8;
    c.vertices.insert(c.vertices.end(), {0, -halfH, 0, 0, -1, 0, 0.5f, 0.5f});
    uint32_t bottomRingStart = c.vertices.size() / 8;
    for (int i = 0; i <= radialSegments; i++) {
        float theta = 2.0f * M_PI * i / radialSegments;
        float cosT = std::cos(theta);
        float sinT = std::sin(theta);
        c.vertices.insert(c.vertices.end(), {radius * cosT, -halfH, radius * sinT, 0, -1, 0, cosT*0.5f+0.5f, sinT*0.5f+0.5f});
    }
    for (int i = 0; i < radialSegments; i++) {
        c.indices.insert(c.indices.end(), {bottomCenter, bottomRingStart + i + 1, bottomRingStart + i});
    }
    return c;
}

Verts CreateConeVerts(int radialSegments, float radius, float height) {
    Verts c;
    float halfH = height * 0.5f;
    // Apex
    c.vertices.insert(c.vertices.end(), {0, halfH, 0, 0, 1, 0, 0.5f, 0.5f});
    // Base ring
    uint32_t baseRingStart = 1;
    for (int i = 0; i <= radialSegments; i++) {
        float theta = 2.0f * M_PI * i / radialSegments;
        float cosT = std::cos(theta);
        float sinT = std::sin(theta);
        float x = radius * cosT;
        float z = radius * sinT;
        float ny = radius / height;
        float nLen = std::sqrt(cosT*cosT + ny*ny + sinT*sinT);
        float nx = cosT / nLen;
        float nny = ny / nLen;
        float nz = sinT / nLen;
        c.vertices.insert(c.vertices.end(), {x, -halfH, z, nx, nny, nz, (float)i/radialSegments, 0});
    }
    for (int i = 0; i < radialSegments; i++) {
        c.indices.insert(c.indices.end(), {0, baseRingStart + i, baseRingStart + i + 1});
    }
    // Base cap
    uint32_t baseCenter = c.vertices.size() / 8;
    c.vertices.insert(c.vertices.end(), {0, -halfH, 0, 0, -1, 0, 0.5f, 0.5f});
    uint32_t baseCapRingStart = c.vertices.size() / 8;
    for (int i = 0; i <= radialSegments; i++) {
        float theta = 2.0f * M_PI * i / radialSegments;
        float cosT = std::cos(theta);
        float sinT = std::sin(theta);
        c.vertices.insert(c.vertices.end(), {radius * cosT, -halfH, radius * sinT, 0, -1, 0, cosT*0.5f+0.5f, sinT*0.5f+0.5f});
    }
    for (int i = 0; i < radialSegments; i++) {
        c.indices.insert(c.indices.end(), {baseCenter, baseCapRingStart + i + 1, baseCapRingStart + i});
    }
    return c;
}

Verts CreateTorusVerts(int majorSegments, int minorSegments, float majorRadius, float minorRadius) {
    Verts t;
    for (int i = 0; i <= majorSegments; i++) {
        float theta = 2.0f * M_PI * i / majorSegments;
        float cosTheta = std::cos(theta);
        float sinTheta = std::sin(theta);
        for (int j = 0; j <= minorSegments; j++) {
            float phi = 2.0f * M_PI * j / minorSegments;
            float cosPhi = std::cos(phi);
            float sinPhi = std::sin(phi);
            float x = (majorRadius + minorRadius * cosPhi) * cosTheta;
            float y = minorRadius * sinPhi;
            float z = (majorRadius + minorRadius * cosPhi) * sinTheta;
            float nx = cosPhi * cosTheta;
            float ny = sinPhi;
            float nz = cosPhi * sinTheta;
            float u = (float)i / majorSegments;
            float v = (float)j / minorSegments;
            t.vertices.insert(t.vertices.end(), {x, y, z, nx, ny, nz, u, v});
        }
    }
    for (int i = 0; i < majorSegments; i++) {
        for (int j = 0; j < minorSegments; j++) {
            uint32_t current = i * (minorSegments + 1) + j;
            uint32_t next = current + minorSegments + 1;
            t.indices.insert(t.indices.end(), {
                current, next, current + 1,
                current + 1, next, next + 1
            });
        }
    }
    return t;
}

// ---------------------------------------------------------------------------
// GL-handle wrappers (upload the CPU verts to a VAO). Behavior is identical to
// the original implementation; the vertex math now lives in Create*Verts().
// ---------------------------------------------------------------------------
MeshData CreateCube() {
    Verts v = CreateCubeVerts();
    MeshData mesh = UploadVerts(v);
    std::cout << "[Mesh] Cube: " << v.vertices.size()/8 << " verts, " << mesh.indexCount << " indices\n";
    return mesh;
}

MeshData CreateSphere(int rings, int segments, float radius) {
    Verts v = CreateSphereVerts(rings, segments, radius);
    MeshData mesh = UploadVerts(v);
    std::cout << "[Mesh] Sphere: " << v.vertices.size()/8 << " verts, " << mesh.indexCount << " indices\n";
    return mesh;
}

MeshData CreatePlane() {
    Verts v = CreatePlaneVerts();
    MeshData mesh = UploadVerts(v);
    std::cout << "[Mesh] Plane: " << v.vertices.size()/8 << " verts, " << mesh.indexCount << " indices\n";
    return mesh;
}

MeshData CreateCylinder(int radialSegments, float radius, float height) {
    Verts v = CreateCylinderVerts(radialSegments, radius, height);
    MeshData mesh = UploadVerts(v);
    std::cout << "[Mesh] Cylinder: " << v.vertices.size()/8 << " verts, " << mesh.indexCount << " indices\n";
    return mesh;
}

MeshData CreateCone(int radialSegments, float radius, float height) {
    Verts v = CreateConeVerts(radialSegments, radius, height);
    MeshData mesh = UploadVerts(v);
    std::cout << "[Mesh] Cone: " << v.vertices.size()/8 << " verts, " << mesh.indexCount << " indices\n";
    return mesh;
}

MeshData CreateTorus(int majorSegments, int minorSegments, float majorRadius, float minorRadius) {
    Verts v = CreateTorusVerts(majorSegments, minorSegments, majorRadius, minorRadius);
    MeshData mesh = UploadVerts(v);
    std::cout << "[Mesh] Torus: " << v.vertices.size()/8 << " verts, " << mesh.indexCount << " indices\n";
    return mesh;
}

GLuint CreateProceduralTexture() {
    const int texSize = 256;
    unsigned char* texData = new unsigned char[texSize * texSize * 3];

    int checkSize = 32;
    for (int y = 0; y < texSize; y++) {
        for (int x = 0; x < texSize; x++) {
            int idx = (y * texSize + x) * 3;
            bool isWhite = ((x / checkSize) + (y / checkSize)) % 2 == 0;

            if (isWhite) {
                texData[idx + 0] = 200;
                texData[idx + 1] = 200;
                texData[idx + 2] = 200;
            } else {
                texData[idx + 0] = 80;
                texData[idx + 1] = 80;
                texData[idx + 2] = 100;
            }
        }
    }

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, texSize, texSize, 0, GL_RGB, GL_UNSIGNED_BYTE, texData);
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindTexture(GL_TEXTURE_2D, 0);
    delete[] texData;

    std::cout << "[OK] Procedural checkerboard texture created (" << texSize << "x" << texSize << ")\n";
    return texture;
}

void InitAll() {
    g_cube = CreateCube();
    g_sphere = CreateSphere(24, 32);
    g_plane = CreatePlane();
    g_cylinder = CreateCylinder(24, 0.5f, 1.0f);
    g_cone = CreateCone(24, 0.5f, 1.0f);
    g_torus = CreateTorus(32, 24, 0.35f, 0.15f);
    g_proceduralTexture = CreateProceduralTexture();
}

void CleanupAll() {
    CleanupCube(g_cube);
    CleanupSphere(g_sphere);
    CleanupPlane(g_plane);
    CleanupCylinder(g_cylinder);
    CleanupCone(g_cone);
    CleanupTorus(g_torus);
    
    if (g_proceduralTexture) glDeleteTextures(1, &g_proceduralTexture);
}

void CleanupCube(MeshData& mesh) { CleanupMesh(mesh); }
void CleanupSphere(MeshData& mesh) { CleanupMesh(mesh); }
void CleanupPlane(MeshData& mesh) { CleanupMesh(mesh); }
void CleanupCylinder(MeshData& mesh) { CleanupMesh(mesh); }
void CleanupCone(MeshData& mesh) { CleanupMesh(mesh); }
void CleanupTorus(MeshData& mesh) { CleanupMesh(mesh); }

// Getters for registered meshes
const MeshData& GetCube() { return g_cube; }
const MeshData& GetSphere() { return g_sphere; }
const MeshData& GetPlane() { return g_plane; }
const MeshData& GetCylinder() { return g_cylinder; }
const MeshData& GetCone() { return g_cone; }
const MeshData& GetTorus() { return g_torus; }
GLuint GetProceduralTexture() { return g_proceduralTexture; }

} // namespace MeshBuilder
