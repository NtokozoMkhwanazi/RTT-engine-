#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <string>
#include <vector>
#include <unordered_map>

#include "Shader.h"

constexpr int MAX_BONES_PER_VERTEX = 4;

// ---------------- Vertex ----------------
struct Vertex
{
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec2 TexCoords;

    glm::ivec4 BoneIDs;   // LOCAL bone indices (after palette remap)
    glm::vec4  Weights;
};

// ---------------- Texture ----------------
struct Texture
{
    unsigned int id;
    std::string type;
    std::string path;
};

// ---------------- Bone Palette ----------------
struct BonePalette
{
    std::vector<int> globalBoneIndices;              // local → global
    std::unordered_map<int, int> globalToLocal;      // global → local
};

// ---------------- Mesh ----------------
class Mesh
{
public:
    std::vector<Vertex>        vertices;
    std::vector<unsigned int>  indices;
    std::vector<Texture>       textures;

    BonePalette bonePalette;

    unsigned int VAO = 0;

    Mesh(
        std::vector<Vertex> vertices,
        std::vector<unsigned int> indices,
        std::vector<Texture> textures
    );

    void Draw(Shader& shader);

private:
    unsigned int VBO = 0;
    unsigned int EBO = 0;

    void setupMesh();
    void BuildBonePalette();
};

