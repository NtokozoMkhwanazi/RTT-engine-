#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <future>
#include <glm/glm.hpp>
#include "config.h"

class Model;
class Shader;
class Texture;

namespace Resources {
    
    // Resource handle for async loading
    template<typename T>
    struct ResourceHandle {
        std::shared_ptr<T> resource;
        std::future<std::shared_ptr<T>> future;
        float progress = 0.0f;
        bool loaded = false;
        bool loading = false;
    };
    
    // Resource manager singleton
    class ResourceManager {
    public:
        static ResourceManager& getInstance();
        
        // Model management
        std::shared_ptr<Model> loadModel(const std::string& path);
        ResourceHandle<Model> loadModelAsync(const std::string& path);
        std::shared_ptr<Model> getModel(const std::string& name);
        bool hasModel(const std::string& name);
        
        // Shader management
        std::shared_ptr<Shader> loadShader(const std::string& vertexPath, 
                                          const std::string& fragmentPath);
        std::shared_ptr<Shader> getShader(const std::string& name);
        bool hasShader(const std::string& name);
        
        // Texture management
        std::shared_ptr<Texture> loadTexture(const std::string& path);
        std::shared_ptr<Texture> getTexture(const std::string& name);
        bool hasTexture(const std::string& name);
        
        // Asset scanning
        std::vector<std::string> scanModels(const std::string& directory);
        std::vector<std::string> scanTextures(const std::string& directory);

        // Asset scanning — NON-BLOCKING (content browser). The recursive
        // directory walk is CPU/IO-bound (opendir/readdir/stat) and blocks the
        // render thread when called synchronously over the whole assets/ tree
        // (the "content browser loads props/textures and freezes the engine"
        // bug). Use the async variants below: they kick the walk on a worker,
        // then the UI polls isAsyncScanReady() (wait_for(0), non-blocking) each
        // frame and fetchAsyncScan()s the result. No GL is touched, so the
        // worker is context-safe.
        void scanModelsAsync(const std::string& directory);
        void scanTexturesAsync(const std::string& directory);
        bool isAsyncScanReady(const std::string& directory) const;
        std::vector<std::string> fetchAsyncScan(const std::string& directory);

        // Join every in-flight async scan. Called by tests (TearDown) and at
        // shutdown so std::async worker threads never leak across the sweep or
        // survive past the ResourceManager singleton's lifetime.
        void drainAsyncScans();
        
        // Cache management
        void clearCache();
        void unloadUnused();
        size_t getCacheSize() const;
        
        // Configuration-based path resolution
        std::string resolveModelPath(const std::string& modelName);
        std::string resolveTexturePath(const std::string& textureName);
        std::string resolveShaderPath(const std::string& shaderName);
        
    private:
        ResourceManager();
        ~ResourceManager();
        
        std::unordered_map<std::string, std::shared_ptr<Model>> m_models;
        std::unordered_map<std::string, std::shared_ptr<Shader>> m_shaders;
        std::unordered_map<std::string, std::shared_ptr<Texture>> m_textures;
        
        std::vector<std::string> scanDirectory(const std::string& path,
                                              const std::vector<std::string>& extensions);

        // In-flight async directory scans keyed by directory (content browser).
        // The scan worker only calls scanDirectory() (pure CPU/IO, no shared
        // mutable state), so the scan itself runs lock-free; only the map of
        // futures is mutex-guarded.
        std::unordered_map<std::string, std::future<std::vector<std::string>>> m_pendingScans;
        mutable std::mutex scanMutex_;
    };
    
    // Helper functions
    inline ResourceManager& getResourceManager() {
        return ResourceManager::getInstance();
    }
}