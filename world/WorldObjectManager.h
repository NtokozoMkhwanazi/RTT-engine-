#pragma once
#include "SimpleWorldRenderer.h"
#include "Octree.h"  // forUI.txt Point 1: WorldSpatialOctree + SpatialRaycast
#include "lighting/LightingEnvironment.h"  // #3: thread env to SimpleWorldRenderer
#include "physicsSystem/Physics.h"
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <memory>
#include <utility>  // std::pair, std::tuple for applyConfigEntries
#include <vector>

// World object types — retained as a convenience enum for the internal
// vegetation system (Tree.type / Rock.type / Plant.type are still ints).
// The DYNAMIC prop system uses STRING keys (loaded from JSON), so artists
// can author any type name (e.g. "M_StreetLamp", "Foliage_Fern") without
// touching this enum. The enum maps to string keys for backward compat.
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

// Convert a WorldObjectType to its string key (backward-compat bridge).
inline const char* WorldObjectTypeToString(WorldObjectType t) {
    switch (t) {
        case WorldObjectType::TREE_PINE:      return "TREE_PINE";
        case WorldObjectType::TREE_OAK:       return "TREE_OAK";
        case WorldObjectType::TREE_BIRCH:     return "TREE_BIRCH";
        case WorldObjectType::ROCK_BOULDER:   return "ROCK_BOULDER";
        case WorldObjectType::ROCK_STONE:     return "ROCK_STONE";
        case WorldObjectType::ROCK_CLIFF:     return "ROCK_CLIFF";
        case WorldObjectType::GRASS_CLUSTER:  return "GRASS_CLUSTER";
        case WorldObjectType::FLOWER_PATCH:   return "FLOWER_PATCH";
        case WorldObjectType::BUSH:           return "BUSH";
        case WorldObjectType::LOG:            return "LOG";
        case WorldObjectType::STUMP:          return "STUMP";
        default:                              return "UNKNOWN";
    }
}

// World object configuration — now string-keyed. `typeKey` is the dynamic
// string name (e.g. "TREE_PINE", "M_StreetLamp"). `windSway` drives the
// vertex-shader foliage sway amplitude (0 = static like rocks).
struct WorldObjectConfig {
    std::string modelPath;
    float minScale = 0.8f;
    float maxScale = 1.2f;
    float windSway = 0.0f;                       // 0 = no sway (rocks/props)
    float lodDistance[3] = {20.0f, 50.0f, 100.0f};
    bool castShadow = true;
    bool receiveShadow = true;
};

// Manages all world objects (trees, rocks, props)
class WorldObjectManager {
public:
    WorldObjectManager();
    ~WorldObjectManager();
    
    // Initialize — clears any previous state. Does NOT load hardcoded configs
    // or models. The artist's JSON config (config/world_objects.json) is the
    // sole source of truth via loadConfigFromFile() / loadDynamicObject().
    void initialize(const std::string& assetDir = "assets/world_objects/");

    // Load a dynamic configuration descriptor file directly (e.g.,
    // config/world_objects.json). Replaces all current configs.
    bool loadConfigFromFile(const std::string& filepath);

    // Save dynamic configurations straight back to the asset folder.
    bool saveConfigToFile(const std::string& filepath) const;

    // Runtime dynamic loader bound to STRING keys rather than the old enum.
    // Artists can pass any typeKey (e.g. "M_StreetLamp"). Returns the modelId
    // or -1 on failure.
    int loadDynamicObject(const std::string& typeKey, const std::string& modelPath,
                          float minScale = 0.8f, float maxScale = 1.2f,
                          float windSway = 0.0f);

    // Load models for ALL string-keyed configs (used after loadConfigFromFile
    // or to (re)import the active set). Skips already-loaded types. Missing
    // assets fall back to a procedural placeholder (verify.txt Point 2).
    void loadModels();

    // ---- Async (worker-parse, main-GL) bulk load of the active config set ----
    // Mirrors loadModels() but performs the heavy Assimp parse + texture decode
    // on worker threads (Model::LoadAsync) while ONLY the GL buffer upload +
    // decimation runs on the main thread via commitWorldObjectLoading(). Lets
    // the editor surface a live "Resolving..." progress bar and keeps the
    // viewport responsive during the heavy "layers/objects" rebuild. Call
    // commitWorldObjectLoading() every frame until isWorldObjectLoadDone().

    // Populate the config table WITHOUT loading (used by the async rebuild
    // path so dispatch starts from a fully-known set of assets).
    void addConfigEntry(const std::string& typeKey, const WorldObjectConfig& cfg);

    // Dispatch non-blocking CPU parses for every string-keyed config (skips
    // already-loaded types + disabled 0..0-range types + missing assets).
    void loadModelsAsync();

    // Pump one commit step on the MAIN (GL) thread: upload + decimate + register
    // any models whose worker parse finished, then wire up the per-type book-
    // keeping (model ids, base scales, wind sway, enum mirror). Returns aggregate
    // progress 0..1. Safe to call every frame; no-ops when idle/done.
    float commitWorldObjectLoading();

    bool isWorldObjectLoading() const;
    bool isWorldObjectLoadDone() const;
    float getWorldObjectLoadProgress() const;
    void cancelWorldObjectLoading();

    // Mutable accessor for the UI's editable config table (forUI.txt Step 2).
    std::unordered_map<std::string, WorldObjectConfig>& getMutableStringConfigs() {
        return m_stringConfigs;
    }

    // Read-only accessor for defaults (used by WorldManager for vegetation).
    const std::unordered_map<std::string, WorldObjectConfig>& getActiveConfigs() const {
        return m_stringConfigs;
    }

    // Backward-compat: load a custom object by enum (delegates to
    // loadDynamicObject with the enum's string name). Still used by the
    // Hot-Swap/Bind button and WorldManager's JSON-override path.
    int loadObject(WorldObjectType type, const std::string& modelPath,
                   float minScale = 0.8f, float maxScale = 1.2f);

    // Wire a physics world in: solid placed objects (boulders/rocks/trees/
    // logs/stumps) then register ONE static box collider each, so the
    // play-mode character collides with the visible world objects instead of
    // walking through them. Ground cover (grass/flowers/bushes) stays
    // walk-through. Without a physics world, placement works normally and
    // simply skips body registration (edit-mode path).
    void setPhysicsWorld(PhysicsWorld* physicsWorld);
    
    // Place an object in the world (enum-based, backward compat).
    void placeObject(WorldObjectType type, const glm::vec3& position,
                     float scale = 1.0f, float rotationY = 0.0f,
                     const glm::vec3& colorTint = glm::vec3(1.0f));

    // Place an object in the world (string-keyed, the dynamic path).
    void placeObject(const std::string& typeKey, const glm::vec3& position,
                     float scale = 1.0f, float rotationY = 0.0f,
                     const glm::vec3& colorTint = glm::vec3(1.0f));

    // verify.txt Step 3: place a single object by string key, returning the
    // instance index (for picking). Used directly by the viewport brush painter.
    int placeDynamicObjectByString(const std::string& typeKey,
                                    const glm::vec3& position,
                                    float scale = 1.0f, float rotationY = 0.0f,
                                    const glm::vec3& colorTint = glm::vec3(1.0f));
    
    // Place multiple objects (for vegetation system integration)
    void placeObjects(WorldObjectType type, 
                      const std::vector<glm::vec3>& positions,
                      const std::vector<float>& scales,
                      const std::vector<float>& rotations);

    // Place multiple objects (string-keyed)
    void placeObjects(const std::string& typeKey,
                      const std::vector<glm::vec3>& positions,
                      const std::vector<float>& scales,
                      const std::vector<float>& rotations);
    
    // forUI.txt Pillar 2: flush pending placements into the spatial index so a
    // brush stroke's instances are cullable/visible this frame.
    void ForceRebuildSpatialIndex();

    // forUI.txt Point 1: O(log N) viewport prop picking via the spatial octree.
    bool SpatialRaycast(const glm::vec3& orig, const glm::vec3& dir, float maxDist,
                        float& outDist, size_t& outIndex) const;

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

    // forPlayer.txt Theme 2/3: expose the prop renderer (the SAME instance
    // render() draws at world_manager.cpp:280) so editor UI can raycast /
    // enumerate instanced world objects via EditorApplication::getWorldRenderer().
    // Accessor only - no second renderer, no double-draw.
    SimpleWorldRenderer& getSimpleWorldRenderer() noexcept { return m_renderer; }
    const SimpleWorldRenderer& getSimpleWorldRenderer() const noexcept { return m_renderer; }

    // Get object count
    size_t getObjectCount() const;

    // Expose the hardcoded default modelPath for a type (used by WorldManager
    // to resolve pine-tree path detection for vegetation defaults). Empty if
    // the type has no default model.
    std::string getDefaultModelPath(WorldObjectType type) const;
    
private:
    SimpleWorldRenderer m_renderer;

    // Dynamic string-keyed config (the primary system). Artists author these
    // via engine_config.json "world_objects" / the UI's Dynamic Prop Configurator.
    std::unordered_map<std::string, WorldObjectConfig> m_stringConfigs;
    std::map<std::string, int> m_stringModelIds;
    std::map<std::string, float> m_stringModelBaseScale;

    // Pine tree asset path detected during initialize() — used to build the
    // string default for TREE_PINE / TREE_OAK / TREE_BIRCH when no JSON entry
    // exists for them.
    std::string m_pinePath;
    std::string m_assetDir;

    // Backward-compat enum-keyed views (mirrors the string maps). Used by
    // placeObject(WorldObjectType) callers in the vegetation system.
    std::map<WorldObjectType, WorldObjectConfig> m_configs;
    std::map<WorldObjectType, int> m_modelIds;
    std::map<WorldObjectType, float> m_modelBaseScale;

    bool m_initialized = false;
    bool m_shadowsEnabled = true;
    
    // Generate random scale for object type
    float randomScale(const std::string& typeKey) const;
    float randomScale(WorldObjectType type) const;

    // Reference height in meters for a type (placement param 1.0 == this height)
    float referenceHeight(WorldObjectType type) const;
    float referenceHeight(const std::string& typeKey) const;

    // Register (or skip) the static collider for a just-placed object.
    // `instanceScale` is the FINAL normalized scale (placement * baseScale)
    // applied to the instance matrix.
    void registerStaticCollider(const std::string& typeKey,
                                const glm::vec3& position,
                                float instanceScale, float rotationY);

    // Post-load bookkeeping shared by the sync loadDynamicObject() and the
    // async commitWorldObjectLoading() path: with a model id in hand, record the
    // string-keyed id, derive the reference-height base scale, apply wind sway,
    // log, and mirror into the enum-keyed maps. Mirrors the original
    // loadDynamicObject math verbatim.
    void recordModelForType(const std::string& typeKey,
                            const std::string& modelPath,
                            const WorldObjectConfig& cfg,
                            int modelId, float windSway);

    PhysicsWorld* m_physicsWorld = nullptr;
    std::vector<BodyHandle> m_bodyHandles;  // one per registered collider

    // forUI.txt Point 1: AABB spatial octree indexing world-object instances.
    WorldSpatialOctree m_spatialOctree;
};
