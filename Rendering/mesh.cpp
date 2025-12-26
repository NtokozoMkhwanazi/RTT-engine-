// Mesh.cpp (partial)
#include "Mesh.h"
#include <iostream>

Mesh::Mesh(std::vector<Vertex> vertices, std::vector<unsigned int> indices, std::vector<Texture> textures) {
    this->vertices = vertices;
    this->indices = indices;
    this->textures = textures;
    setupMesh();
}

void Mesh::AddBoneDataToVertex(unsigned int vertexID, int boneID, float weight)
{
    Vertex& v = vertices[vertexID];

    for (int i = 0; i < MAX_BONES_PER_VERTEX; i++)
    {
        if (v.Weights[i] == 0.0f)
        {
            v.BoneIDs[i] = boneID;
            v.Weights[i] = weight;
            std::cout <<"BoneId : "<< v.BoneIDs[i]<< std::endl;
            std::cout << " Weights : "<< v.Weights[i]<<std::endl;
            return;
        }
    }

    // extra influences are ignored (acceptable)
}


void Mesh::setupMesh() {
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);

    glBindVertexArray(VAO);

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

    // pos (layout 0)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Position));
    // normal (layout 1)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Normal));
    // texcoords (layout 2)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, TexCoords));
    // bone IDs (layout 3) - integer attribute
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 4, GL_INT, sizeof(Vertex), (void*)offsetof(Vertex, BoneIDs));
    // bone weights (layout 4)
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Weights));

    glBindVertexArray(0);
}

void Mesh::Draw(Shader& shader) {
    // Bind PBR textures as before (if any). This code assumes you already handle texture binding.
    for (unsigned int i = 0; i < textures.size(); i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        std::string name = textures[i].type;
        if (name == "albedo") shader.setInt("albedoMap", i);
        else if (name == "normal") shader.setInt("normalMap", i);
        else if (name == "metallic") shader.setInt("metallicMap", i);
        else if (name == "roughness") shader.setInt("roughnessMap", i);
        else if (name == "ao") shader.setInt("aoMap", i);
        glBindTexture(GL_TEXTURE_2D, textures[i].id);
    }

    glBindVertexArray(VAO);
    glDrawElements(GL_TRIANGLES, (GLsizei)indices.size(), GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    glActiveTexture(GL_TEXTURE0);
}

