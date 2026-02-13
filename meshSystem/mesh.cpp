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

    // ✅ CRITICAL FIX: Disable per-mesh palette remapping.
    // Vertices already have global bone IDs from Assimp (0..64).
    // Do NOT convert them to local indices — keep them global.
    // The shader and animator must stay synchronized on global indexing.
    
    std::cout << "[Mesh] BuildBonePalette: Skipping per-mesh remapping (using GLOBAL bone IDs)\n";
    std::cout << "       Vertices will keep their global bone indices (0..64).\n";
    std::cout << "       Shader will use uPaletteSize = full skeleton size (65).\n";

    // Normalize weights (they should already be normalized, but ensure it)
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

    std::cout << "[Mesh] BuildBonePalette complete: vertices store GLOBAL bone IDs\n";
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
    // Always bind VAO for drawing
    glBindVertexArray(VAO);

    // Bind textures (if present) to samplers named texture_diffuseN
    for (unsigned int i = 0; i < textures.size(); i++)
    {
        glActiveTexture(GL_TEXTURE0 + i);
        shader.setInt((std::string("texture_diffuse") + std::to_string(i + 1)).c_str(), i);
        glBindTexture(GL_TEXTURE_2D, textures[i].id);
    }

    // Draw
    glDrawElements(
        GL_TRIANGLES,
        static_cast<GLsizei>(indices.size()),
        GL_UNSIGNED_INT,
        0
    );

    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
}

