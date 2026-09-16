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
#include <string>
#include <unordered_map>
#include <algorithm>
#include <memory>

// ============================================================================
// UBO Configuration - Binding points for uniform buffers
// ============================================================================
#define CAMERA_UBO_BINDING 0
#define LIGHT_UBO_BINDING 1
#define MATERIAL_UBO_BINDING 2
// Bone matrix UBO/SSBO used to live at binding = 0 alongside CAMERA_UBO_BINDING.
// That collision is now resolved — bone buffers own binding point 3.
// VS.glsl must declare layout(std140, binding = 3) uniform BoneMatricesUBO {...}
// and BoneMatrixBuffer::Bind() must be called with BONE_BUFFER_BINDING.
#define BONE_BUFFER_BINDING 3

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
// Multi-light support: the block carries an array of light positions + colors
// plus a count (std140: arrays of vec4 are tightly packed at 16-byte stride).
// Must stay in sync with the CameraBlock layout in editor/shader_manager.cpp.
constexpr int kMaxLights = 8;

struct RenderLight {
    glm::vec3 position;
    glm::vec3 color;
    float intensity;
};

struct CameraUBO {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 projection;
    alignas(16) glm::mat4 viewProjection;
    alignas(16) glm::vec4 viewPos;
    alignas(16) glm::vec4 lightPositions[kMaxLights];
    alignas(16) glm::vec4 lightColors[kMaxLights];
    alignas(16) int lightCount;
    alignas(16) int lightPad[3];  // std140 block size stays a multiple of 16
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
        
        // Model matrix for this batch. Fed to the shader via the uModel/model
        // uniform (the mesh VAOs only define attributes 0-6, so the instanced
        // attribute path cannot carry per-batch transforms reliably when
        // several batches share a VAO - the playable character's draw path
        // renders correctly for the same reason: it uses a uniform matrix).
        glm::mat4 modelMatrix{1.0f};
        
        void ComputeSortKey() {
            // Sort by: shader (high bits) -> texture (mid) -> VAO (low)
            sortKey = ((uint64_t)shaderProgram << 32) | 
                      ((uint64_t)textureID << 16) | 
                      (uint64_t)vertexArrayObject;
        }
    };

    // ========================================================================
    // Persistent Buffer Pool - Ring buffer with persistent mapping
    // ========================================================================
    struct BufferPool {
        static constexpr size_t POOL_SIZE = 64 * 1024 * 1024; // 64MB
        static constexpr size_t MAX_BUFFERS = 1024;
        static constexpr size_t MAX_FRAMES_IN_FLIGHT = 3;
        
        GLuint bufferIDs[MAX_BUFFERS];
        size_t bufferSizes[MAX_BUFFERS];
        bool inUse[MAX_BUFFERS];
        void* mappedPointers[MAX_BUFFERS];
        size_t numBuffers = 0;
        size_t nextFree = 0;

        // Persistent mapped ring buffer for per-frame uploads
        GLuint ringBuffer = 0;
        void* ringMappedPtr = nullptr;
        size_t ringBufferSize = 0;
        size_t ringOffset = 0;
        bool ringBufferSupported = false;

        // Frame sync: track GPU fence per ring buffer allocation
        struct SyncEntry {
            GLuint64 fence;
            size_t offset;
            size_t size;
        };
        std::vector<SyncEntry> syncHistory;
        size_t currentFrame = 0;

        void Initialize();
        void Shutdown();
        
        // Allocate from pool (legacy path)
        size_t Allocate(size_t size, void** outMappedPtr);
        void Free(size_t bufferIndex);
        
        // Ring buffer allocation for per-frame data (persistent mapped)
        size_t AllocateRing(size_t size, void** outMappedPtr);
        void ResetRing();
    };

    // ========================================================================
    // Multi-Draw Indirect Support
    // ========================================================================
    
    // Indirect draw command for indexed rendering
    struct DrawElementsIndirectCommand {
        GLuint count;           // Number of indices
        GLuint instanceCount;    // Number of instances
        GLuint firstIndex;       // Offset in index buffer
        GLint  baseVertex;       // Offset in vertex buffer
        GLuint baseInstance;     // Base instance
    };

    // Indirect draw command for non-indexed rendering
    struct DrawArraysIndirectCommand {
        GLuint count;           // Number of vertices
        GLuint instanceCount;    // Number of instances
        GLuint first;            // Start vertex
        GLuint baseInstance;     // Base instance
    };

    static constexpr size_t MAX_INDIRECT_COMMANDS = 4096;

    void InitializeMDI();
    void ExecuteMultiDraw();

    Renderer();
    ~Renderer();

    void Initialize();
    void Shutdown();

    // Add a renderable object to the batch
    void AddRenderable(GLuint VAO, GLuint VBO, GLuint EBO, GLsizei vertexCount,
                      GLenum primitiveType, GLuint shaderProgram,
                      const std::vector<glm::mat4>& transforms = {},
                      const glm::vec3& color = glm::vec3(1.0f),
                      float metallic = 0.0f,
                      float roughness = 0.5f,
                      const glm::vec3& bboxMin = glm::vec3(-1),
                      const glm::vec3& bboxMax = glm::vec3(1),
                      GLuint textureID = 0);  // albedo map (useAlbedoMap=1)

    // Submit all batches for rendering (with sorting)
    void SubmitBatches();

    // Render all submitted batches (sorted, optimized)
    void Render();

    // Clear all batches
    void ClearBatches();
    
    // Culling controls (wired from Editor UI / World Settings)
    void SetMaxVisibleInstances(int max) { m_maxVisibleInstances = max; }
    void SetCullingDebug(bool on) { m_cullingDebug = on; }
    bool IsCullingDebug() const { return m_cullingDebug; }
    
    // Culling statistics (updated each SubmitBatches call)
    size_t getVisibleInstances() const { return m_visibleInstances; }
    size_t getCulledInstances() const { return m_culledInstances; }

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
    
    // Set light parameters (updates UBO). The multi-light path takes an array
    // of render lights (editor entities -> LightSystem); the single-light
    // convenience keeps existing callers working.
    void SetLights(const RenderLight* lights, int count, const glm::vec3& viewPos);
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
    bool m_initialized = false;
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
    
    // Culling control + stats
    int m_maxVisibleInstances = 2000;   // 0 = unlimited
    bool m_cullingDebug = false;
    size_t m_visibleInstances = 0;
    size_t m_culledInstances = 0;
    
    // Frustum for culling
    Frustum frustum;
    
    // Buffer pool for instance data
    BufferPool bufferPool;
    
    // Cached uniform locations per shader program
    std::unordered_map<GLuint, std::unordered_map<std::string, int>> shaderUniformCache;
    
    // Pre-computed sort keys
    std::vector<size_t> batchIndices;

    // Multi-Draw Indirect state
    GLuint mdiIndirectBuffer = 0;           // SSBO for indirect commands
    GLuint mdiVertexArray = 0;              // Quad VAO for indirect draws
    void* mdiMappedCommands = nullptr;      // Persistent mapped pointer
    bool mdiSupported = false;              // Whether GL_ARB_multi_draw_indirect is available
    bool useMultiDraw = true;               // Toggle MDI (falls back to individual draws)

    void SetupBatch(RenderBatch& batch, const std::vector<glm::mat4>& transforms);
    void UpdateCameraUBO();
    void BindCameraUBO(GLuint shaderProgram);
    int GetCachedUniformLocation(GLuint shaderProgram, const std::string& name);

    // Uniform helpers for the batched draw path. The batched path must
    // replicate what Model::Draw does with its Shader wrapper (set the model
    // matrix + camera uniforms), or every batched mesh renders with the
    // shader's default zero matrices - degenerate geometry that reads as
    // scattered lines.
    void SetCommonShaderUniforms(GLuint shaderProgram);
    void SetModelUniform(GLuint shaderProgram, const glm::mat4& model);
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

inline int Renderer::GetCachedUniformLocation(GLuint shaderProgram, const std::string& name) {
    auto& cache = shaderUniformCache[shaderProgram];
    auto it = cache.find(name);
    if (it != cache.end()) {
        return it->second;
    }
    int loc = glGetUniformLocation(shaderProgram, name.c_str());
    cache[name] = loc;
    return loc;
}
