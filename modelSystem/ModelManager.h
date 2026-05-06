#pragma once

#include "../../modelSystem/Model.h"
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

/**
 * ModelManager - Central resource manager for 3D models
 * 
 * Handles loading, caching, and providing access to loaded models.
 * Prevents duplicate loading and manages memory efficiently.
 */
class ModelManager {
public:
    /**
     * Get singleton instance
     */
    static ModelManager& getInstance() {
        static ModelManager instance;
        return instance;
    }

    /**
     * Load a model from file path
     * Returns cached model if already loaded
     * 
     * @param filePath Path to model file (GLTF, GLB, FBX, OBJ)
     * @return Pointer to loaded Model, nullptr if failed
     */
    Model* loadModel(const std::string& filePath);

    /**
     * Get a loaded model by path
     * 
     * @param filePath Path used when loading
     * @return Pointer to Model if loaded, nullptr otherwise
     */
    Model* getModel(const std::string& filePath) const;

    /**
     * Get a loaded model by index
     * 
     * @param index Model index
     * @return Pointer to Model if exists, nullptr otherwise
     */
    Model* getModelByIndex(size_t index) const;

    /**
     * Check if model is loaded
     */
    bool isModelLoaded(const std::string& filePath) const;

    /**
     * Check if a model pointer is still valid (not deleted/unloaded)
     * Returns true if the pointer is still in the loaded models cache
     */
    bool isModelPointerValid(const Model* modelPtr) const;

    /**
     * Unload a specific model
     */
    void unloadModel(const std::string& filePath);

    /**
     * Unload all models
     */
    void unloadAll();

    /**
     * Get loaded model count
     */
    size_t getModelCount() const { return loadedModels.size(); }

    /**
     * Get all loaded model paths
     */
    std::vector<std::string> getLoadedModelPaths() const;

    /**
     * Get model statistics
     */
    struct ModelStats {
        size_t totalModels = 0;
        size_t totalMeshes = 0;
        size_t totalVertices = 0;
        size_t totalTriangles = 0;
        float totalMemoryMB = 0.0f;
    };
    ModelStats getStatistics() const;

    /**
     * Create procedural model from VAO
     */
    Model* createProceduralModel(GLuint VAO, GLsizei indexCount);

    /**
     * Debug output
     */
    void printLoadedModels() const;

private:
    ModelManager() = default;
    ~ModelManager() = default;

    // Prevent copying
    ModelManager(const ModelManager&) = delete;
    ModelManager& operator=(const ModelManager&) = delete;

    // Loaded models cache
    struct ModelEntry {
        std::unique_ptr<Model> model;
        std::string filePath;
        bool isProcedural = false;
    };

    std::unordered_map<std::string, ModelEntry> loadedModels;
    std::vector<Model*> modelIndex;  // For fast index-based access

    // Procedural models (not file-based)
    std::vector<std::unique_ptr<Model>> proceduralModels;
};

// Convenience functions
inline Model* LoadModel(const std::string& path) {
    return ModelManager::getInstance().loadModel(path);
}

inline Model* GetModel(const std::string& path) {
    return ModelManager::getInstance().getModel(path);
}
