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

// ============================================================
// Material Types
// ============================================================
enum class MaterialType {
    STANDARD,      // Basic diffuse + specular
    PBR_METALLIC,  // PBR with metallic workflow
    PBR_ROUGHNESS, // PBR with roughness workflow
    EMISSIVE,      // Self-illuminated
    TRANSPARENT    // Alpha-blended materials
};

// ============================================================
// PBR Material Structure
// ============================================================
struct PBRMaterial {
    // Base properties
    glm::vec3 albedo{0.8f, 0.8f, 0.8f};
    float metallic{0.0f};
    float roughness{0.5f};
    float ao{1.0f};
    float alpha{1.0f};
    
    // Emissive
    glm::vec3 emissive{0.0f, 0.0f, 0.0f};
    float emissiveStrength{1.0f};
    
    // Normal mapping
    float normalScale{1.0f};
    
    // Texture IDs
    unsigned int albedoMap{0};
    unsigned int normalMap{0};
    unsigned int metallicMap{0};
    unsigned int roughnessMap{0};
    unsigned int aoMap{0};
    unsigned int emissiveMap{0};
    
    // Texture presence flags
    bool hasAlbedoMap{false};
    bool hasNormalMap{false};
    bool hasMetallicMap{false};
    bool hasRoughnessMap{false};
    bool hasAOMap{false};
    bool hasEmissiveMap{false};
    
    // Material type
    MaterialType type{MaterialType::PBR_METALLIC};
    
    // Culling
    bool doubleSided{false};
    bool useAlphaTest{false};
    float alphaTestThreshold{0.5f};
};

// ============================================================
// LOD Level
// ============================================================
struct LODLevel {
    Mesh mesh;
    float distanceThreshold; // Switch to this LOD at this distance
    int triangleCount;
    
    LODLevel(Mesh m, float dist) 
        : mesh(std::move(m)), distanceThreshold(dist), triangleCount(0) {}
};

// ============================================================
// Model Instance Data (for instancing)
// ============================================================
struct ModelInstance {
    glm::mat4 modelMatrix{1.0f};
    glm::vec3 color{1.0f, 1.0f, 1.0f};
    bool enabled{true};
    
    // Per-instance animation data
    Animator* animator{nullptr};
};

// ============================================================
// Model Class
// ============================================================
class Model
{
public:
    explicit Model(const std::string& path);
    ~Model();
    
    // Rendering
    void Draw(Shader& shader, Animator& animator);
    void DrawLOD(Shader& shader, Animator& animator, const glm::vec3& cameraPos, float lodBias = 1.0f);
    void DrawInstanced(Shader& shader, Animator& animator, const std::vector<ModelInstance>& instances);
    
    // Bone texture
    void UploadBoneTexture(Shader& shader, const std::vector<glm::mat4>& mats);
    
    // Animation
    Animation* GetAnimation(size_t index);
    size_t GetAnimationCount() const { return m_Animations.size(); }
    
    // Model info
    glm::vec3 GetSize() const;
    BoundingBox GetBoundingBox() const { return boundingBox; }
    BoundingSphere GetBoundingSphere() const { return boundingSphere; }
    
    // Skeleton
    const Skeleton& GetSkeleton() const { return m_Skeleton; }
    const aiScene* GetScene() const { return scene; }
    int GetRootBoneIndex() const { return rootBoneIndex; }
    
    // Meshes
    Mesh& GetMesh(size_t i) { return meshes.at(i); }
    const Mesh& GetMesh(size_t i) const { return meshes.at(i); }
    size_t GetMeshCount() const { return meshes.size(); }
    
    // Materials
    void SetMeshMaterial(size_t meshIndex, const PBRMaterial& material);
    const PBRMaterial& GetMeshMaterial(size_t meshIndex) const;
    
    // LOD
    void AddLODLevel(const std::string& lodModelPath, float distanceThreshold);
    size_t GetLODLevelCount() const { return lodLevels.size(); }
    
    // Statistics
    int GetTotalTriangleCount() const;
    int GetVertexCount() const;
    
    // Debug
    void DebugDrawSkeleton(const std::vector<glm::mat4>& boneTransforms, const Skeleton& skeleton);
    
    // Resource management
    void EnableDebugOutput(bool enable) { debugOutput = enable; }

private:
    // Core data
    std::vector<Mesh> meshes;
    std::vector<PBRMaterial> meshMaterials;
    std::string directory;
    
    Assimp::Importer importer;
    const aiScene* scene = nullptr;
    
    // Skeleton & animation
    Skeleton m_Skeleton;
    int rootBoneIndex = 0;
    std::vector<std::unique_ptr<Animation>> m_Animations;
    
    // LOD
    std::vector<std::vector<LODLevel>> lodLevels; // Per-mesh LOD levels
    
    // Bounding volumes
    BoundingBox boundingBox;
    BoundingSphere boundingSphere;
    
    // Bone bookkeeping
    int m_BoneCounter = 0;
    unsigned int boneTexID = 0;
    
    // Debug output flag
    bool debugOutput = false;
    
    // Loading
    void loadModel(const std::string& path);
    void calculateBoundingVolumes();
    
    // Hierarchy
    void ReadHierarchyRecursive(AssimpNodeData& dest, const aiNode* src, const glm::mat4& accumulatedTransform);
    void ReadHierarchyRecursive(AssimpNodeData& dest, const aiNode* src);  // Overload for backward compatibility
    void ReadHierarchy(AssimpNodeData& dest, const aiNode* src);
    void BuildNodeBoneMap(
        AssimpNodeData& node,
        const std::unordered_map<std::string, int>& normBoneMap
    );
    
    // Scene traversal
    void processNode(aiNode* node, const aiScene* scene);
    Mesh processMesh(aiMesh* mesh, const aiScene* scene);
    
    // Bones
    void ExtractBones(aiMesh* mesh, const AssimpNodeData& rootNode);
    void extractBoneWeights(std::vector<Vertex>& vertices, aiMesh* mesh);
    
    // Materials
    PBRMaterial processMaterial(aiMaterial* mat, const std::string& directory);
    std::vector<Texture> loadMaterialTextures(
        aiMaterial* mat,
        aiTextureType type,
        const std::string& typeName
    );
    unsigned int loadTexture(const std::string& path, aiTextureType type);
    
    // LOD
    void generateLODLevels();
    
    // Utilities
    static glm::mat4 aiMat4ToGlm(const aiMatrix4x4& m);
};
