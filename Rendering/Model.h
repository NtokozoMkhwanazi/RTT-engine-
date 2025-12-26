

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

#include <assimp/scene.h>
#include <assimp/Importer.hpp>

#include "Mesh.h"
#include "Shader.h"
#include "Skeleton.h"
#include "Animation.h"
#include "AnimationConfig.h"
#include "DebugSkeleton.h"

class Model {
public:
    explicit Model(const std::string& path);

    void Draw(Shader& shader);

    // Animation / Skeleton access (READ ONLY)
    const Skeleton& GetSkeleton() const { return m_Skeleton; }
    Animation* GetAnimation(size_t index);
    const aiScene* GetScene() const { return scene; }

    glm::mat4 aiMat4ToGlm(const aiMatrix4x4& m);

private:
    // Loading
    void loadModel(const std::string& path);
    void processNode(aiNode* node, const aiScene* scene);
    Mesh processMesh(aiMesh* mesh, const aiScene* scene);

    // Bones
    void ExtractBones(aiMesh* mesh);
    void extractBoneWeights(std::vector<Vertex>& vertices, aiMesh* mesh);

    // Materials
    std::vector<Texture> loadMaterialTextures(
        aiMaterial* mat,
        aiTextureType type,
        const std::string& typeName
    );

    // Skeleton hierarchy
    void ReadHierarchy(AssimpNodeData& dest, const aiNode* src);
    void BuildNodeBoneMap(AssimpNodeData& node, const std::unordered_map<std::string, int>& normBoneMap);
    void CountBoneNodes(const AssimpNodeData& node, int& count);

private:
    std::vector<Mesh> meshes;
    std::vector<Texture> loaded_textures;
    std::string directory;

    Skeleton m_Skeleton;
    std::vector<std::unique_ptr<Animation>> m_Animations;

    Assimp::Importer importer;
    const aiScene* scene = nullptr;
};


