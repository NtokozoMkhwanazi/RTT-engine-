#pragma once
#include "SimpleWorldRenderer.h"
#include "lighting/LightingEnvironment.h"  // #3: thread env to SimpleWorldRenderer
#include "physicsSystem/Physics.h"
#include <string>
#include <map>
#include <memory>

// World object types
enum class WorldObjectType {
    TREE_PINE,
    TREE_OAK,
    TREE_BIRCH,
    ROCK_BOULDER,
    ROCK_STONE,
    ROCK_CLIFF,
    GRASS_CLUSTER,
    FLOWER_PATCH,
    BUSH,
    LOG,
    STUMP,
    COUNT
};

// World object configuration
struct WorldObjectConfig {
    std::string modelPath;
    float minScale = 0.8f;
    float maxScale = 1.2f;
    float lodDistance[3] = {20.0f, 50.0f, 100.0f};  // LOD thresholds
    bool castShadow = true;
    bool receiveShadow = true;
};

// Manages all world objects (trees, rocks, props)
class WorldObjectManager {
public:
    WorldObjectManager();
    ~WorldObjectManager();
    
    // Initialize with default objects
    void initialize(const std::string& assetDir = "assets/world_objects/");

    // Wire a physics world in: solid placed objects (boulders/rocks/trees/
    // logs/stumps) then register ONE static box collider each, so the
    // play-mode character collides with the visible world objects instead of
    // walking through them. Ground cover (grass/flowers/bushes) stays
    // walk-through. Without a physics world, placement works normally and
    // simply skips body registration (edit-mode path).
    void setPhysicsWorld(PhysicsWorld* physicsWorld);
    
    // Load a custom object type
    int loadObject(WorldObjectType type, const std::string& modelPath,
                   float minScale = 0.8f, float maxScale = 1.2f);
    
    // Place an object in the world
    void placeObject(WorldObjectType type, const glm::vec3& position,
                     float scale = 1.0f, float rotationY = 0.0f,
                     const glm::vec3& colorTint = glm::vec3(1.0f));
    
    // Place multiple objects (for vegetation system integration)
    void placeObjects(WorldObjectType type, 
                      const std::vector<glm::vec3>& positions,
                      const std::vector<float>& scales,
                      const std::vector<float>& rotations);
    
    // Update (cull distant objects, update LOD)
    void update(const glm::vec3& cameraPos, float dt);
    
    // Render all objects
    void render(const glm::mat4& view, const glm::mat4& projection,
                const glm::vec3& cameraPos,
                const LightingEnvironment& lighting = LightingEnvironment::Instance());
    
    // Clear all placed objects
    void clear();

    // Toggle frustum culling for all world-object instances.
    void setFrustumCulling(bool enabled) { m_renderer.setFrustumCulling(enabled); }
    bool isFrustumCullingEnabled() const { return m_renderer.isFrustumCullingEnabled(); }

    // Get object count
    size_t getObjectCount() const;

private:
    SimpleWorldRenderer m_renderer;
    std::map<WorldObjectType, int> m_modelIds;  // Map type to loaded model ID
    std::map<WorldObjectType, WorldObjectConfig> m_configs;

    // Normalization: each raw asset is scaled so its Y extent maps to a
    // reference height in meters (see referenceHeight()), letting the
    // vegetation placement scales (heights in meters) render at sane sizes
    // regardless of how big the source model is.
    std::map<WorldObjectType, float> m_modelBaseScale;

    bool m_initialized = false;
    bool m_shadowsEnabled = true;
    
    // Generate random scale for object type
    float randomScale(WorldObjectType type) const;
    // Reference height in meters for a type (placement param 1.0 == this height)
    float referenceHeight(WorldObjectType type) const;

    // Register (or skip) the static collider for a just-placed object.
    // `instanceScale` is the FINAL normalized scale (placement * baseScale)
    // applied to the instance matrix.
    void registerStaticCollider(WorldObjectType type, const glm::vec3& position,
                                float instanceScale, float rotationY);

    PhysicsWorld* m_physicsWorld = nullptr;
    std::vector<BodyHandle> m_bodyHandles;  // one per registered collider
};
