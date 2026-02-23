#include "BoneMatrixBuffer.h"
#include <iostream>
#include <chrono>
#include <cstring>

// Define SSBO constants if not available (OpenGL 4.3+)
#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#endif

BoneMatrixBuffer::BoneMatrixBuffer() 
    : buffer(0)
    , bufferType(BoneBufferType::NONE)
    , maxBoneCount(0)
    , currentBoneCount(0)
    , initialized(false)
    , needsUpdate(true)
{
}

BoneMatrixBuffer::~BoneMatrixBuffer() {
    Shutdown();
}

bool BoneMatrixBuffer::IsSSBOSupported() {
    // Check OpenGL version (SSBO requires OpenGL 4.3+)
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    
    if (major > 4 || (major == 4 && minor >= 3)) {
        return true;
    }
    
    // For older versions, assume not supported
    // (could check extensions but version check is more reliable)
    return false;
}

void BoneMatrixBuffer::SelectBufferType(size_t requestedBones) {
    // Force SSBO if configured
    if (config.preferSSBO) {
        bufferType = BoneBufferType::SSBO;
        std::cout << "[BoneMatrixBuffer] SSBO forced by config\n";
        return;
    }
    
    // Auto-select based on bone count
    if (requestedBones <= config.uboMaxBones) {
        bufferType = BoneBufferType::UBO;
        std::cout << "[BoneMatrixBuffer] Using UBO for " << requestedBones << " bones\n";
    } else {
        // Check if SSBO is supported
        if (IsSSBOSupported()) {
            bufferType = BoneBufferType::SSBO;
            std::cout << "[BoneMatrixBuffer] Using SSBO for " << requestedBones << " bones\n";
        } else {
            // Fallback to UBO with warning
            bufferType = BoneBufferType::UBO;
            std::cerr << "[BoneMatrixBuffer] WARNING: SSBO not supported, using UBO for " 
                      << requestedBones << " bones (may exceed limits)\n";
        }
    }
}

bool BoneMatrixBuffer::Initialize(size_t maxBones, const BoneBufferConfig& cfg) {
    if (initialized) {
        std::cerr << "[BoneMatrixBuffer] Already initialized!\n";
        return false;
    }

    config = cfg;
    maxBoneCount = maxBones;
    
    // Select buffer type
    SelectBufferType(maxBones);
    
    // Generate buffer
    glGenBuffers(1, &buffer);
    if (buffer == 0) {
        std::cerr << "[BoneMatrixBuffer] Failed to generate buffer!\n";
        return false;
    }

    // Allocate buffer storage
    // std140/std430 layout: each mat4 takes 64 bytes (4 vec4s)
    size_t bufferSize = maxBoneCount * 64;  // 64 bytes per mat4
    
    if (bufferType == BoneBufferType::UBO) {
        // UBO binding
        glBindBuffer(GL_UNIFORM_BUFFER, buffer);
        glBufferData(GL_UNIFORM_BUFFER, bufferSize, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, buffer);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        
        std::cout << "[BoneMatrixBuffer] UBO initialized: " << maxBones << " bones, " 
                  << (bufferSize / 1024) << " KB\n";
    } else {
        // SSBO binding
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, bufferSize, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        
        std::cout << "[BoneMatrixBuffer] SSBO initialized: " << maxBones << " bones, " 
                  << (bufferSize / 1024) << " KB\n";
    }

    initialized = true;
    needsUpdate = true;
    
    return true;
}

void BoneMatrixBuffer::Shutdown() {
    if (buffer != 0) {
        glDeleteBuffers(1, &buffer);
        buffer = 0;
    }
    
    bufferType = BoneBufferType::NONE;
    maxBoneCount = 0;
    currentBoneCount = 0;
    initialized = false;
    needsUpdate = true;
    
    std::cout << "[BoneMatrixBuffer] Shutdown complete\n";
}

bool BoneMatrixBuffer::Update(const std::vector<glm::mat4>& boneMatrices, bool forceUpdate) {
    if (!initialized) {
        std::cerr << "[BoneMatrixBuffer] Not initialized!\n";
        return false;
    }

    if (boneMatrices.empty()) {
        std::cerr << "[BoneMatrixBuffer] Empty bone matrix list!\n";
        return false;
    }

    // Check if update is needed
    if (!forceUpdate && !needsUpdate && boneMatrices.size() == currentBoneCount) {
        return true;  // No update needed
    }

    // Check capacity
    if (boneMatrices.size() > maxBoneCount) {
        std::cerr << "[BoneMatrixBuffer] Too many bones: " << boneMatrices.size() 
                  << " > " << maxBoneCount << "!\n";
        return false;
    }

    // Start timing
    auto startTime = std::chrono::high_resolution_clock::now();

    // Upload data
    const float* data = reinterpret_cast<const float*>(boneMatrices.data());
    size_t dataSize = boneMatrices.size() * 16 * sizeof(float);  // 16 floats per mat4
    
    UploadData(data, dataSize);
    
    currentBoneCount = boneMatrices.size();
    needsUpdate = false;

    // End timing
    auto endTime = std::chrono::high_resolution_clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    
    // Update statistics
    stats.updateCount++;
    stats.bytesUploaded += dataSize;
    stats.lastUpdateTimeMs = elapsedMs;
    
    // Calculate average (smooth over 100 frames)
    frameCount++;
    lastFrameTime += elapsedMs;
    if (frameCount >= 100) {
        stats.avgUpdateTimeMs = lastFrameTime / frameCount;
        frameCount = 0;
        lastFrameTime = 0.0;
    }

    return true;
}

bool BoneMatrixBuffer::UpdateRaw(const float* matrices, size_t count) {
    if (!initialized) {
        std::cerr << "[BoneMatrixBuffer] Not initialized!\n";
        return false;
    }

    if (!matrices || count == 0) {
        std::cerr << "[BoneMatrixBuffer] Invalid matrix data!\n";
        return false;
    }

    // Check capacity
    if (count > maxBoneCount) {
        std::cerr << "[BoneMatrixBuffer] Too many bones: " << count 
                  << " > " << maxBoneCount << "!\n";
        return false;
    }

    // Start timing
    auto startTime = std::chrono::high_resolution_clock::now();

    // Upload data
    size_t dataSize = count * 16 * sizeof(float);
    UploadData(matrices, dataSize);
    
    currentBoneCount = count;
    needsUpdate = false;

    // End timing
    auto endTime = std::chrono::high_resolution_clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    
    // Update statistics
    stats.updateCount++;
    stats.bytesUploaded += dataSize;
    stats.lastUpdateTimeMs = elapsedMs;
    
    frameCount++;
    lastFrameTime += elapsedMs;
    if (frameCount >= 100) {
        stats.avgUpdateTimeMs = lastFrameTime / frameCount;
        frameCount = 0;
        lastFrameTime = 0.0;
    }

    return true;
}

void BoneMatrixBuffer::Bind(GLuint bindingPoint) const {
    if (!initialized) {
        std::cerr << "[BoneMatrixBuffer] Cannot bind - not initialized!\n";
        return;
    }

    if (bufferType == BoneBufferType::UBO) {
        glBindBufferBase(GL_UNIFORM_BUFFER, bindingPoint, buffer);
    } else {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, bindingPoint, buffer);
    }
    
    stats.bindCount++;
}

void BoneMatrixBuffer::Unbind() const {
    if (bufferType == BoneBufferType::UBO) {
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, 0);
    } else {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    }
}

void BoneMatrixBuffer::AllocateBuffer() {
    if (buffer == 0 || maxBoneCount == 0) {
        return;
    }

    size_t bufferSize = maxBoneCount * 64;  // 64 bytes per mat4 (std140/std430)
    
    if (bufferType == BoneBufferType::UBO) {
        glBindBuffer(GL_UNIFORM_BUFFER, buffer);
        glBufferData(GL_UNIFORM_BUFFER, bufferSize, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    } else {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, bufferSize, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }
}

void BoneMatrixBuffer::UploadData(const float* data, size_t size) {
    if (buffer == 0 || !data || size == 0) {
        return;
    }

    if (bufferType == BoneBufferType::UBO) {
        // UBO upload
        glBindBuffer(GL_UNIFORM_BUFFER, buffer);
        
        // Orphan the buffer (allocate new storage) to avoid synchronization stalls
        glBufferData(GL_UNIFORM_BUFFER, size, nullptr, GL_DYNAMIC_DRAW);
        
        // Map and upload
        void* mapped = glMapBuffer(GL_UNIFORM_BUFFER, GL_WRITE_ONLY);
        if (mapped) {
            memcpy(mapped, data, size);
            glUnmapBuffer(GL_UNIFORM_BUFFER);
        } else {
            // Fallback to glBufferSubData if mapping fails
            glBufferSubData(GL_UNIFORM_BUFFER, 0, size, data);
        }
        
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    } else {
        // SSBO upload
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        
        // Orphan the buffer
        glBufferData(GL_SHADER_STORAGE_BUFFER, size, nullptr, GL_DYNAMIC_DRAW);
        
        // Map and upload
        void* mapped = glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_WRITE_ONLY);
        if (mapped) {
            memcpy(mapped, data, size);
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        } else {
            // Fallback to glBufferSubData if mapping fails
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, size, data);
        }
        
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }
}

std::string BoneMatrixBuffer::GetBufferTypeString() const {
    switch (bufferType) {
        case BoneBufferType::UBO: return "UBO";
        case BoneBufferType::SSBO: return "SSBO";
        default: return "NONE";
    }
}

void BoneMatrixBuffer::PrintStats() const {
    std::cout << "\n========== BONE MATRIX BUFFER STATS ==========\n";
    std::cout << "Initialized: " << (initialized ? "YES" : "NO") << "\n";
    std::cout << "Buffer type: " << GetBufferTypeString() << "\n";
    std::cout << "Max bones: " << maxBoneCount << "\n";
    std::cout << "Current bones: " << currentBoneCount << "\n";
    std::cout << "Updates: " << stats.updateCount << "\n";
    std::cout << "Binds: " << stats.bindCount << "\n";
    std::cout << "Bytes uploaded: " << (stats.bytesUploaded / 1024 / 1024) << " MB total\n";
    std::cout << "Last update time: " << stats.lastUpdateTimeMs << " ms\n";
    std::cout << "Avg update time: " << stats.avgUpdateTimeMs << " ms\n";
    
    // Calculate speedup vs old method
    if (stats.avgUpdateTimeMs > 0) {
        double oldMethodTime = currentBoneCount * 0.001;  // ~1μs per uniform
        double speedup = oldMethodTime / stats.avgUpdateTimeMs;
        std::cout << "Speedup vs uniforms: " << speedup << "x\n";
    }
    
    std::cout << "========================================\n\n";
}

void BoneMatrixBuffer::ResetStats() {
    stats = Stats();
    frameCount = 0;
    lastFrameTime = 0.0;
}
