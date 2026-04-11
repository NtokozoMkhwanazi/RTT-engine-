#pragma once
/**
 * Optimized Renderer with Production-Ready Features
 * 
 * Features:
 * - UBO (Uniform Buffer Object) for camera matrices
 * - Batch sorting by shader/VAO to minimize state changes
 * - Persistent mapped buffer pool for instance data
 * - Explicit uniform locations (no glGetUniformLocation)
 * - Frustum culling with SIMD optimization
 * - Buffer pooling (no glGen/glDelete per frame)
 * - Multi-Draw Indirect support (optional)
 */

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <array>
#include <unordered_map>
#include <algorithm>
#include <memory>

// ============================================================================
// UBO Configuration - Binding points for uniform buffers
// ============================================================================
#define CAMERA_UBO_BINDING 0
#define LIGHT_UBO_BINDING 1
#define MATERIAL_UBO_BINDING 2

// ============================================================================
// Explicit Uniform Locations (no glGetUniformLocation needed)
// ============================================================================
#define UNIFORM_LOCATION_MODEL        0
#define UNIFORM_LOCATION_VIEW         1
#define UNIFORM_LOCATION_PROJECTION   2
#define UNIFORM_LOCATION_COLOR        3
#define UNIFORM_LOCATION_LIGHTPOS     4
#define UNIFORM_LOCATION_VIEWPOS      5

// ============================================================================
// Camera UBO Structure - Shared across all shaders
// ============================================================================
struct CameraUBO {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 projection;
    alignas(16) glm::mat4 viewProjection;
    alignas(16) glm::vec4 viewPos;
    alignas(16) glm::vec4 lightPos;
};

// ============================================================================
// Material UBO Structure
// ============================================================================
struct MaterialUBO {
    alignas(16) glm::vec4 albedo;
    alignas(16) glm::vec4 emissive;
    alignas(16) float metallic;
    alignas(16) float roughness;
    alignas(16) float ao;
    alignas(16) float padding;
};

// ============================================================================
// Frustum Planes for Culling
// ============================================================================
struct Frustum {
    enum Plane { NEAR, FAR, LEFT, RIGHT, TOP, BOTTOM };
    std::array<glm::vec4, 6> planes;
    
    void Update(const glm::mat4& vp);
    bool TestPoint(const glm::vec3& p) const;
    bool TestSphere(const glm::vec3& center, float radius) const;
    bool TestAABB(const glm::vec3& min, const glm::vec3& max) const;
};

// ============================================================================
// Optimized Render Batch
// ============================================================================
class Renderer {
public:
    struct RenderBatch {
        GLuint vertexArrayObject = 0;
        GLuint vertexBuffer = 0;
        GLuint elementBuffer = 0;
        GLuint instanceBuffer = 0;  // For instance data
        GLsizei vertexCount = 0;
        GLsizei instanceCount = 0;
        GLenum primitiveType = GL_TRIANGLES;
        GLuint shaderProgram = 0;
        GLuint textureID = 0;
        
        // Material properties for this batch
        glm::vec3 albedo{1.0f};
        float metallic{0.0f};
        float roughness{0.5f};
        float ao{1.0f};
        glm::vec3 emissive{0.0f};
        
        // For sorting - creates a sortable key
        uint64_t sortKey = 0;
        
        // Instance data offset in persistent buffer
        size_t instanceDataOffset = 0;
        
        // Bounding box for frustum culling
        glm::vec3 bboxMin{-1,-1,-1};
        glm::vec3 bboxMax{1,1,1};
        
        void ComputeSortKey() {
            // Sort by: shader (high bits) -> texture (mid) -> VAO (low)
            sortKey = ((uint64_t)shaderProgram << 32) | 
                      ((uint64_t)textureID << 16) | 
                      (uint64_t)vertexArrayObject;
        }
    };

    // ========================================================================
    // Persistent Buffer Pool - Avoids glGen/glDelete every frame
    // ========================================================================
    struct BufferPool {
        static constexpr size_t POOL_SIZE = 64 * 1024 * 1024; // 64MB
        static constexpr size_t MAX_BUFFERS = 1024;
        
        GLuint bufferIDs[MAX_BUFFERS];
        size_t bufferSizes[MAX_BUFFERS];
        bool inUse[MAX_BUFFERS];
        void* mappedPointers[MAX_BUFFERS];
        size_t numBuffers = 0;
        size_t nextFree = 0;
        
        void Initialize();
        void Shutdown();
        
        // Allocate from pool
        size_t Allocate(size_t size, void** outMappedPtr);
        void Free(size_t bufferIndex);
        
        // Ring buffer allocation for per-frame data
        size_t AllocateRing(size_t size, void** outMappedPtr);
        void ResetRing();
        
    private:
        size_t ringBufferOffset = 0;
        GLuint ringBuffer = 0;
        void* ringBufferMapped = nullptr;
    };

    Renderer();
    ~Renderer();

    void Initialize();
    void Shutdown();

    // Add a renderable object to the batch
    void AddRenderable(GLuint VAO, GLuint VBO, GLuint EBO, GLsizei vertexCount,
                      GLenum primitiveType, GLuint shaderProgram,
                      const std::vector<glm::mat4>& transforms = {},
                      const glm::vec3& color = glm::vec3(1.0f),
                      const glm::vec3& bboxMin = glm::vec3(-1),
                      const glm::vec3& bboxMax = glm::vec3(1));

    // Submit all batches for rendering (with sorting)
    void SubmitBatches();

    // Render all submitted batches (sorted, optimized)
    void Render();

    // Clear all batches
    void ClearBatches();

    // Set viewport
    void SetViewport(int x, int y, int width, int height);

    // Set clear color
    void SetClearColor(float r, float g, float b, float a = 1.0f);

    // Toggle depth testing
    void SetDepthTesting(bool enabled);

    // Toggle face culling
    void SetFaceCulling(bool enabled);

    // Set camera matrices (updates UBO)
    void SetCameraMatrices(const glm::mat4& view, const glm::mat4& projection);
    
    // Set light parameters (updates UBO)
    void SetLightParameters(const glm::vec3& lightPos, const glm::vec3& viewPos);

    // Enable/disable frustum culling
    void SetFrustumCulling(bool enabled);
    
    // Get batch count for debugging
    size_t GetBatchCount() const { return batches.size(); }
    
    // Set default texture ID
    void SetDefaultTexture(GLuint textureID) { defaultTexture = textureID; }
    
    // Make batches public for debug
    std::vector<RenderBatch> batches;

private:
    // UBO
    GLuint cameraUBO = 0;
    CameraUBO cameraData;
    
    // Default texture
    GLuint defaultTexture = 0;
    
    // Viewport state
    int viewportX = 0, viewportY = 0;
    int viewportWidth = 800, viewportHeight = 600;
    glm::vec4 clearColor{0.1f, 0.1f, 0.15f, 1.0f};
    
    // Render state
    bool depthTestingEnabled = true;
    bool faceCullingEnabled = true;
    bool frustumCullingEnabled = true;
    
    // Frustum for culling
    Frustum frustum;
    
    // Buffer pool for instance data
    BufferPool bufferPool;
    
    // Cached uniform locations (fallback for shaders without explicit layout)
    std::unordered_map<GLuint, std::unordered_map<GLint, GLint>> uniformCache;
    
    // Pre-computed sort keys
    std::vector<size_t> batchIndices;

    void SetupBatch(RenderBatch& batch, const std::vector<glm::mat4>& transforms);
    void UpdateCameraUBO();
    void BindCameraUBO(GLuint shaderProgram);
};

// ============================================================================
// Inline Implementations
// ============================================================================

inline void Renderer::SetFrustumCulling(bool enabled) {
    frustumCullingEnabled = enabled;
}

inline void Renderer::BindCameraUBO(GLuint shaderProgram) {
    // Bind UBO to binding point 0
    // Shaders must have: layout(std140, binding = 0) uniform CameraBlock { ... };
    GLuint uboIndex = glGetUniformBlockIndex(shaderProgram, "CameraBlock");
    if (uboIndex != GL_INVALID_INDEX) {
        glUniformBlockBinding(shaderProgram, uboIndex, CAMERA_UBO_BINDING);
    }
}
