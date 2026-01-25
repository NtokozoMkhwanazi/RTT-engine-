#include "Mesh.h"
#include <iostream>

// --------------------------------------------------
Mesh::Mesh(
    std::vector<Vertex> vertices,
    std::vector<unsigned int> indices,
    std::vector<Texture> textures)
{
    this->vertices = std::move(vertices);
    this->indices  = std::move(indices);
    this->textures = std::move(textures);

    BuildBonePalette();   // ✅ palette compaction
    setupMesh();
}

// --------------------------------------------------
void Mesh::BuildBonePalette()
{
    bonePalette.globalBoneIndices.clear();
    bonePalette.globalToLocal.clear();

    // Collect used bones
    for (const auto& v : vertices)
    {
        for (int i = 0; i < MAX_BONES_PER_VERTEX; i++)
        {
            int globalID = v.BoneIDs[i];
            if (globalID < 0) continue;

            if (!bonePalette.globalToLocal.count(globalID))
            {
                int localID =
                    static_cast<int>(bonePalette.globalBoneIndices.size());

                bonePalette.globalToLocal[globalID] = localID;
                bonePalette.globalBoneIndices.push_back(globalID);
            }
        }
    }

    // Remap vertex bone IDs → local palette indices
    for (auto& v : vertices)
    {
        for (int i = 0; i < MAX_BONES_PER_VERTEX; i++)
        {
            int globalID = v.BoneIDs[i];
            if (globalID < 0) continue;

            v.BoneIDs[i] = bonePalette.globalToLocal[globalID];
        }
    }
        for (auto& v : vertices)
{
    float sum =
        v.Weights.x +
        v.Weights.y +
        v.Weights.z +
        v.Weights.w;

    if (sum > 0.0f)
        v.Weights /= sum;
}


    std::cout << "[Mesh] Bone palette size: "
              << bonePalette.globalBoneIndices.size() << "\n";
}

// --------------------------------------------------
void Mesh::setupMesh()
{
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);

    glBindVertexArray(VAO);

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(
        GL_ARRAY_BUFFER,
        vertices.size() * sizeof(Vertex),
        vertices.data(),
        GL_STATIC_DRAW
    );

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        indices.size() * sizeof(unsigned int),
        indices.data(),
        GL_STATIC_DRAW
    );

    // ---------------- Attributes ----------------

    // Position
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0, 3, GL_FLOAT, GL_FALSE,
        sizeof(Vertex),
        (void*)offsetof(Vertex, Position)
    );

    // Normal
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 3, GL_FLOAT, GL_FALSE,
        sizeof(Vertex),
        (void*)offsetof(Vertex, Normal)
    );

    // TexCoords
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(
        2, 2, GL_FLOAT, GL_FALSE,
        sizeof(Vertex),
        (void*)offsetof(Vertex, TexCoords)
    );

    // Bone IDs (INTEGER!)
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(
        3, 4, GL_INT,
        sizeof(Vertex),
        (void*)offsetof(Vertex, BoneIDs)
    );

    // Weights
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(
        4, 4, GL_FLOAT, GL_FALSE,
        sizeof(Vertex),
        (void*)offsetof(Vertex, Weights)
    );

    glBindVertexArray(0);
}

// --------------------------------------------------
void Mesh::Draw(Shader& shader)
{
    // Bind textures (optional)
    for (unsigned int i = 0; i < textures.size(); i++)
    {
        glActiveTexture(GL_TEXTURE0 + i);
        shader.setInt(textures[i].type, i);
        glBindTexture(GL_TEXTURE_2D, textures[i].id);
    }

    glBindVertexArray(VAO);

    // ✅ THIS WAS MISSING — ACTUAL DRAW CALL
    glDrawElements(
        GL_TRIANGLES,
        static_cast<GLsizei>(indices.size()),
        GL_UNSIGNED_INT,
        0
    );

    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
}

