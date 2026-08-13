/**
 * Optimized Renderer Implementation
 * 
 * Optimizations:
 * - UBO for camera matrices (single update per frame)
 * - Batch sorting by shader/VAO
 * - Frustum culling
 * - Buffer pooling (reduced glGen/glDelete)
 */

#include "Renderer.h"
#include "../editor/gl_context_lifecycle.h"
#include <iostream>
#include <cstring>

// OpenGL 4.3+ constants not available in OpenGL 3.3 headers
#ifndef GL_DRAW_INDIRECT_BUFFER
#define GL_DRAW_INDIRECT_BUFFER 0x8F3F
#endif
#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT 0x0080
#endif

// Function pointers for OpenGL 4.3+ features
typedef void (APIENTRY *PFNGLBUFFERSTORAGEPROC)(GLenum target, GLsizeiptr size, const void *data, GLbitfield flags);
typedef void (APIENTRY *PFNGLMULTIDRAWELEMENTSINDIRECTPROC)(GLenum mode, GLenum type, const void *indirect, GLsizei drawcount, GLsizei stride);
static PFNGLBUFFERSTORAGEPROC glBufferStoragePtr = nullptr;
static PFNGLMULTIDRAWELEMENTSINDIRECTPROC glMultiDrawElementsIndirectPtr = nullptr;

// ============================================================================
// Frustum Implementation
// ============================================================================

void Frustum::Update(const glm::mat4& vp) {
    // Extract frustum planes from view-projection matrix
    planes[LEFT].x = vp[0].w + vp[0].x;
    planes[LEFT].y = vp[1].w + vp[1].x;
    planes[LEFT].z = vp[2].w + vp[2].x;
    planes[LEFT].w = vp[3].w + vp[3].x;
    
    planes[RIGHT].x = vp[0].w - vp[0].x;
    planes[RIGHT].y = vp[1].w - vp[1].x;
    planes[RIGHT].z = vp[2].w - vp[2].x;
    planes[RIGHT].w = vp[3].w - vp[3].x;
    
    planes[BOTTOM].x = vp[0].w + vp[0].y;
    planes[BOTTOM].y = vp[1].w + vp[1].y;
    planes[BOTTOM].z = vp[2].w + vp[2].y;
    planes[BOTTOM].w = vp[3].w + vp[3].y;
    
    planes[TOP].x = vp[0].w - vp[0].y;
    planes[TOP].y = vp[1].w - vp[1].y;
    planes[TOP].z = vp[2].w - vp[2].y;
    planes[TOP].w = vp[3].w - vp[3].y;
    
    planes[NEAR].x = vp[0].w + vp[0].z;
    planes[NEAR].y = vp[1].w + vp[1].z;
    planes[NEAR].z = vp[2].w + vp[2].z;
    planes[NEAR].w = vp[3].w + vp[3].z;
    
    planes[FAR].x = vp[0].w - vp[0].z;
    planes[FAR].y = vp[1].w - vp[1].z;
    planes[FAR].z = vp[2].w - vp[2].z;
    planes[FAR].w = vp[3].w - vp[3].z;
    
    // Normalize planes
    for (auto& plane : planes) {
        float len = glm::length(glm::vec3(plane.x, plane.y, plane.z));
        if (len > 0.0001f) {
            plane /= len;
        }
    }
}

bool Frustum::TestPoint(const glm::vec3& p) const {
    for (const auto& plane : planes) {
        if (glm::dot(plane, glm::vec4(p, 1.0f)) < 0.0f) {
            return false;
        }
    }
    return true;
}

bool Frustum::TestSphere(const glm::vec3& center, float radius) const {
    for (const auto& plane : planes) {
        float dist = glm::dot(plane, glm::vec4(center, 1.0f));
        if (dist < -radius) {
            return false;
        }
    }
    return true;
}

bool Frustum::TestAABB(const glm::vec3& min, const glm::vec3& max) const {
    for (const auto& plane : planes) {
        glm::vec3 positiveVertex;
        positiveVertex.x = (plane.x >= 0) ? max.x : min.x;
        positiveVertex.y = (plane.y >= 0) ? max.y : min.y;
        positiveVertex.z = (plane.z >= 0) ? max.z : min.z;
        
        if (glm::dot(plane, glm::vec4(positiveVertex, 1.0f)) < 0.0f) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// Buffer Pool Implementation (Persistent Mapped Ring Buffer)
// ============================================================================

void Renderer::BufferPool::Initialize() {
    // Initialize legacy buffer slots
    numBuffers = 0;
    for (size_t i = 0; i < MAX_BUFFERS; i++) {
        inUse[i] = false;
        bufferSizes[i] = 0;
        mappedPointers[i] = nullptr;
    }

    // Check for OpenGL 4.4+ or GL_ARB_buffer_storage
    ringBufferSupported = false;

    const char* versionStr = (const char*)glGetString(GL_VERSION);
    if (versionStr && std::strstr(versionStr, "4.4") != nullptr) {
        ringBufferSupported = true;
    }
    const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
    if (extensions && std::strstr(extensions, "GL_ARB_buffer_storage") != nullptr) {
        ringBufferSupported = true;
    }

    if (!ringBufferSupported) {
        std::cout << "[Renderer] Persistent buffer storage not supported (requires OpenGL 4.4+ or GL_ARB_buffer_storage)\n";
        std::cout << "[Renderer] Falling back to glBufferSubData\n";
        return;
    }

    // Create persistent mapped ring buffer
    glGenBuffers(1, &ringBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, ringBuffer);

    // Use glBufferStorage with persistent mapping flags
    // GL_MAP_PERSISTENT_BIT: mapping persists after unmap
    // GL_MAP_COHERENT_BIT: GPU sees CPU writes immediately (no flush needed)
    glBufferStoragePtr(GL_ARRAY_BUFFER, (GLsizeiptr)POOL_SIZE, nullptr,
                       GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);

    // Map once at init time
    ringMappedPtr = glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)POOL_SIZE,
                                      GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);

    if (!ringMappedPtr) {
        std::cout << "[Renderer] Failed to map persistent ring buffer\n";
        ringBufferSupported = false;
        glDeleteBuffers(1, &ringBuffer);
        return;
    }

    ringBufferSize = POOL_SIZE;
    ringOffset = 0;

    syncHistory.reserve(MAX_FRAMES_IN_FLIGHT * 256);

    std::cout << "[Renderer] Persistent mapped ring buffer initialized (64MB)\n";
}

void Renderer::BufferPool::Shutdown() {
    // Clean up legacy buffers
    for (size_t i = 0; i < numBuffers; i++) {
        if (bufferIDs[i]) {
            glDeleteBuffers(1, &bufferIDs[i]);
        }
    }

    // Clean up ring buffer
    if (ringBuffer) {
        if (ringMappedPtr) {
            glUnmapBuffer(GL_ARRAY_BUFFER);
            ringMappedPtr = nullptr;
        }
        glDeleteBuffers(1, &ringBuffer);
        ringBuffer = 0;
    }

    // Clean up sync objects
    for (auto& sync : syncHistory) {
        if (sync.fence) {
            glDeleteSync((GLsync)sync.fence);
        }
    }
    syncHistory.clear();
}

size_t Renderer::BufferPool::Allocate(size_t size, void** outMappedPtr) {
    // Find a free buffer slot or create new one
    for (size_t i = 0; i < numBuffers; i++) {
        if (!inUse[i] && bufferSizes[i] >= size) {
            inUse[i] = true;
            *outMappedPtr = mappedPointers[i];
            return i;
        }
    }
    
    // Need to create a new buffer
    if (numBuffers >= MAX_BUFFERS) {
        std::cerr << "[Renderer] Buffer pool exhausted!\n";
        return 0;
    }
    
    size_t idx = numBuffers++;
    glGenBuffers(1, &bufferIDs[idx]);
    glBindBuffer(GL_ARRAY_BUFFER, bufferIDs[idx]);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)size, nullptr, GL_DYNAMIC_DRAW);

    // Try to persistently map this buffer too
    if (ringBufferSupported && size < POOL_SIZE / 4) {
        glBufferStoragePtr(GL_ARRAY_BUFFER, (GLsizeiptr)size, nullptr,
                           GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
        mappedPointers[idx] = glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)size,
                                                GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    } else {
        mappedPointers[idx] = nullptr;
    }

    bufferSizes[idx] = size;
    inUse[idx] = true;
    
    *outMappedPtr = mappedPointers[idx];
    return idx;
}

void Renderer::BufferPool::Free(size_t bufferIndex) {
    if (bufferIndex < numBuffers) {
        inUse[bufferIndex] = false;
    }
}

/**
 * Allocate from persistent ring buffer.
 * Returns offset into the ring buffer and a pointer to the mapped region.
 * The caller can memcpy directly into the pointer - GPU sees changes immediately
 * due to GL_MAP_COHERENT_BIT.
 *
 * Handles ring wrap-around and GPU sync to avoid overwriting data still in use.
 */
size_t Renderer::BufferPool::AllocateRing(size_t size, void** outMappedPtr) {
    if (!ringBufferSupported) {
        // Fallback: use legacy allocation
        return Allocate(size, outMappedPtr);
    }

    // Align to 16 bytes for SIMD compatibility
    size = (size + 15) & ~15;

    // Check if we need to wrap around
    if (ringOffset + size > ringBufferSize) {
        // Insert GPU sync fence for current ring segment before wrapping
        GLuint64 fence = (GLuint64)glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        syncHistory.push_back({fence, ringOffset, ringBufferSize - ringOffset});

        ringOffset = 0;
    }

    // Wait on any sync objects that overlap with our new allocation
    // (simple approach: wait on any pending fence - in production, track per-frame)
    if (!syncHistory.empty()) {
        // Only wait on the oldest fence that's been there long enough
        auto& oldest = syncHistory.front();
        if (oldest.fence) {
            GLenum result = glClientWaitSync((GLsync)oldest.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000); // 1ms timeout
            if (result == GL_CONDITION_SATISFIED || result == GL_ALREADY_SIGNALED) {
                glDeleteSync((GLsync)oldest.fence);
                oldest.fence = 0;
            }
        }
        // Prune completed sync entries
        syncHistory.erase(
            std::remove_if(syncHistory.begin(), syncHistory.end(),
                [](const SyncEntry& e) { return e.fence == 0; }),
            syncHistory.end()
        );
    }

    // Safety check: if ring is too full, flush and wait
    if (ringOffset + size > ringBufferSize) {
        glFinish(); // Force GPU to finish all pending work
        ringOffset = 0;

        // Clear all remaining sync objects
        for (auto& sync : syncHistory) {
            if (sync.fence) {
                glDeleteSync((GLsync)sync.fence);
                sync.fence = 0;
            }
        }
        syncHistory.clear();
    }

    size_t offset = ringOffset;
    *outMappedPtr = static_cast<char*>(ringMappedPtr) + offset;
    ringOffset += size;

    return offset;
}

void Renderer::BufferPool::ResetRing() {
    // Reset ring buffer at start of each frame
    // (only needed if not using persistent coherent mapping)
    if (ringBufferSupported) {
        // Insert fence at current position for next frame's sync
        if (ringOffset > 0) {
            GLuint64 fence = (GLuint64)glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            syncHistory.push_back({fence, 0, ringOffset});
        }
        ringOffset = 0;
        currentFrame++;
    }
}
    
// ============================================================================
// Renderer Implementation
// ============================================================================

Renderer::Renderer() {
    // Default initialization
}

Renderer::~Renderer() {
    Shutdown();
}

void Renderer::Initialize() {
    if (m_initialized) return;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    
    // Initialize UBO
    glGenBuffers(1, &cameraUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, cameraUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(CameraUBO), nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, CAMERA_UBO_BINDING, cameraUBO);
    
    // Initialize buffer pool
    bufferPool.Initialize();
    
    // Pre-allocate batch indices vector
    batchIndices.reserve(256);
    
    // Initialize Multi-Draw Indirect
    InitializeMDI();
    
    m_initialized = true;
    std::cout << "[Renderer] Initialized with UBO and batch sorting\n";
}

void Renderer::Shutdown() {
    if (!m_initialized) return;

    // Defense in depth: even if m_initialized is somehow still true at static
    // destruction time (e.g., Init was called but explicit Shutdown was missed),
    // do not issue GL calls on a dead context.
    if (!glctx::isAlive()) {
        // Still flip m_initialized so ~Renderer() doesn't re-enter.
        m_initialized = false;
        return;
    }

    bufferPool.Shutdown();
    
    if (cameraUBO) {
        glDeleteBuffers(1, &cameraUBO);
        cameraUBO = 0;
    }
    
    if (mdiIndirectBuffer) {
        if (mdiMappedCommands) {
            glUnmapBuffer(GL_DRAW_INDIRECT_BUFFER);
            mdiMappedCommands = nullptr;
        }
        glDeleteBuffers(1, &mdiIndirectBuffer);
        mdiIndirectBuffer = 0;
    }
    
    if (mdiVertexArray) {
        glDeleteVertexArrays(1, &mdiVertexArray);
        mdiVertexArray = 0;
    }
    
    if (defaultTexture) {
        glDeleteTextures(1, &defaultTexture);
        defaultTexture = 0;
    }

    batches.clear();
    batchIndices.clear();
    m_initialized = false;
}

void Renderer::SetCameraMatrices(const glm::mat4& view, const glm::mat4& projection) {
    cameraData.view = view;
    cameraData.projection = projection;
    cameraData.viewProjection = projection * view;
    
    // Update frustum
    if (frustumCullingEnabled) {
        frustum.Update(cameraData.viewProjection);
    }
    
    UpdateCameraUBO();
}

void Renderer::SetLightParameters(const glm::vec3& lightPos, const glm::vec3& viewPos) {
    cameraData.lightPos = glm::vec4(lightPos, 1.0f);
    cameraData.viewPos = glm::vec4(viewPos, 1.0f);
    UpdateCameraUBO();
}

void Renderer::UpdateCameraUBO() {
    // Update UBO once per frame via glBufferSubData
    // (UBOs are small enough that this is fast; the ring buffer is used for larger per-frame data)
    glBindBuffer(GL_UNIFORM_BUFFER, cameraUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(CameraUBO), &cameraData);
}

void Renderer::AddRenderable(GLuint VAO, GLuint VBO, GLuint EBO, GLsizei vertexCount,
                            GLenum primitiveType, GLuint shaderProgram,
                            const std::vector<glm::mat4>& transforms,
                            const glm::vec3& color,
                            float metallic,
                            float roughness,
                            const glm::vec3& bboxMin,
                            const glm::vec3& bboxMax,
                            GLuint textureID) {
    // Frustum culling - skip if not visible
    if (frustumCullingEnabled && !transforms.empty()) {
        glm::vec3 center = (bboxMin + bboxMax) * 0.5f;
        glm::vec3 extent = (bboxMax - bboxMin) * 0.5f;
        float maxExtent = glm::length(extent);

        glm::vec4 transformedCenter = transforms[0] * glm::vec4(center, 1.0f);

        if (!frustum.TestSphere(glm::vec3(transformedCenter), maxExtent * 2.0f)) {
            return;  // Culled
        }
    }

    RenderBatch batch;
    batch.vertexArrayObject = VAO;
    batch.vertexBuffer = VBO;
    batch.elementBuffer = EBO;
    batch.vertexCount = vertexCount;
    batch.primitiveType = primitiveType;
    batch.shaderProgram = shaderProgram;
    batch.instanceCount = static_cast<GLsizei>(transforms.size());
    batch.textureID = textureID;
    batch.albedo = color;
    batch.metallic = metallic;
    batch.roughness = roughness;
    batch.bboxMin = bboxMin;
    batch.bboxMax = bboxMax;
    // The batched path draws each batch with a uniform model matrix (the
    // instanced attribute path can't carry per-batch transforms when several
    // batches share a VAO).
    if (!transforms.empty()) {
        batch.modelMatrix = transforms[0];
    }

    // Compute sort key for efficient sorting
    batch.ComputeSortKey();

    // Setup instance data
    if (!transforms.empty()) {
        SetupBatch(batch, transforms);
    }

    batches.push_back(batch);
}

void Renderer::SubmitBatches() {
    // Sort batches by shader program, then texture, then VAO
    if (batches.size() > 1) {
        batchIndices.resize(batches.size());
        for (size_t i = 0; i < batches.size(); i++) {
            batchIndices[i] = i;
        }
        
        // Sort indices by sort key
        std::sort(batchIndices.begin(), batchIndices.end(),
            [this](size_t a, size_t b) {
                return batches[a].sortKey < batches[b].sortKey;
            });
    }
    
    // Reset ring buffer for new frame (syncs GPU fences, wraps ring)
    bufferPool.ResetRing();
    
    Render();
}

void Renderer::Render() {
    // Apply render state
    if (depthTestingEnabled) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);

    if (faceCullingEnabled) glEnable(GL_CULL_FACE);
    else glDisable(GL_CULL_FACE);

    glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
    
    // Bind camera UBO once - all shaders will use it
    glBindBufferBase(GL_UNIFORM_BUFFER, CAMERA_UBO_BINDING, cameraUBO);
    
    // Bind default texture to texture unit 0
    if (defaultTexture != 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, defaultTexture);
    }

    if (useMultiDraw && !batches.empty()) {
        // === MDI PATH: Group batches and issue multi-draw calls ===
        ExecuteMultiDraw();
    } else {
        // === FALLBACK PATH: Individual draw calls per batch ===
        GLuint lastShader = 0;
        GLuint lastVAO = 0;
        
        const std::vector<size_t>* renderOrder = &batchIndices;
        std::vector<size_t> fallbackOrder;
        
        if (batchIndices.empty() && !batches.empty()) {
            fallbackOrder.resize(batches.size());
            for (size_t i = 0; i < batches.size(); i++) fallbackOrder[i] = i;
            renderOrder = &fallbackOrder;
        }
        
        for (size_t idx : *renderOrder) {
            const auto& batch = batches[idx];
            
            if (batch.vertexArrayObject == 0) continue;
            
            // Minimize state changes
            if (batch.shaderProgram != lastShader) {
                glUseProgram(batch.shaderProgram);
                lastShader = batch.shaderProgram;
                BindCameraUBO(batch.shaderProgram);
                SetCommonShaderUniforms(batch.shaderProgram);
            }
            
            // Feed this batch's transform through the model uniform (same
            // contract as the playable character's Model::Draw path).
            SetModelUniform(batch.shaderProgram, batch.modelMatrix);
            
            // Set material uniforms using explicit layout locations from the shader
            glUniform3f(0, batch.albedo.r, batch.albedo.g, batch.albedo.b);
            glUniform1f(1, batch.metallic);
            glUniform1f(2, batch.roughness);
            glUniform1f(3, batch.ao);
            glUniform3f(4, batch.emissive.r, batch.emissive.g, batch.emissive.b);

            GLint useAlbedoMapLoc = GetCachedUniformLocation(batch.shaderProgram, "useAlbedoMap");

            if (useAlbedoMapLoc != -1) {
                bool hasTexture = (batch.textureID != 0);
                glUniform1i(useAlbedoMapLoc, hasTexture ? 1 : 0);

                if (hasTexture) {
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, batch.textureID);
                }
            }
            
            if (batch.vertexArrayObject != lastVAO) {
                glBindVertexArray(batch.vertexArrayObject);
                lastVAO = batch.vertexArrayObject;
            }

            bool useIndexed = (batch.elementBuffer != 0);
            if (useIndexed) {
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, batch.elementBuffer);
            }

            if (batch.instanceCount > 0) {
                if (useIndexed) {
                    glDrawElementsInstanced(batch.primitiveType, batch.vertexCount, GL_UNSIGNED_INT, 0, batch.instanceCount);
                } else {
                    glDrawArraysInstanced(batch.primitiveType, 0, batch.vertexCount, batch.instanceCount);
                }
            } else {
                if (useIndexed) {
                    glDrawElements(batch.primitiveType, batch.vertexCount, GL_UNSIGNED_INT, 0);
                } else {
                    glDrawArrays(batch.primitiveType, 0, batch.vertexCount);
                }
            }
        }
    }

    // Clear batches after rendering
    batches.clear();
    batchIndices.clear();
}

void Renderer::ClearBatches() {
    batches.clear();
    batchIndices.clear();
}

void Renderer::SetViewport(int x, int y, int width, int height) {
    viewportX = x;
    viewportY = y;
    viewportWidth = width;
    viewportHeight = height;
    glViewport(x, y, width, height);
}

void Renderer::SetClearColor(float r, float g, float b, float a) {
    clearColor = glm::vec4(r, g, b, a);
}

void Renderer::SetDepthTesting(bool enabled) {
    depthTestingEnabled = enabled;
}

void Renderer::SetFaceCulling(bool enabled) {
    faceCullingEnabled = enabled;
}

void Renderer::SetupBatch(RenderBatch& batch, const std::vector<glm::mat4>& transforms) {
    if (transforms.empty()) return;
    
    size_t dataSize = transforms.size() * sizeof(glm::mat4);

    if (bufferPool.ringBufferSupported) {
        // === PERSISTENT RING BUFFER PATH (OpenGL 4.4+) ===
        // Upload instance data directly into the persistent ring buffer
        void* mappedPtr = nullptr;
        bufferPool.AllocateRing(dataSize, &mappedPtr);
        
        // Copy transform data into the mapped ring buffer region
        std::memcpy(mappedPtr, transforms.data(), dataSize);
        
        // Use the shared ring buffer as instance data source
        GLuint instanceBuffer = bufferPool.ringBuffer;
        batch.instanceBuffer = instanceBuffer;
        batch.instanceDataOffset = reinterpret_cast<uintptr_t>(mappedPtr) - reinterpret_cast<uintptr_t>(bufferPool.ringMappedPtr);
        
        // Setup vertex attribute pointers for instance data (locations 7-10)
        // Bind the ring buffer and set the offset for each matrix column
        glBindVertexArray(batch.vertexArrayObject);
        glBindBuffer(GL_ARRAY_BUFFER, instanceBuffer);
        
        for (int i = 0; i < 4; i++) {
            glEnableVertexAttribArray(7 + i);
            glVertexAttribPointer(7 + i, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4),
                                 (void*)(batch.instanceDataOffset + sizeof(float) * 4 * i));
            glVertexAttribDivisor(7 + i, 1);
        }
        
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else {
        // === FALLBACK: Legacy per-batch buffer ===
        GLuint instanceBuffer;
        glGenBuffers(1, &instanceBuffer);
        glBindBuffer(GL_ARRAY_BUFFER, instanceBuffer);
        glBufferData(GL_ARRAY_BUFFER, transforms.size() * sizeof(glm::mat4),
                     transforms.data(), GL_DYNAMIC_DRAW);
        
        // Setup vertex attribute pointers for instance data (locations 7-10)
        glBindVertexArray(batch.vertexArrayObject);
        
        for (int i = 0; i < 4; i++) {
            glEnableVertexAttribArray(7 + i);
            glVertexAttribPointer(7 + i, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4),
                                 (void*)(sizeof(float) * 4 * i));
            glVertexAttribDivisor(7 + i, 1);
        }
        
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        
        batch.instanceBuffer = instanceBuffer;
        batch.instanceDataOffset = 0;
    }
}

// ============================================================================
// Multi-Draw Indirect Implementation
// ============================================================================

void Renderer::InitializeMDI() {
    // Check for GL_ARB_multi_draw_indirect or OpenGL 4.3+
    mdiSupported = false;

    const char* versionStr = (const char*)glGetString(GL_VERSION);
    const char* extensions = (const char*)glGetString(GL_EXTENSIONS);

    if (versionStr && std::strstr(versionStr, "4.") != nullptr) {
        mdiSupported = true;
    }

    if (extensions && std::strstr(extensions, "GL_ARB_multi_draw_indirect") != nullptr) {
        mdiSupported = true;
    }

    if (!mdiSupported) {
        std::cout << "[Renderer] Multi-Draw Indirect not supported (requires OpenGL 4.3+ or GL_ARB_multi_draw_indirect)\n";
        std::cout << "[Renderer] Falling back to glMultiDrawElements (OpenGL 1.4+)\n";
        return;
    }

    // Load OpenGL 4.3+ function pointers
    glBufferStoragePtr = (PFNGLBUFFERSTORAGEPROC)glfwGetProcAddress("glBufferStorage");
    glMultiDrawElementsIndirectPtr = (PFNGLMULTIDRAWELEMENTSINDIRECTPROC)glfwGetProcAddress("glMultiDrawElementsIndirect");

    if (!glBufferStoragePtr || !glMultiDrawElementsIndirectPtr) {
        std::cout << "[Renderer] MDI functions not available via glfwGetProcAddress\n";
        std::cout << "[Renderer] Falling back to glMultiDrawElements (OpenGL 1.4+)\n";
        mdiSupported = false;
        return;
    }

    // Create indirect command buffer with persistent mapping
    glGenBuffers(1, &mdiIndirectBuffer);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, mdiIndirectBuffer);
    
    // Allocate storage for indirect commands
    size_t bufferSize = MAX_INDIRECT_COMMANDS * sizeof(DrawElementsIndirectCommand);
    glBufferStoragePtr(GL_DRAW_INDIRECT_BUFFER, (GLsizeiptr)bufferSize, nullptr,
                       GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    
    // Map the buffer for persistent access
    mdiMappedCommands = glMapBufferRange(GL_DRAW_INDIRECT_BUFFER, 0, (GLsizeiptr)bufferSize,
                                          GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    
    if (!mdiMappedCommands) {
        std::cout << "[Renderer] Failed to map indirect command buffer\n";
        mdiSupported = false;
        glDeleteBuffers(1, &mdiIndirectBuffer);
        return;
    }

    // Create a dummy VAO for indirect draws
    glGenVertexArrays(1, &mdiVertexArray);

    std::cout << "[Renderer] Multi-Draw Indirect initialized (persistent mapped buffer, "
              << MAX_INDIRECT_COMMANDS << " commands)\n";
}

void Renderer::ExecuteMultiDraw() {
    if (batches.empty()) return;

    const std::vector<size_t>* renderOrder = &batchIndices;
    std::vector<size_t> fallbackOrder;
    
    if (batchIndices.empty()) {
        fallbackOrder.resize(batches.size());
        for (size_t i = 0; i < batches.size(); i++) fallbackOrder[i] = i;
        renderOrder = &fallbackOrder;
    }

    // Group batches by shader+VAO+texture for MDI (one multi-draw call per
    // group). The texture is part of the key because a multi-draw group shares
    // a single texture bind - meshes with different albedo maps must not be
    // batched together.
    struct MDIGroup {
        GLuint shaderProgram;
        GLuint VAO;
        GLuint EBO;
        GLuint textureID;
        GLsizei vertexCount;      // Same for all in group (same mesh)
        GLenum primitiveType;
        GLsizei instanceCount;    // Max instance count in group
        std::vector<size_t> batchIndices;
    };

    // Build groups
    std::vector<MDIGroup> groups;
    groups.reserve(renderOrder->size());

    for (size_t idx : *renderOrder) {
        const auto& batch = batches[idx];
        if (batch.vertexArrayObject == 0) continue;

        bool useIndexed = (batch.elementBuffer != 0);
        if (!useIndexed) continue;  // MDI path only handles indexed draws for now

        // Find or create group for this shader+VAO+texture combination
        bool found = false;
        for (auto& group : groups) {
            if (group.shaderProgram == batch.shaderProgram &&
                group.VAO == batch.vertexArrayObject &&
                group.EBO == batch.elementBuffer &&
                group.textureID == batch.textureID) {
                group.batchIndices.push_back(idx);
                group.instanceCount = std::max(group.instanceCount, batch.instanceCount);
                found = true;
                break;
            }
        }

        if (!found) {
            MDIGroup newGroup;
            newGroup.shaderProgram = batch.shaderProgram;
            newGroup.VAO = batch.vertexArrayObject;
            newGroup.EBO = batch.elementBuffer;
            newGroup.textureID = batch.textureID;
            newGroup.vertexCount = batch.vertexCount;
            newGroup.primitiveType = batch.primitiveType;
            newGroup.instanceCount = std::max((GLsizei)1, batch.instanceCount);
            newGroup.batchIndices.push_back(idx);
            groups.push_back(std::move(newGroup));
        }
    }

    // Execute each group as a multi-draw call
    for (const auto& group : groups) {
        if (group.batchIndices.empty()) continue;

        // Set shader and material state once per group
        glUseProgram(group.shaderProgram);
        BindCameraUBO(group.shaderProgram);
        SetCommonShaderUniforms(group.shaderProgram);
        glBindVertexArray(group.VAO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, group.EBO);

        // Material uniforms from the first batch (shared shader state)
        const auto& firstBatch = batches[group.batchIndices[0]];
        glUniform3f(0, firstBatch.albedo.r, firstBatch.albedo.g, firstBatch.albedo.b);
        glUniform1f(1, firstBatch.metallic);
        glUniform1f(2, firstBatch.roughness);
        glUniform1f(3, firstBatch.ao);
        glUniform3f(4, firstBatch.emissive.r, firstBatch.emissive.g, firstBatch.emissive.b);

        GLint useAlbedoMapLoc = GetCachedUniformLocation(group.shaderProgram, "useAlbedoMap");
        if (useAlbedoMapLoc != -1) {
            bool hasTexture = (firstBatch.textureID != 0);
            glUniform1i(useAlbedoMapLoc, hasTexture ? 1 : 0);
            if (hasTexture) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, firstBatch.textureID);
            }
        }

        // A multi-draw group shares ONE uniform state, so it can only batch
        // together batches that use the SAME model matrix. Batches that share
        // a VAO but carry different transforms (e.g. several entities using
        // the same model, or the 6 physics crates that share one cube VAO)
        // must be drawn one at a time with their own uModel - otherwise the
        // per-VAO instanced-attribute setup leaves only the LAST batch's
        // transform for every command in the group, and the rest render with
        // garbage matrices (the "models look like broken lines" bug).
        bool sameTransform = true;
        for (size_t i = 1; i < group.batchIndices.size(); ++i) {
            if (batches[group.batchIndices[i]].modelMatrix != firstBatch.modelMatrix) {
                sameTransform = false;
                break;
            }
        }

        if (!sameTransform) {
            // Per-batch draw: each batch gets its own model + material uniforms.
            for (size_t idx : group.batchIndices) {
                const auto& batch = batches[idx];
                glUniform3f(0, batch.albedo.r, batch.albedo.g, batch.albedo.b);
                glUniform1f(1, batch.metallic);
                glUniform1f(2, batch.roughness);
                glUniform1f(3, batch.ao);
                glUniform3f(4, batch.emissive.r, batch.emissive.g, batch.emissive.b);
                SetModelUniform(group.shaderProgram, batch.modelMatrix);
                if (batch.instanceCount > 0) {
                    glDrawElementsInstanced(group.primitiveType, batch.vertexCount,
                                            GL_UNSIGNED_INT, 0, batch.instanceCount);
                } else {
                    glDrawElements(group.primitiveType, batch.vertexCount, GL_UNSIGNED_INT, 0);
                }
            }
            continue;
        }

        // All batches in the group share the same transform: one model uniform
        // and a single multi-draw call for all of them.
        SetModelUniform(group.shaderProgram, firstBatch.modelMatrix);

        if (mdiSupported && useMultiDraw) {
            // === PATH 1: Full Multi-Draw Indirect (OpenGL 4.3+) ===
            
            // Build indirect commands in persistent mapped buffer
            auto* commands = static_cast<DrawElementsIndirectCommand*>(mdiMappedCommands);
            size_t cmdCount = group.batchIndices.size();

            if (cmdCount > MAX_INDIRECT_COMMANDS) {
                cmdCount = MAX_INDIRECT_COMMANDS;  // Clamp to buffer size
            }

            for (size_t i = 0; i < cmdCount; ++i) {
                size_t batchIdx = group.batchIndices[i];
                const auto& batch = batches[batchIdx];
                
                commands[i].count = static_cast<GLuint>(batch.vertexCount);
                commands[i].instanceCount = std::max((GLuint)batch.instanceCount, 1u);
                commands[i].firstIndex = 0;  // Assumes EBO starts at 0
                commands[i].baseVertex = 0;
                commands[i].baseInstance = 0;
            }

            // Single multi-draw call for all batches in this group
            if (glMultiDrawElementsIndirectPtr) {
                glMultiDrawElementsIndirectPtr(group.primitiveType, GL_UNSIGNED_INT,
                                               nullptr, static_cast<GLsizei>(cmdCount), 0);
            }
        } else {
            // === PATH 2: glMultiDrawElements fallback (OpenGL 1.4+) ===
            
            size_t cmdCount = group.batchIndices.size();
            std::vector<GLsizei> counts(cmdCount);
            std::vector<const void*> offsets(cmdCount);

            for (size_t i = 0; i < cmdCount; ++i) {
                counts[i] = static_cast<GLsizei>(batches[group.batchIndices[i]].vertexCount);
                offsets[i] = nullptr;  // All use offset 0
            }

            glMultiDrawElements(group.primitiveType, counts.data(), GL_UNSIGNED_INT,
                                offsets.data(), static_cast<GLsizei>(cmdCount));
        }
    }
}

// ============================================================================
// Uniform Helpers for the Batched Draw Path
// ============================================================================

void Renderer::SetCommonShaderUniforms(GLuint shaderProgram) {
    // Shaders that use plain camera uniforms (VS.glsl style) get them from
    // here; the editor's main shader reads the CameraBlock UBO instead, so
    // these lookups simply return -1 for it (harmless).
    GLint viewLoc = GetCachedUniformLocation(shaderProgram, "view");
    if (viewLoc >= 0) glUniformMatrix4fv(viewLoc, 1, GL_FALSE, &cameraData.view[0][0]);
    GLint projLoc = GetCachedUniformLocation(shaderProgram, "projection");
    if (projLoc >= 0) glUniformMatrix4fv(projLoc, 1, GL_FALSE, &cameraData.projection[0][0]);

    // The batched path feeds transforms through uniforms, not the instanced
    // attribute - the mesh VAOs only define attributes 0-6.
    GLint instLoc = GetCachedUniformLocation(shaderProgram, "uInstanced");
    if (instLoc >= 0) glUniform1i(instLoc, 0);
}

void Renderer::SetModelUniform(GLuint shaderProgram, const glm::mat4& model) {
    GLint modelLoc = GetCachedUniformLocation(shaderProgram, "uModel");
    if (modelLoc >= 0) glUniformMatrix4fv(modelLoc, 1, GL_FALSE, &model[0][0]);

    // Cover shaders that still name it "model" (VS.glsl style).
    GLint legacyLoc = GetCachedUniformLocation(shaderProgram, "model");
    if (legacyLoc >= 0 && legacyLoc != modelLoc) {
        glUniformMatrix4fv(legacyLoc, 1, GL_FALSE, &model[0][0]);
    }
}
