#include "primitives.h"
#include <iostream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Primitives {

static MeshData g_cube;
static MeshData g_sphere;
static MeshData g_plane;
static MeshData g_cylinder;
static MeshData g_cone;
static MeshData g_torus;

static void CleanupMesh(MeshData& mesh) {
    if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
    if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
    if (mesh.ebo) glDeleteBuffers(1, &mesh.ebo);
    mesh.vao = 0; mesh.vbo = 0; mesh.ebo = 0;
    mesh.indexCount = 0;
}

MeshData CreateCube() {
    MeshData mesh;
    
    float verts[] = {
        -0.5f,-0.5f,-0.5f,  0,0,-1,  0,0,   0.5f,-0.5f,-0.5f,  0,0,-1,  1,0,   0.5f,0.5f,-0.5f,  0,0,-1,  1,1,
         0.5f,0.5f,-0.5f,  0,0,-1,  1,1,  -0.5f,0.5f,-0.5f,  0,0,-1,  0,1,  -0.5f,-0.5f,-0.5f,  0,0,-1,  0,0,
        -0.5f,-0.5f, 0.5f,  0,0, 1,  0,0,   0.5f,-0.5f, 0.5f,  0,0, 1,  1,0,   0.5f, 0.5f, 0.5f,  0,0, 1,  1,1,
         0.5f, 0.5f, 0.5f,  0,0, 1,  1,1,  -0.5f, 0.5f, 0.5f,  0,0, 1,  0,1,  -0.5f,-0.5f, 0.5f,  0,0, 1,  0,0,
        -0.5f, 0.5f, 0.5f, -1,0, 0,  0,0,  -0.5f, 0.5f,-0.5f, -1,0, 0,  1,0,  -0.5f,-0.5f,-0.5f, -1,0, 0,  1,1,
        -0.5f,-0.5f,-0.5f, -1,0, 0,  1,1,  -0.5f,-0.5f, 0.5f, -1,0, 0,  0,1,  -0.5f, 0.5f, 0.5f, -1,0, 0,  0,0,
         0.5f, 0.5f, 0.5f,  1,0, 0,  0,0,   0.5f, 0.5f,-0.5f,  1,0, 0,  1,0,   0.5f,-0.5f,-0.5f,  1,0, 0,  1,1,
         0.5f,-0.5f,-0.5f,  1,0, 0,  1,1,   0.5f,-0.5f, 0.5f,  1,0, 0,  0,1,   0.5f, 0.5f, 0.5f,  1,0, 0,  0,0,
        -0.5f,-0.5f,-0.5f,  0,-1, 0,  0,0,   0.5f,-0.5f,-0.5f,  0,-1, 0,  1,0,   0.5f,-0.5f, 0.5f,  0,-1, 0,  1,1,
         0.5f,-0.5f, 0.5f,  0,-1, 0,  1,1,  -0.5f,-0.5f, 0.5f,  0,-1, 0,  0,1,  -0.5f,-0.5f,-0.5f,  0,-1, 0,  0,0,
        -0.5f, 0.5f,-0.5f,  0, 1, 0,  0,0,   0.5f, 0.5f,-0.5f,  0, 1, 0,  1,0,   0.5f, 0.5f, 0.5f,  0, 1, 0,  1,1,
         0.5f, 0.5f, 0.5f,  0, 1, 0,  1,1,  -0.5f, 0.5f, 0.5f,  0, 1, 0,  0,1,  -0.5f, 0.5f,-0.5f,  0, 1, 0,  0,0
    };
    unsigned int idx[] = {
        0,1,2,2,3,4,5, 6,7,7,8,9, 10,11,12,12,13,14,
        15,16,17,17,18,19, 20,21,22,22,23,24, 25,26,27,27,28,29,
        30,31,32,32,33,34, 35,36,37,37,38,39
    };

    glGenVertexArrays(1, &mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    glGenBuffers(1, &mesh.ebo);

    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(6*sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);
    mesh.indexCount = 36;
    
    return mesh;
}

MeshData CreateSphere(int rings, int segments, float radius) {
    MeshData mesh;
    std::vector<float> verts;
    std::vector<unsigned int> indices;

    for (int r = 0; r <= rings; r++) {
        float phi = M_PI * r / rings;
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);

        for (int s = 0; s <= segments; s++) {
            float theta = 2.0f * M_PI * s / segments;
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            float x = radius * sinPhi * cosTheta;
            float y = radius * cosPhi;
            float z = radius * sinPhi * sinTheta;

            float nx = sinPhi * cosTheta;
            float ny = cosPhi;
            float nz = sinPhi * sinTheta;

            float u = (float)s / segments;
            float v = (float)r / rings;

            verts.insert(verts.end(), {x, y, z, nx, ny, nz, u, v});
        }
    }

    for (int r = 0; r < rings; r++) {
        for (int s = 0; s < segments; s++) {
            unsigned int current = r * (segments + 1) + s;
            unsigned int next = current + segments + 1;
            indices.insert(indices.end(), {
                current, next, current + 1,
                current + 1, next, next + 1
            });
        }
    }

    mesh.indexCount = indices.size();
    glGenVertexArrays(1, &mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    glGenBuffers(1, &mesh.ebo);
    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    std::cout << "[Primitives] Sphere: " << verts.size()/8 << " verts, " << mesh.indexCount << " indices\n";
    return mesh;
}

MeshData CreatePlane() {
    MeshData mesh;
    float verts[] = {
        -0.5f, 0.0f, -0.5f,  0, 1, 0,  0, 0,
         0.5f, 0.0f, -0.5f,  0, 1, 0,  1, 0,
         0.5f, 0.0f,  0.5f,  0, 1, 0,  1, 1,
        -0.5f, 0.0f,  0.5f,  0, 1, 0,  0, 1,
    };
    unsigned int indices[] = { 0, 1, 2, 2, 3, 0 };
    mesh.indexCount = 6;

    glGenVertexArrays(1, &mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    glGenBuffers(1, &mesh.ebo);
    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(6*sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    return mesh;
}

MeshData CreateCylinder(int radialSegments, float radius, float height) {
    MeshData mesh;
    std::vector<float> verts;
    std::vector<unsigned int> indices;

    for (int i = 0; i <= radialSegments; i++) {
        float theta = 2.0f * M_PI * i / radialSegments;
        float x = radius * std::cos(theta);
        float z = radius * std::sin(theta);
        float nx = std::cos(theta);
        float nz = std::sin(theta);

        verts.insert(verts.end(), {x, 0.0f, z, nx, 0.0f, nz, (float)i / radialSegments, 0.0f});
        verts.insert(verts.end(), {x, height, z, nx, 0.0f, nz, (float)i / radialSegments, 1.0f});
    }

    for (int i = 0; i < radialSegments; i++) {
        unsigned int current = i * 2;
        indices.insert(indices.end(), {current, current + 1, current + 2});
        indices.insert(indices.end(), {current + 1, current + 3, current + 2});
    }

    mesh.indexCount = indices.size();
    glGenVertexArrays(1, &mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    glGenBuffers(1, &mesh.ebo);
    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    return mesh;
}

MeshData CreateCone(int radialSegments, float radius, float height) {
    MeshData mesh;
    std::vector<float> verts;
    std::vector<unsigned int> indices;

    verts.insert(verts.end(), {0.0f, height, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f});

    for (int i = 0; i <= radialSegments; i++) {
        float theta = 2.0f * M_PI * i / radialSegments;
        float x = radius * std::cos(theta);
        float z = radius * std::sin(theta);
        float nx = std::cos(theta);
        float nz = std::sin(theta);
        float ny = radius / height;
        float len = std::sqrt(nx*nx + ny*ny + nz*nz);

        verts.insert(verts.end(), {x, 0.0f, z, nx/len, ny/len, nz/len, (float)i / radialSegments, 0.0f});
    }

    for (int i = 1; i <= radialSegments; i++) {
        indices.insert(indices.end(), {0, i, i + 1});
    }

    mesh.indexCount = indices.size();
    glGenVertexArrays(1, &mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    glGenBuffers(1, &mesh.ebo);
    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    return mesh;
}

MeshData CreateTorus(int majorSegments, int minorSegments, float majorRadius, float minorRadius) {
    MeshData mesh;
    std::vector<float> verts;
    std::vector<unsigned int> indices;

    for (int i = 0; i <= majorSegments; i++) {
        float u = (float)i / majorSegments * 2.0f * M_PI;
        float cosU = std::cos(u);
        float sinU = std::sin(u);

        for (int j = 0; j <= minorSegments; j++) {
            float v = (float)j / minorSegments * 2.0f * M_PI;
            float cosV = std::cos(v);
            float sinV = std::sin(v);

            float x = (majorRadius + minorRadius * cosV) * cosU;
            float y = minorRadius * sinV;
            float z = (majorRadius + minorRadius * cosV) * sinU;

            float nx = cosV * cosU;
            float ny = sinV;
            float nz = cosV * sinU;

            verts.insert(verts.end(), {x, y, z, nx, ny, nz, (float)i / majorSegments, (float)j / minorSegments});
        }
    }

    for (int i = 0; i < majorSegments; i++) {
        for (int j = 0; j < minorSegments; j++) {
            unsigned int current = i * (minorSegments + 1) + j;
            unsigned int next = current + minorSegments + 1;
            indices.insert(indices.end(), {current, next, current + 1});
            indices.insert(indices.end(), {current + 1, next, next + 1});
        }
    }

    mesh.indexCount = indices.size();
    glGenVertexArrays(1, &mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    glGenBuffers(1, &mesh.ebo);
    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    return mesh;
}

void Cleanup(MeshData& mesh) {
    CleanupMesh(mesh);
}

const MeshData& GetCube() { return g_cube; }
const MeshData& GetSphere() { return g_sphere; }
const MeshData& GetPlane() { return g_plane; }
const MeshData& GetCylinder() { return g_cylinder; }
const MeshData& GetCone() { return g_cone; }
const MeshData& GetTorus() { return g_torus; }

void InitAll() {
    g_cube = CreateCube();
    g_sphere = CreateSphere(24, 32);
    g_plane = CreatePlane();
    g_cylinder = CreateCylinder(24);
    g_cone = CreateCone(24);
    g_torus = CreateTorus(32, 24);
}

void CleanupAll() {
    CleanupMesh(g_cube);
    CleanupMesh(g_sphere);
    CleanupMesh(g_plane);
    CleanupMesh(g_cylinder);
    CleanupMesh(g_cone);
    CleanupMesh(g_torus);
}

} // namespace Primitives