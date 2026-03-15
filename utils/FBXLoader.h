#pragma once

// ============================================================================
// FBX Model Loader Utility
// ============================================================================
// Helper functions for loading and managing multiple FBX models
// ============================================================================

#include "../modelSystem/Model.h"
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <sstream>

// GLM string conversion helper
inline std::string Vec3ToString(const glm::vec3& v) {
    std::ostringstream oss;
    oss << "(" << v.x << ", " << v.y << ", " << v.z << ")";
    return oss.str();
}

// ============================================================================
// Model Load Result
// ============================================================================
struct ModelLoadResult {
    Model* model{nullptr};
    std::string path;
    bool success{false};
    std::string errorMessage;
    
    // Model info
    int boneCount{0};
    int meshCount{0};
    glm::vec3 size{0.0f};
    
    void printInfo() const {
        std::cout << "\n=== Model Load Result: " << path << " ===\n";
        std::cout << "  Success: " << (success ? "YES" : "NO") << "\n";
        if (!success && !errorMessage.empty()) {
            std::cout << "  Error: " << errorMessage << "\n";
        }
        std::cout << "  Bones: " << boneCount << "\n";
        std::cout << "  Meshes: " << meshCount << "\n";
        std::cout << "  Size: " << Vec3ToString(size) << "\n";
    }
};

// ============================================================================
// Load FBX Model with Detailed Error Reporting
// ============================================================================
inline ModelLoadResult LoadFBXModel(const std::string& path, const char* displayName = nullptr) {
    ModelLoadResult result;
    result.path = path;
    
    const char* name = displayName ? displayName : path.c_str();
    std::cout << "\n=== Loading Model: " << name << " ===\n";
    
    try {
        Model* model = new Model(path.c_str());
        
        if (!model) {
            result.errorMessage = "Model pointer is null";
            result.printInfo();
            return result;
        }
        
        int meshCount = model->GetMeshCount();
        if (meshCount == 0) {
            result.errorMessage = "No meshes found in model";
            delete model;
            result.printInfo();
            return result;
        }
        
        const Skeleton& skel = model->GetSkeleton();
        glm::vec3 size = model->GetSize();
        
        result.model = model;
        result.success = true;
        result.boneCount = (int)skel.bones.size();
        result.meshCount = meshCount;
        result.size = size;
        
        std::cout << "  Bones: " << result.boneCount << "\n";
        std::cout << "  Meshes: " << result.meshCount << "\n";
        std::cout << "  Size: " << Vec3ToString(result.size) << "\n";
        
        // Check if model has skeleton (animated) or is static
        if (result.boneCount > 0) {
            std::cout << "  Type: ANIMATED (skeletal)\n";
        } else {
            std::cout << "  Type: STATIC (no skeleton)\n";
        }
        
    } catch (const std::exception& e) {
        result.errorMessage = std::string("Exception: ") + e.what();
        std::cerr << "  [ERROR] " << result.errorMessage << "\n";
    } catch (...) {
        result.errorMessage = "Unknown exception";
        std::cerr << "  [ERROR] Unknown exception\n";
    }
    
    result.printInfo();
    return result;
}

// ============================================================================
// Model Manager - Handle Multiple FBX Models
// ============================================================================
class ModelManager {
public:
    ~ModelManager() {
        clear();
    }
    
    // Load a model and store it by name
    bool load(const std::string& name, const std::string& path) {
        ModelLoadResult result = LoadFBXModel(path, name.c_str());
        
        if (result.success) {
            models[name] = result.model;
            modelInfo[name] = result;
            std::cout << "[ModelManager] Loaded: " << name << "\n";
            return true;
        } else {
            std::cerr << "[ModelManager] Failed to load: " << name << "\n";
            return false;
        }
    }
    
    // Get model by name
    Model* get(const std::string& name) {
        auto it = models.find(name);
        if (it != models.end()) {
            return it->second;
        }
        return nullptr;
    }
    
    // Get all model names
    std::vector<std::string> getModelNames() const {
        std::vector<std::string> names;
        for (const auto& pair : models) {
            names.push_back(pair.first);
        }
        return names;
    }
    
    // Get model count
    size_t getModelCount() const {
        return models.size();
    }
    
    // Clear all models
    void clear() {
        for (auto& pair : models) {
            if (pair.second) {
                delete pair.second;
            }
        }
        models.clear();
        modelInfo.clear();
    }
    
    // Print all loaded models
    void printAll() const {
        std::cout << "\n=== Loaded Models ===\n";
        for (const auto& pair : modelInfo) {
            std::cout << "  " << pair.first << ": "
                      << pair.second.boneCount << " bones, "
                      << pair.second.meshCount << " meshes\n";
        }
    }
    
private:
    std::map<std::string, Model*> models;
    std::map<std::string, ModelLoadResult> modelInfo;
};

// ============================================================================
// Quick Load Multiple Models
// ============================================================================
inline std::vector<ModelLoadResult> LoadMultipleModels(
    const std::vector<std::pair<std::string, std::string>>& modelPaths)
{
    std::vector<ModelLoadResult> results;
    
    for (const auto& pair : modelPaths) {
        results.push_back(LoadFBXModel(pair.second, pair.first.c_str()));
    }
    
    return results;
}

// ============================================================================
// Usage Example:
// ============================================================================
/*
    // Load multiple models
    ModelManager modelMgr;
    modelMgr.load("Bot", "assets/bot.fbx");
    modelMgr.load("Bear", "assets/World_objects/Bear_DEMO.fbx");
    modelMgr.load("Datsun", "assets/World_objects/datsun.fbx");
    
    modelMgr.printAll();
    
    // Switch between models
    Model* currentModel = modelMgr.get("Bot");
    
    // Cleanup
    modelMgr.clear();
*/
