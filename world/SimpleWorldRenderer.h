#pragma once
#include "modelSystem/Model.h"
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>

// Simple model instance
struct SimpleModelInstance {
    std::shared_ptr<Model> model;
    glm::mat4 modelMatrix;
};

// Simple renderer for world objects (trees, rocks, etc.)
class SimpleWorldRenderer {
public:
    SimpleWorldRenderer();
    ~SimpleWorldRenderer();
    
    // Load a model
    int loadModel(const std::string& path);
    
    // Add an instance
    void addInstance(int modelId, const glm::vec3& position,
                     float scale = 1.0f, float rotationY = 0.0f);
    
    // Remove instances beyond max distance
    void cullDistant(const glm::vec3& cameraPos, float maxDist);

    // Model dimensions (bounding-box size) for placement scaling. Returns a
    // sane non-zero size even for degenerate models.
    glm::vec3 getModelSize(int modelId) const;
    
    // Render all instances
    void render(const glm::mat4& view, const glm::mat4& projection);
    
    // Get count
    size_t getInstanceCount() const;

private:
    std::vector<SimpleModelInstance> m_instances;
    std::vector<std::shared_ptr<Model>> m_models;

    // Path -> model id, so loading the same asset (e.g. all three tree
    // variants sharing quiver_tree/q.gltf) reuses one Model + its textures.
    std::unordered_map<std::string, int> m_modelByPath;
};
