#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <cfloat>

#include <glm/glm.hpp>

#include <assimp/scene.h>
#include <assimp/Importer.hpp>

#include "meshSystem/Mesh.h"
#include "shaderSystem/Shader.h"
#include "animationSystem/Animation.h"
#include "animationSystem/Animator.h"
#include "boneSystem/Skeleton.h"
#include "boneSystem/BoneName.h"

// Use canonical bone name normalization from BoneName.h
// ------------------------------------------------------------
// Model
// ------------------------------------------------------------
class Model
{
public:
    explicit Model(const std::string& path);
    unsigned int boneTexID = 0;
    
    void Draw(Shader& shader, Animator& animator);
    void UploadBoneTexture(Shader& shader, const std::vector<glm::mat4>& mats);
    Animation* GetAnimation(size_t index);
    glm::vec3 GetSize() const;
    void DebugDrawSkeleton(const std::vector<glm::mat4>& boneTransforms, const Skeleton& skeleton);

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
    void ReadHierarchyRecursive(AssimpNodeData& dest, const aiNode* src);
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
    void ExtractBones(aiMesh* mesh, const AssimpNodeData& rootNode);
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

