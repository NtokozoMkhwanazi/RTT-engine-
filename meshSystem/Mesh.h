#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

#include "../shaderSystem/Shader.h"
#include "../animationSystem/AnimationConfig.h"

// ============================================================
// Configuration
// ============================================================
// MAX_BONE_INFLUENCE, MAX_BONES, MAX_BONES_PER_VERTEX defined in AnimationConfig.h

// ============================================================
// Vertex Formats
// ============================================================

// Standard vertex with skinning support
struct Vertex
{
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec2 TexCoords;
    glm::vec3 Tangent;      // For normal mapping
    glm::vec3 Bitangent;    // For normal mapping
    glm::ivec4 BoneIDs;     // Bone indices
    glm::vec4  Weights;     // Bone weights
    
    // Get vertex size in bytes
    static size_t Size() { return sizeof(Vertex); }
};

// Lightweight vertex for static meshes (no skinning)
struct VertexStatic
{
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec2 TexCoords;
    glm::vec3 Tangent;
    glm::vec3 Bitangent;
    
    static size_t Size() { return sizeof(VertexStatic); }
};

// Instanced data structure
struct InstanceData
{
    glm::mat4 ModelMatrix;
    glm::vec4 Color;  // Can be used for tinting or other per-instance data
    
    static size_t Size() { return sizeof(InstanceData); }
};

// ============================================================
// Texture
// ============================================================
struct Texture
{
    unsigned int id;
    std::string type;
    std::string path;
    
    // Texture type enum
    enum class Type {
        DIFFUSE,
        SPECULAR,
        NORMAL,
        HEIGHT,
        METALLIC,
        ROUGHNESS,
        AO,
        EMISSIVE
    };
    
    Type textureType{Type::DIFFUSE};
    
    // Get OpenGL texture target
    GLenum GetTarget() const { return GL_TEXTURE_2D; }
};

// ============================================================
// Bone Palette
// ============================================================
struct BonePalette
{
    std::vector<int> globalBoneIndices;              // local → global
    std::unordered_map<int, int> globalToLocal;      // global → local
    
    void Clear()
    {
        globalBoneIndices.clear();
        globalToLocal.clear();
    }
    
    int AddGlobalBone(int globalIndex)
    {
        auto it = globalToLocal.find(globalIndex);
        if (it != globalToLocal.end())
            return it->second;
        
        int localIndex = static_cast<int>(globalBoneIndices.size());
        globalBoneIndices.push_back(globalIndex);
        globalToLocal[globalIndex] = localIndex;
        return localIndex;
    }
    
    int GetLocalBone(int globalIndex) const
    {
        auto it = globalToLocal.find(globalIndex);
        return (it != globalToLocal.end()) ? it->second : -1;
    }
};

// ============================================================
// Mesh Statistics
// ============================================================
struct MeshStatistics
{
    size_t vertexCount{0};
    size_t indexCount{0};
    size_t triangleCount{0};
    size_t boneCount{0};
    size_t textureCount{0};
    float memoryUsageMB{0.0f};
    
    void Calculate(const std::vector<Vertex>& verts, 
                   const std::vector<unsigned int>& inds,
                   const std::vector<Texture>& texs)
    {
        vertexCount = verts.size();
        indexCount = inds.size();
        triangleCount = inds.size() / 3;
        textureCount = texs.size();
        
        // Calculate memory usage
        memoryUsageMB = (verts.size() * sizeof(Vertex) + 
                        inds.size() * sizeof(unsigned int)) / (1024.0f * 1024.0f);
    }
};

// ============================================================
// Bounding Volumes
// ============================================================
struct BoundingBox
{
    glm::vec3 min{FLT_MAX, FLT_MAX, FLT_MAX};
    glm::vec3 max{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    
    bool IsValid() const
    {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }
    
    glm::vec3 Center() const { return (min + max) * 0.5f; }
    glm::vec3 Size() const { return max - min; }
    glm::vec3 Extent() const { return Size() * 0.5f; }
    float Radius() const { return glm::length(Size() * 0.5f); }
    
    void Extend(const glm::vec3& point)
    {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }
    
    void Extend(const BoundingBox& other)
    {
        if (other.IsValid())
        {
            Extend(other.min);
            Extend(other.max);
        }
    }
    
    bool Contains(const glm::vec3& point) const
    {
        return point.x >= min.x && point.x <= max.x &&
               point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }
    
    bool Intersects(const BoundingBox& other) const
    {
        return (min.x <= other.max.x && max.x >= other.min.x) &&
               (min.y <= other.max.y && max.y >= other.min.y) &&
               (min.z <= other.max.z && max.z >= other.min.z);
    }
};

struct BoundingSphere
{
    glm::vec3 center{0.0f};
    float radius{0.0f};
    
    bool IsValid() const { return radius > 0.0f; }
    
    static BoundingSphere FromBoundingBox(const BoundingBox& bbox)
    {
        BoundingSphere sphere;
        sphere.center = bbox.Center();
        sphere.radius = bbox.Radius();
        return sphere;
    }
};

// ============================================================
// Mesh Configuration
// ============================================================
enum class MeshFlags
{
    None = 0,
    HasSkinning = 1 << 0,
    HasTangents = 1 << 1,
    HasNormals = 1 << 2,
    HasUVs = 1 << 3,
    IsDynamic = 1 << 4,      // Buffer will be updated frequently
    UseInstancing = 1 << 5,  // Enable instanced rendering
};

inline MeshFlags operator|(MeshFlags a, MeshFlags b)
{
    return static_cast<MeshFlags>(static_cast<int>(a) | static_cast<int>(b));
}

inline MeshFlags operator&(MeshFlags a, MeshFlags b)
{
    return static_cast<MeshFlags>(static_cast<int>(a) & static_cast<int>(b));
}

// ============================================================
// Mesh Class
// ============================================================
class Mesh
{
public:
    // Data
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::vector<Texture> textures;
    
    BonePalette bonePalette;
    MeshFlags flags{MeshFlags::None};
    
    // Statistics
    MeshStatistics stats;
    
    // Bounding volumes
    BoundingBox boundingBox;
    BoundingSphere boundingSphere;
    
    // OpenGL
    unsigned int VAO = 0;
    
    // Constructors
    Mesh(std::vector<Vertex> vertices,
         std::vector<unsigned int> indices,
         std::vector<Texture> textures = {});
    
    Mesh() = default;

    // Move-only: OpenGL handles are not copyable
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    
    // Destructor
    ~Mesh();
    
    // Drawing
    void Draw(Shader& shader);
    void DrawInstanced(Shader& shader, size_t instanceCount);
    void DrawInstanced(Shader& shader, const std::vector<InstanceData>& instances);
    
    // Setup
    void SetupMesh();
    void UpdateVertexBuffer();  // For dynamic meshes
    void UpdateIndexBuffer();   // Update index buffer after optimization

    // Bounding volumes
    void CalculateBoundingVolumes();
    const BoundingBox& GetBoundingBox() const { return boundingBox; }
    const BoundingSphere& GetBoundingSphere() const { return boundingSphere; }
    
    // Statistics
    const MeshStatistics& GetStatistics() const { return stats; }
    
    // Utilities
    void RecalculateNormals();
    void RecalculateTangents();
    void Optimize();  // Uses Forsyth triangle ordering (advanced optimization)
    
    // Bone palette
    void BuildBonePalette();
    
    // Flags
    void SetFlag(MeshFlags flag, bool enabled);
    bool HasFlag(MeshFlags flag) const { return (flags & flag) != MeshFlags::None; }

    // Get OpenGL buffer IDs
    unsigned int GetVAO() const { return VAO; }
    unsigned int GetVBO() const { return VBO; }
    unsigned int GetEBO() const { return EBO; }

    // Memory management
    void Clear();
    size_t GetMemoryUsage() const;

private:
    unsigned int VBO = 0;
    unsigned int EBO = 0;
    unsigned int instanceVBO = 0;
    
    bool setupDone = false;
    
    void SetupVertexAttributes();
    void SetupInstanceAttributes();
};

// ============================================================
// Mesh Utilities (Free Functions)
// ============================================================
namespace MeshUtils
{
    // Calculate tangents and bitangents
    void CalculateTangents(std::vector<Vertex>& vertices,
                          const std::vector<unsigned int>& indices);

    // Recalculate normals from geometry
    void RecalculateNormals(std::vector<Vertex>& vertices,
                           const std::vector<unsigned int>& indices);

    // Optimizes vertex cache for better GPU performance
    // DEPRECATED: Use OptimizeTriangleOrderingForsyth for superior results
    [[deprecated("Use OptimizeTriangleOrderingForsyth instead")]]
    void OptimizeVertexCache(std::vector<unsigned int>& indices,
                            size_t vertexCount);

    // Simplify mesh (basic vertex decimation)
    void SimplifyMesh(std::vector<Vertex>& vertices,
                     std::vector<unsigned int>& indices,
                     float reductionRatio);

    // Merge multiple meshes
    Mesh MergeMeshes(const std::vector<Mesh>& meshes);

    // Transform mesh
    void TransformMesh(Mesh& mesh, const glm::mat4& transform);

    // Flip UV coordinates
    void FlipUVs(std::vector<Vertex>& vertices, bool flipU = false, bool flipV = true);

    // Center mesh at origin
    glm::mat4 CenterMesh(std::vector<Vertex>& vertices);

    // ============================================================
    // Advanced Mesh Optimization
    // ============================================================

    // Fast Triangle Reordering (Forsyth Algorithm)
    // Optimizes triangle order for maximum post-transform cache hits
    // Returns the optimized index buffer
    void OptimizeTriangleOrderingForsyth(std::vector<unsigned int>& indices,
                                         size_t vertexCount,
                                         size_t cacheSize = 24);

    // Vertex Clustering for Mesh Reduction
    // Clusters vertices within a 3D grid and merges them
    // gridCellSize: size of each grid cell (world units)
    // Returns simplified mesh data
    void VertexClustering(std::vector<Vertex>& vertices,
                         std::vector<unsigned int>& indices,
                         float gridCellSize);

    // Combined optimization: reorder + cluster
    // Best for maximum performance gain
    struct MeshOptimizationConfig
    {
        bool reorderTriangles = true;
        bool clusterVertices = false;
        float clusterCellSize = 0.1f;  // Adjust based on mesh scale
        size_t targetCacheSize = 24;
    };

    void OptimizeMeshForRendering(Mesh& mesh, 
                                  const MeshOptimizationConfig& config);
}
