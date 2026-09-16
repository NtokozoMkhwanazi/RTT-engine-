#pragma once
/**
 * Bone Matrix Buffer - UBO/SSBO for Bone Matrices
 * 
 * Automatically selects optimal buffer type:
 * - UBO (Uniform Buffer Object): <= 120 bones, fast, widely supported
 * - SSBO (Shader Storage Buffer Object): > 120 bones, larger capacity, compute shaders
 * 
 * Replaces slow per-bone uniform uploads with fast buffer binding.
 * 
 * Performance comparison:
 *   Old (per-bone uniforms): 65 uniforms × ~1μs = ~65μs per frame
 *   UBO (<=120 bones): 1 bind call + 1 buffer update = ~5-10μs per frame (6-13x faster)
 *   SSBO (>120 bones): 1 bind call + 1 buffer update = ~5-15μs per frame (supports 1000+ bones)
 * 
 * Usage:
 *   BoneMatrixBuffer boneBuffer;
 *   boneBuffer.Initialize();  // Auto-selects UBO or SSBO
 *   
 *   // Each frame:
 *   boneBuffer.Update(animator.GetFinalBoneMatrices());
 *   boneBuffer.Bind(1);  // Bind to binding point 1
 */

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>

// Buffer type selection
enum class BoneBufferType {
    NONE = 0,       // Not initialized
    UBO = 1,        // Uniform Buffer Object (<=120 bones)
    SSBO = 2        // Shader Storage Buffer Object (>120 bones)
};

// Configuration
struct BoneBufferConfig {
    size_t uboMaxBones = 256;      // Switch to SSBO above this count
    bool preferSSBO = false;        // Force SSBO even for small skeletons (for crowds)
    bool usePersistentMapping = false;  // Use persistent mapping for even faster updates
};

class BoneMatrixBuffer {
public:
    BoneMatrixBuffer();
    ~BoneMatrixBuffer();

    // =========================================================================
    // INITIALIZATION
    // =========================================================================

    /**
     * Initialize buffer with specified capacity
     * Auto-selects UBO or SSBO based on bone count and config
     * 
     * @param maxBones Maximum number of bones to support (default 120)
     * @param config Configuration options
     * @return true if successful
     */
    bool Initialize(size_t maxBones = 120, const BoneBufferConfig& config = BoneBufferConfig());

    /**
     * Shutdown and cleanup
     */
    void Shutdown();

    // =========================================================================
    // UPDATE
    // =========================================================================

    /**
     * Update bone matrices in buffer
     * 
     * @param boneMatrices Vector of bone matrices (4x4 column-major)
     * @param forceUpdate Force update even if count hasn't changed
     * @return true if successful
     */
    bool Update(const std::vector<glm::mat4>& boneMatrices, bool forceUpdate = false);

    /**
     * Update with raw float data (faster, no glm conversion)
     * 
     * @param matrices Pointer to matrix data (16 floats per matrix)
     * @param count Number of matrices
     * @return true if successful
     */
    bool UpdateRaw(const float* matrices, size_t count);

    // =========================================================================
    // BONES PER-CHUNK SOA BATCH API (item 1 of C)
    // The per-animator bone matrices are already stored as a contiguous
    // std::vector<glm::mat4> slab (Animator::finalBoneMatrices -> GetFinalBoneMatrices(),
    // already a per-animator SoA slab over glm::mat4). The remaining indirection in
    // the skinning path is the per-entity glBufferSubData call in
    // ModelRenderSystem::renderAnimatedModel (one Update() per animator). This API
    // collapses N animators' slabs into ONE staging buffer + ONE buffer update per
    // frame. ComputeBatch is statically GL-free so it is unit-tested headless.
    // =========================================================================
    static std::vector<glm::mat4> ComputeBatch(
        const std::vector<const std::vector<glm::mat4>*>& batches);

    bool UpdateBatched(const std::vector<const std::vector<glm::mat4>*>& batches,
                       bool forceUpdate = false);

    // =========================================================================
    // BINDING    // =========================================================================

    /**
     * Bind buffer to specified binding point
     * 
     * @param bindingPoint Binding point (must match shader layout)
     */
    void Bind(GLuint bindingPoint = 0) const;

    /**
     * Unbind buffer
     */
    void Unbind() const;

    // =========================================================================
    // STATE QUERIES
    // =========================================================================

    /**
     * Check if buffer is initialized
     */
    bool IsInitialized() const { return initialized; }

    /**
     * Get buffer type (UBO or SSBO)
     */
    BoneBufferType GetBufferType() const { return bufferType; }

    /**
     * Get current bone count
     */
    size_t GetBoneCount() const { return currentBoneCount; }

    /**
     * Get maximum bone capacity
     */
    size_t GetMaxBones() const { return maxBoneCount; }

    /**
     * Check if last update was successful
     */
    bool NeedsUpdate() const { return needsUpdate; }

    /**
     * Check if SSBO is supported on this hardware
     */
    static bool IsSSBOSupported();
    
    // Pure upload decision: skip only when NOT forced, NOT dirty, and the
    // bone count is unchanged. This used to be inline in Update() and caused
    // the "skeleton animates but mesh frozen in Idle" bug - animated callers
    // must pass forceUpdate=true.
    static bool ShouldSkipUpload(bool forceUpdate, bool needsUpdate,
                                 size_t currentCount, size_t newCount) {
        return !forceUpdate && !needsUpdate && (currentCount == newCount);
    }

    // =========================================================================
    // STATISTICS
    // =========================================================================

    struct Stats {
        size_t updateCount{0};
        size_t bindCount{0};
        size_t bytesUploaded{0};
        double avgUpdateTimeMs{0.0};
        double lastUpdateTimeMs{0.0};
    };

    Stats GetStats() const { return stats; }
    void PrintStats() const;
    void ResetStats();

    /**
     * Get buffer type as string
     */
    std::string GetBufferTypeString() const;

private:
    GLuint buffer{0};                 // UBO or SSBO object
    BoneBufferType bufferType{BoneBufferType::NONE};  // Type of buffer
    size_t maxBoneCount{0};           // Maximum bone capacity
    size_t currentBoneCount{0};       // Current number of bones
    bool initialized{false};          // Is buffer initialized
    bool needsUpdate{true};           // Does buffer need update
    BoneBufferConfig config;          // Configuration

    // Statistics
    mutable Stats stats;

    // Timing
    double lastFrameTime{0.0};
    int frameCount{0};

    // Internal methods
    void AllocateBuffer();
    void UploadData(const float* data, size_t size);
    void SelectBufferType(size_t requestedBones);
};

// ============================================================================
// SHADER LAYOUT (GLSL)
// ============================================================================
// 
// For UBO (<=120 bones):
// layout(std140, binding = 0) uniform BoneMatrices {
//     mat4 boneMatrices[120];
// } boneBlock;
// 
// For SSBO (>120 bones):
// layout(std430, binding = 0) buffer BoneMatrices {
//     mat4 boneMatrices[];  // Unsized array for SSBO
// } boneBlock;
// 
// // In skinning calculation (works for both):
// mat4 boneMatrix = boneMatrices[boneID];
// 
// ============================================================================
