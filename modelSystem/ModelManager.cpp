#include "ModelManager.h"
#include <iostream>
#include <algorithm>

Model* ModelManager::loadModel(const std::string& filePath) {
    // Check if already loaded
    auto it = loadedModels.find(filePath);
    if (it != loadedModels.end()) {
        std::cout << "[ModelManager] Using cached model: " << filePath << "\n";
        return it->second.model.get();
    }

    // Load new model
    std::cout << "[ModelManager] Loading model: " << filePath << "\n";
    
    try {
        auto model = std::make_unique<Model>(filePath);
        
        if (!model || model->GetMeshCount() == 0) {
            std::cerr << "[ModelManager] Failed to load model: " << filePath << "\n";
            return nullptr;
        }

        Model* modelPtr = model.get();
        
        ModelEntry entry;
        entry.model = std::move(model);
        entry.filePath = filePath;
        entry.isProcedural = false;
        
        loadedModels[filePath] = std::move(entry);
        modelIndex.push_back(modelPtr);

        std::cout << "[ModelManager] Successfully loaded: " << filePath 
                  << " | Meshes: " << modelPtr->GetMeshCount()
                  << " | Bones: " << modelPtr->GetSkeleton().bones.size()
                  << " | Animations: " << modelPtr->GetAnimationCount() << "\n";

        return modelPtr;
    } catch (const std::exception& e) {
        std::cerr << "[ModelManager] Exception loading model: " << e.what() << "\n";
        return nullptr;
    }
}

Model* ModelManager::getModel(const std::string& filePath) const {
    auto it = loadedModels.find(filePath);
    if (it != loadedModels.end()) {
        return it->second.model.get();
    }
    return nullptr;
}

Model* ModelManager::getModelByIndex(size_t index) const {
    if (index < modelIndex.size()) {
        return modelIndex[index];
    }
    return nullptr;
}

bool ModelManager::isModelLoaded(const std::string& filePath) const {
    return loadedModels.find(filePath) != loadedModels.end();
}

bool ModelManager::isModelPointerValid(const Model* modelPtr) const {
    if (!modelPtr) return false;
    for (const auto& [path, entry] : loadedModels) {
        if (entry.model.get() == modelPtr) {
            return true;
        }
    }
    for (const auto& procModel : proceduralModels) {
        if (procModel.get() == modelPtr) {
            return true;
        }
    }
    return false;
}

void ModelManager::unloadModel(const std::string& filePath) {
    auto it = loadedModels.find(filePath);
    if (it != loadedModels.end()) {
        // Remove from index
        auto* modelPtr = it->second.model.get();
        modelIndex.erase(
            std::remove(modelIndex.begin(), modelIndex.end(), modelPtr),
            modelIndex.end()
        );
        
        std::cout << "[ModelManager] Unloaded model: " << filePath << "\n";
        loadedModels.erase(it);
    }
}

void ModelManager::unloadAll() {
    std::cout << "[ModelManager] Unloading all models (" << loadedModels.size() << ")\n";
    loadedModels.clear();
    modelIndex.clear();
    proceduralModels.clear();
}

std::vector<std::string> ModelManager::getLoadedModelPaths() const {
    std::vector<std::string> paths;
    paths.reserve(loadedModels.size());
    for (const auto& [path, entry] : loadedModels) {
        paths.push_back(path);
    }
    return paths;
}

ModelManager::ModelStats ModelManager::getStatistics() const {
    ModelStats stats;
    stats.totalModels = loadedModels.size();

    for (const auto& [path, entry] : loadedModels) {
        const auto* model = entry.model.get();
        if (!model) continue;

        stats.totalMeshes += model->GetMeshCount();
        stats.totalVertices += model->GetVertexCount();
        stats.totalTriangles += model->GetTotalTriangleCount();

        // Estimate memory usage
        for (size_t i = 0; i < model->GetMeshCount(); ++i) {
            const auto& mesh = model->GetMesh(i);
            stats.totalMemoryMB += mesh.GetStatistics().memoryUsageMB;
        }
    }

    return stats;
}

Model* ModelManager::createProceduralModel(GLuint VAO, GLsizei indexCount) {
    auto model = std::make_unique<Model>("");
    model->setDebugVAO(VAO);
    model->setDebugIndexCount(indexCount);
    
    Model* modelPtr = model.get();
    proceduralModels.push_back(std::move(model));
    
    return modelPtr;
}

void ModelManager::printLoadedModels() const {
    std::cout << "\n========== LOADED MODELS ==========\n";
    std::cout << "Total models: " << loadedModels.size() << "\n\n";
    
    for (const auto& [path, entry] : loadedModels) {
        const auto* model = entry.model.get();
        if (!model) continue;

        std::cout << "Path: " << path << "\n";
        std::cout << "  Meshes: " << model->GetMeshCount() << "\n";
        std::cout << "  Vertices: " << model->GetVertexCount() << "\n";
        std::cout << "  Triangles: " << model->GetTotalTriangleCount() << "\n";
        std::cout << "  Bones: " << model->GetSkeleton().bones.size() << "\n";
        std::cout << "  Animations: " << model->GetAnimationCount() << "\n";
        
        // Print animations
        for (size_t i = 0; i < model->GetAnimationCount(); ++i) {
            const auto* anim = model->GetAnimation(i);
            if (anim) {
                std::cout << "    [" << i << "] " << anim->name 
                         << " (" << anim->GetDuration() << "s)\n";
            }
        }
        
        std::cout << "\n";
    }
    std::cout << "=====================================\n\n";
}
