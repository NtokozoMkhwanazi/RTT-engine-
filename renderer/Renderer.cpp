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
#include <iostream>
#include <cstring>

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
// Buffer Pool Implementation (Simplified - no persistent mapping)
// ============================================================================

void Renderer::BufferPool::Initialize() {
    // Pre-allocate some buffers for reuse
    numBuffers = 0;
    for (size_t i = 0; i < MAX_BUFFERS; i++) {
        inUse[i] = false;
        bufferSizes[i] = 0;
    }
    
    std::cout << "[Renderer] Buffer pool initialized\n";
}

void Renderer::BufferPool::Shutdown() {
    for (size_t i = 0; i < numBuffers; i++) {
        if (bufferIDs[i]) {
            glDeleteBuffers(1, &bufferIDs[i]);
        }
    }
}

size_t Renderer::BufferPool::Allocate(size_t size, void** outMappedPtr) {
    // Find a free buffer slot or create new one
    for (size_t i = 0; i < numBuffers; i++) {
        if (!inUse[i] && bufferSizes[i] >= size) {
            inUse[i] = true;
            *outMappedPtr = nullptr;  // Will use glBufferSubData
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
    bufferSizes[idx] = size;
    inUse[idx] = true;
    
    *outMappedPtr = nullptr;
    return idx;
}

void Renderer::BufferPool::Free(size_t bufferIndex) {
    if (bufferIndex < numBuffers) {
        inUse[bufferIndex] = false;
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
    
    std::cout << "[Renderer] Initialized with UBO and batch sorting\n";
}

void Renderer::Shutdown() {
    bufferPool.Shutdown();
    
    if (cameraUBO) {
        glDeleteBuffers(1, &cameraUBO);
    }
    
    batches.clear();
    batchIndices.clear();
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
    // Update UBO once per frame
    glBindBuffer(GL_UNIFORM_BUFFER, cameraUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(CameraUBO), &cameraData);
}

void Renderer::AddRenderable(GLuint VAO, GLuint VBO, GLuint EBO, GLsizei vertexCount,
                            GLenum primitiveType, GLuint shaderProgram,
                            const std::vector<glm::mat4>& transforms,
                            const glm::vec3& color,
                            const glm::vec3& bboxMin,
                            const glm::vec3& bboxMax) {
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
    batch.textureID = 0;
    batch.bboxMin = bboxMin;
    batch.bboxMax = bboxMax;
    
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

    // Render batches in sorted order
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
        }
        
        if (batch.vertexArrayObject != lastVAO) {
            glBindVertexArray(batch.vertexArrayObject);
            lastVAO = batch.vertexArrayObject;
        }
        
        if (batch.instanceCount > 0) {
            glDrawArraysInstanced(batch.primitiveType, 0, batch.vertexCount, batch.instanceCount);
        } else {
            glDrawArrays(batch.primitiveType, 0, batch.vertexCount);
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
    
    // Create instance buffer
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
    
    // Store buffer ID for cleanup (in production, use buffer pool)
    batch.instanceBuffer = instanceBuffer;
}
