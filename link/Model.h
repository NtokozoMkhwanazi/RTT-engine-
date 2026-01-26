#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

#include <glm/glm.hpp>

#include <assimp/scene.h>
#include <assimp/Importer.hpp>

#include "Mesh.h"
#include "Shader.h"
#include "Animation.h"
#include "Skeleton.h"

// ------------------------------------------------------------
// Model
// ------------------------------------------------------------
class Model
{
public:
    explicit Model(const std::string& path);

    void Draw(Shader& shader);

    Animation* GetAnimation(size_t index);

    const Skeleton& GetSkeleton() const { return m_Skeleton; }
    const aiScene* GetScene() const { return scene; }
    int GetRootBoneIndex() const { return rootBoneIndex; }

    Mesh& GetMesh(size_t i) { return meshes.at(i); }
    const Mesh& GetMesh(size_t i) const { return meshes.at(i); }
    size_t GetMeshCount() const { return meshes.size(); }

private:
    // --------------------------------------------------------
    // Core data
    // --------------------------------------------------------
    std::vector<Mesh> meshes;
    std::string directory;

    Assimp::Importer importer;
    const aiScene* scene = nullptr;

    // --------------------------------------------------------
    // Skeleton & animation
    // --------------------------------------------------------
    Skeleton m_Skeleton;
    int rootBoneIndex = 0;

    std::vector<std::unique_ptr<Animation>> m_Animations;

    // --------------------------------------------------------
    // Bone bookkeeping
    // --------------------------------------------------------
    int m_BoneCounter = 0;

    // --------------------------------------------------------
    // Loading
    // --------------------------------------------------------
    void loadModel(const std::string& path);

    // --------------------------------------------------------
    // Hierarchy
    // --------------------------------------------------------
    void ReadHierarchy(AssimpNodeData& dest, const aiNode* src);

    void BuildNodeBoneMap(
        AssimpNodeData& node,
        const std::unordered_map<std::string, int>& normBoneMap
    );

    // --------------------------------------------------------
    // Scene traversal
    // --------------------------------------------------------
    void processNode(aiNode* node, const aiScene* scene);
    Mesh processMesh(aiMesh* mesh, const aiScene* scene);

    // --------------------------------------------------------
    // Bones
    // --------------------------------------------------------
    void ExtractBones(aiMesh* mesh);
    void extractBoneWeights(std::vector<Vertex>& vertices, aiMesh* mesh);

    // --------------------------------------------------------
    // Materials
    // --------------------------------------------------------
    std::vector<Texture> loadMaterialTextures(
        aiMaterial* mat,
        aiTextureType type,
        const std::string& typeName
    );

    // --------------------------------------------------------
    // Utilities
    // --------------------------------------------------------
    static glm::mat4 aiMat4ToGlm(const aiMatrix4x4& m);
};

