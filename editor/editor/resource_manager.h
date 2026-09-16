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
    };
    
    // Helper functions
    inline ResourceManager& getResourceManager() {
        return ResourceManager::getInstance();
    }
}