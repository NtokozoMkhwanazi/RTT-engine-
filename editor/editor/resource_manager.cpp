#include "resource_manager.h"
#include "modelSystem/Model.h"
#include "shaderSystem/Shader.h"
#include <iostream>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>

namespace Resources {

ResourceManager::ResourceManager() {
    // Preload common shaders
    std::string vertexPath = Config::getShaderPath("VS.glsl");
    std::string fragmentPath = Config::getShaderPath("FS.glsl");
    
    try {
        auto shader = std::make_shared<Shader>(vertexPath.c_str(), fragmentPath.c_str());
        m_shaders["default"] = shader;
    } catch (const std::exception& e) {
        std::cerr << "[ResourceManager] Failed to load default shader: " << e.what() << std::endl;
    }
}

ResourceManager::~ResourceManager() {
    clearCache();
}

ResourceManager& ResourceManager::getInstance() {
    static ResourceManager instance;
    return instance;
}

std::shared_ptr<Model> ResourceManager::loadModel(const std::string& path) {
    std::string fullPath = resolveModelPath(path);
    
    // Check if already loaded
    auto it = m_models.find(fullPath);
    if (it != m_models.end()) {
        return it->second;
    }
    
    // Load model
    try {
        auto model = std::make_shared<Model>(fullPath);
        if (model) {
            m_models[fullPath] = model;
            std::cout << "[ResourceManager] Loaded model: " << fullPath << std::endl;
            return model;
        }
    } catch (const std::exception& e) {
        std::cerr << "[ResourceManager] Failed to load model " << fullPath << ": " << e.what() << std::endl;
    }
    
    return nullptr;
}

ResourceHandle<Model> ResourceManager::loadModelAsync(const std::string& path) {
    ResourceHandle<Model> handle;
    handle.loading = true;
    
    std::string fullPath = resolveModelPath(path);
    
    // Check if already loaded
    auto it = m_models.find(fullPath);
    if (it != m_models.end()) {
        handle.resource = it->second;
        handle.loaded = true;
        handle.loading = false;
        return handle;
    }
    
    // Start async loading
    handle.future = std::async(std::launch::async, [fullPath, this]() -> std::shared_ptr<Model> {
        try {
            auto model = std::make_shared<Model>(fullPath);
            if (model) {
                // Thread-safe insertion
                static std::mutex modelMutex;
                std::lock_guard<std::mutex> lock(modelMutex);
                m_models[fullPath] = model;
                std::cout << "[ResourceManager] Async loaded model: " << fullPath << std::endl;
            }
            return model;
        } catch (const std::exception& e) {
            std::cerr << "[ResourceManager] Async load failed for " << fullPath << ": " << e.what() << std::endl;
            return nullptr;
        }
    });
    
    return handle;
}

std::shared_ptr<Model> ResourceManager::getModel(const std::string& name) {
    auto it = m_models.find(name);
    if (it != m_models.end()) {
        return it->second;
    }
    return nullptr;
}

bool ResourceManager::hasModel(const std::string& name) {
    return m_models.find(name) != m_models.end();
}

std::shared_ptr<Shader> ResourceManager::loadShader(const std::string& vertexPath,
                                                    const std::string& fragmentPath) {
    std::string name = vertexPath + "|" + fragmentPath;
    
    auto it = m_shaders.find(name);
    if (it != m_shaders.end()) {
        return it->second;
    }
    
    try {
        auto shader = std::make_shared<Shader>(vertexPath.c_str(), fragmentPath.c_str());
        m_shaders[name] = shader;
        return shader;
    } catch (const std::exception& e) {
        std::cerr << "[ResourceManager] Failed to load shader: " << e.what() << std::endl;
    }
    
    return nullptr;
}

std::shared_ptr<Shader> ResourceManager::getShader(const std::string& name) {
    auto it = m_shaders.find(name);
    if (it != m_shaders.end()) {
        return it->second;
    }
    return nullptr;
}

bool ResourceManager::hasShader(const std::string& name) {
    return m_shaders.find(name) != m_shaders.end();
}

std::vector<std::string> ResourceManager::scanModels(const std::string& directory) {
    std::string fullPath = Config::getPaths().modelPath;
    if (!directory.empty()) {
        fullPath = directory;
    }
    
    return scanDirectory(fullPath, {".fbx", ".gltf", ".glb", ".obj"});
}

std::vector<std::string> ResourceManager::scanTextures(const std::string& directory) {
    std::string fullPath = Config::getPaths().texturePath;
    if (!directory.empty()) {
        fullPath = directory;
    }
    
    return scanDirectory(fullPath, {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr"});
}

std::vector<std::string> ResourceManager::scanDirectory(const std::string& path,
                                                       const std::vector<std::string>& extensions) {
    std::vector<std::string> results;
    
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        std::cerr << "[ResourceManager] Cannot open directory: " << path << std::endl;
        return results;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        
        std::string fullPath = path + "/" + name;
        struct stat st;
        if (stat(fullPath.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                // Recurse into subdirectory
                auto subResults = scanDirectory(fullPath, extensions);
                results.insert(results.end(), subResults.begin(), subResults.end());
            } else {
                // Check extension
                size_t dot = name.find_last_of('.');
                if (dot != std::string::npos) {
                    std::string ext = name.substr(dot);
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    
                    if (std::find(extensions.begin(), extensions.end(), ext) != extensions.end()) {
                        results.push_back(fullPath);
                    }
                }
            }
        }
    }
    
    closedir(dir);
    std::sort(results.begin(), results.end());
    return results;
}

void ResourceManager::clearCache() {
    m_models.clear();
    m_shaders.clear();
    m_textures.clear();
}

void ResourceManager::unloadUnused() {
    // TODO: Implement reference counting and unload models with ref count = 0
}

size_t ResourceManager::getCacheSize() const {
    return m_models.size() + m_shaders.size() + m_textures.size();
}

std::string ResourceManager::resolveModelPath(const std::string& modelName) {
    if (modelName.empty()) return "";
    
    // If it's already a full path, return as-is
    if (modelName[0] == '/' || modelName[0] == '.') {
        return modelName;
    }
    
    // Check if file exists in model path
    std::string fullPath = Config::getPaths().modelPath + "/" + modelName;
    struct stat st;
    if (stat(fullPath.c_str(), &st) == 0) {
        return fullPath;
    }
    
    // Try with common extensions
    const char* extensions[] = {".fbx", ".gltf", ".glb", ".obj", nullptr};
    for (int i = 0; extensions[i] != nullptr; i++) {
        std::string testPath = fullPath + extensions[i];
        if (stat(testPath.c_str(), &st) == 0) {
            return testPath;
        }
    }
    
    return fullPath; // Return original path even if not found
}

std::string ResourceManager::resolveTexturePath(const std::string& textureName) {
    if (textureName.empty()) return "";
    
    if (textureName[0] == '/' || textureName[0] == '.') {
        return textureName;
    }
    
    return Config::getPaths().texturePath + "/" + textureName;
}

std::string ResourceManager::resolveShaderPath(const std::string& shaderName) {
    if (shaderName.empty()) return "";
    
    if (shaderName[0] == '/' || shaderName[0] == '.') {
        return shaderName;
    }
    
    return Config::getPaths().shaderPath + "/" + shaderName;
}

} // namespace Resources