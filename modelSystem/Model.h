#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <cfloat>
#include <iostream>
#include <future>
#include <atomic>

#include <glm/glm.hpp>

#include <assimp/scene.h>
#include <assimp/Importer.hpp>

#include "../meshSystem/Mesh.h"
#include "../shaderSystem/Shader.h"
#include "../animationSystem/Animation.h"
#include "../animationSystem/Animator.h"
#include "../animationSystem/BoneMatrixBuffer.h"
#include "../boneSystem/Skeleton.h"
#include "../boneSystem/BoneName.h"

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
// Async Model Loading Data
// ============================================================
struct TexturePixelData {
    std::string path;
    std::string type;       // "diffuse", "normal", etc.
    int width{0};
    int height{0};
    int channels{0};
    std::vector<uint8_t> data;
    bool isNormalMap{false};
};

struct AsyncModelData {
    // Raw mesh data (CPU only, no GL resources)
    struct RawMeshData {
        std::vector<Vertex> vertices;
        std::vector<unsigned int> indices;
        PBRMaterial material;
        std::string directory;
        std::vector<std::pair<std::string, std::string>> texturePaths; // (path, type)
    };
    
    std::vector<RawMeshData> meshes;
    std::vector<TexturePixelData> textures;
    std::vector<PBRMaterial> meshMaterials;
    
    // Skeleton & animation
    Skeleton skeleton;
    int rootBoneIndex{0};
    std::vector<std::unique_ptr<Animation>> animations;
    AssimpNodeData rootNode;
    glm::mat4 globalInverseTransform{1.0f};
    
    // Bounding volumes
    BoundingBox boundingBox;
    BoundingSphere boundingSphere;
    
    // Metadata
    std::string sourcePath;
    std::string directory;
    int totalTriangles{0};
    int totalVertices{0};
    bool success{false};
    std::string error;
};

// ============================================================
// Model Class
// ============================================================
// Model Class
// ============================================================
class Model
{
public:
    explicit Model(const std::string& path = "");
    ~Model();

    // Async loading handle
    struct AsyncLoadHandle {
        std::future<std::unique_ptr<AsyncModelData>> future;
        float progress{0.0f};
        bool isValid() const { return future.valid(); }
        bool isComplete() const { return future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
    };

    // Factory method for programmatic mesh creation
    static Model* CreateFromVAO(GLuint VAO, GLsizei indexCount);

    // Async loading factory methods
    static AsyncLoadHandle LoadAsync(const std::string& path);
    static Model* CreateFromAsync(AsyncLoadHandle& handle, std::string* outError = nullptr);
    static std::unique_ptr<AsyncModelData> LoadModelData(const std::string& path, float* outProgress = nullptr);

    // Debug methods for programmatic mesh creation
    void setDebugVAO(GLuint VAO) { m_debugVAO = VAO; }
    void setDebugIndexCount(GLsizei count) { m_debugIndexCount = count; }
    GLuint getDebugVAO() const { return m_debugVAO; }
    GLsizei getDebugIndexCount() const { return m_debugIndexCount; }
    
    // Rendering
    void Draw(Shader& shader, Animator& animator);
    void DrawStatic(Shader& shader);  // For models without animations
    void DrawLOD(Shader& shader, Animator& animator, const glm::vec3& cameraPos, float lodBias = 1.0f);
    void DrawInstanced(Shader& shader, Animator& animator, const std::vector<ModelInstance>& instances);
    
    // Bone texture
    void UploadBoneTexture(Shader& shader, const std::vector<glm::mat4>& mats);
    
    // Animation
    Animation* GetAnimation(size_t index);
    const Animation* GetAnimation(size_t index) const;
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
    size_t GetMeshCount() const { 
        return meshes.size(); 
    }
    
    // Materials
    void SetMeshMaterial(size_t meshIndex, const PBRMaterial& material);
    const PBRMaterial& GetMeshMaterial(size_t meshIndex) const;
    size_t GetMeshMaterialCount() const { return meshMaterials.size(); }
    
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
    
    // Per-model texture cache: many glTF assets have dozens of meshes that
    // reference the SAME texture file. Without this, each mesh re-decodes the
    // (often 4K) image and re-runs CPU BC1 compression - minutes of redundant
    // work per model. Path -> GL texture id (0 = failed, cached to avoid
    // repeating the lookup).
    std::unordered_map<std::string, unsigned int> m_textureCache;
    
    // Scratch buffer that keeps downsampled texture pixels alive for the
    // duration of loadTexture() when a 4K+ map is reduced to 1024 before
    // CPU BC1 compression. Owned by the model so the raw pointer stays valid
    // until the upload + compression finish.
    std::vector<unsigned char> m_resizedTexBuffer;
    
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
    
    // Bone texture (legacy - kept for compatibility)
    unsigned int boneTexID = 0;
    
    // SSBO/UBO for bone matrices (fast path)
    BoneMatrixBuffer boneBuffer;

    // Debug output flag
    bool debugOutput = false;

    // Debug VAO for programmatic meshes
    GLuint m_debugVAO = 0;
    GLsizei m_debugIndexCount = 0;
    
    // Loading
    void loadModel(const std::string& path);
    void calculateBoundingVolumes();
    void setupFromAsyncData(std::unique_ptr<AsyncModelData> data, std::string& outError);
    unsigned int uploadTextureFromPixels(const TexturePixelData& texData);
    
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
    PBRMaterial processMaterial(aiMaterial* mat, const std::string& directory, const aiScene* scene);
    std::vector<Texture> loadMaterialTextures(
        aiMaterial* mat,
        aiTextureType type,
        const std::string& typeName
    );
    unsigned int loadTexture(const std::string& path, aiTextureType type);

    // Embedded-texture support: many FBX exports (Mixamo characters) store
    // the texture pixels INSIDE the file (FBX Video nodes) and reference them
    // by the original path - usually an absolute temp path like
    // "/tmp/.../skins_xxx.fbm/Vampire_diffuse.png" that does not exist on
    // disk. Without decoding these, clothed characters render with the flat
    // default grey.
    const aiTexture* findEmbeddedTexture(const aiScene* scene, const std::string& path) const;
    unsigned int loadEmbeddedTexture(const aiScene* scene, const std::string& path, aiTextureType type);
    // Shared upload of decoded pixels (downscale + BC1/BC3 compression + GL
    // upload), used by both loadTexture (disk) and embedded textures.
    unsigned int uploadImageData(unsigned char* data, int width, int height,
                                 int nrComponents, aiTextureType type, bool ownedByStbi);
    
    // LOD
    void generateLODLevels();
    
    // Utilities
    static glm::mat4 aiMat4ToGlm(const aiMatrix4x4& m);
    
    // Static helpers for async loading (no 'this' pointer)
    static void ReadHierarchyStatic(AssimpNodeData& dest, const aiNode* src);
    static void BuildNodeBoneMapStatic(AssimpNodeData& node, const std::unordered_map<std::string, int>& normBoneMap);
    static void ExtractBonesStatic(aiMesh* mesh, Skeleton& skeleton);
    static void extractBoneWeightsStatic(std::vector<Vertex>& vertices, aiMesh* mesh, Skeleton& skeleton);
};
