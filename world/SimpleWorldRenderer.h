#pragma once
#include "modelSystem/Model.h"
#include "lighting/LightingEnvironment.h"  // #3: thread env to WaterRenderer
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include "renderer/WaterRenderer.h"

// Simple model instance
struct SimpleModelInstance {
    std::shared_ptr<Model> model;
    glm::mat4 modelMatrix;
    glm::vec4 color = glm::vec4(1.0f);  // per-instance tint (vegetation variation)
};

// Simple renderer for world objects (trees, rocks, etc.)
class SimpleWorldRenderer {
public:
    SimpleWorldRenderer();
    ~SimpleWorldRenderer();
    
    // Load a model. maxTrianglesPerMesh bounds the per-mesh decimation
    // budget (default 12000 for props; ground plants pass a small budget so
    // hundreds of grass tufts stay cheap).
    int loadModel(const std::string& path, size_t maxTrianglesPerMesh = 12000);
    
    // Add an instance. colorTint multiplies the model's albedo (per-instance
    // vegetation color variation); default white = no tint.
    void addInstance(int modelId, const glm::vec3& position,
                     float scale = 1.0f, float rotationY = 0.0f,
                     const glm::vec4& colorTint = glm::vec4(1.0f));

    // Per-model wind sway strength. Plants (grass/flowers/bushes) get a
    // strong value so their tips sway; trees get a small value; rocks 0.
    // Passed to the shared vertex shader as uWindStrength.
    void setWindStrength(int modelId, float strength);
    
    // Remove instances beyond max distance. NON-destructive: instances beyond
    // the radius are parked in a dormant cache and respawn (cache-hit
    // reactivates) when the camera comes back within `respawnDist` (default
    // 85% of maxDist - hysteresis so a boundary object never flickers) - so
    // objects behind/around the camera reappear instead of staying gone.
    void cullDistant(const glm::vec3& cameraPos, float maxDist,
                     float respawnDist = -1.0f);

    // Number of instances parked in the dormant respawn cache.
    size_t getDormantCount() const { return m_dormant.size(); }

    // Toggle frustum culling on/off for all instances. When disabled, every
    // instance is sent to the GPU (useful for debugging or fully-visible
    // small scenes like the arena).
    void setFrustumCulling(bool enabled) { m_frustumCulling = enabled; }
    bool isFrustumCullingEnabled() const { return m_frustumCulling; }

    // Permanently wipe the active instances AND the dormant respawn cache.
    void clearAll();

    // Model dimensions (bounding-box size) for placement scaling. Returns a
    // sane non-zero size even for degenerate models.
    glm::vec3 getModelSize(int modelId) const;

    // Raw model bounding box (model space) - used to place a collider on the
    // VISIBLE mesh: the instance anchors the model origin at its position and
    // scales uniformly, so the visual world-space box is bb.min/max * scale
    // around the origin.
    BoundingBox getModelBounds(int modelId) const;
    
    // Render all instances.
    // #3: `lighting` is threaded from the frame owner to the water renderer.
    void render(const glm::mat4& view, const glm::mat4& projection,
                const LightingEnvironment& lighting = LightingEnvironment::Instance());
    
    // Get count
    size_t getInstanceCount() const;

private:
    std::vector<SimpleModelInstance> m_instances;
    std::vector<SimpleModelInstance> m_dormant;   // respawn cache (cullDistant)
    std::vector<std::shared_ptr<Model>> m_models;
    std::vector<float> m_modelWindStrength;  // parallel to m_models

    // Reused scratch buffer for the per-frame visible-instance collection
    // (avoids rebuilding a fresh unordered_map + vectors every frame).
    std::vector<InstanceData> m_scratch;

    // Procedural water plane (env-driven sun + fog). Rendered on top of the
    // opaque scene so the canonical sun glint + warm-fog tint read coherently.
    // Disabled by default: a flat placeholder water plane at water_level=0
    // occludes the visible terrain (the camera now sits close enough to see it).
    // Toggle m_renderWater = true once proper water is implemented.
    std::unique_ptr<WaterRenderer> m_water;
    bool m_renderWater = false;

    // Frustum culling toggle (editor UI: World Settings → Frustum Culling).
    // Defaults on; when off every instance bypasses the frustum test.
    bool m_frustumCulling = true;

    // Path -> model id, so loading the same asset (e.g. all three tree
    // variants sharing quiver_tree/q.gltf) reuses one Model + its textures.
    std::unordered_map<std::string, int> m_modelByPath;
};
