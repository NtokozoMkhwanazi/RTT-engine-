// Mesh.h (partial)
#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include "Shader.h"
#include "AnimationConfig.h"

const int MAX_BONES_PER_VERTEX = 4;

struct Vertex {
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec2 TexCoords;
    int BoneIDs[MAX_BONES_PER_VERTEX];
    float Weights[MAX_BONES_PER_VERTEX];

    Vertex() {
        for (int i=0;i<MAX_BONES_PER_VERTEX;i++){ BoneIDs[i]=0; Weights[i]=0.0f; }
    }
};

struct Texture {
    unsigned int id;
    std::string type;
    std::string path;
};

class Mesh {
public:
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::vector<Texture> textures;
    unsigned int VAO;

    Mesh(std::vector<Vertex> vertices, std::vector<unsigned int> indices, std::vector<Texture> textures);
    void Draw(Shader& shader);

private:
    unsigned int VBO, EBO;
    void setupMesh();
    void AddBoneDataToVertex(unsigned int vertexID, int boneID, float weight);
};

