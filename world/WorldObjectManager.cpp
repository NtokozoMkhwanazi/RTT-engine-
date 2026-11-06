#include "WorldObjectManager.h"
#include <iostream>
#include <chrono>
#include <random>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <jsoncpp/json/json.h>

WorldObjectManager::WorldObjectManager() {
}

WorldObjectManager::~WorldObjectManager() {
}

void WorldObjectManager::setPhysicsWorld(PhysicsWorld* physicsWorld) {
    m_physicsWorld = physicsWorld;
}

// forUI.txt Step 1: Dynamic Asset Initialization Sequence.
// initialize() clears all state — NO hardcoded configs, NO models loaded.
// The artist's JSON config (config/world_objects.json) is the sole source of
// truth. Use loadConfigFromFile() or loadDynamicObject() to populate.
void WorldObjectManager::initialize(const std::string& assetDir) {
    std::cout << "[WorldObjectManager] Dynamic Asset Initialization Sequence...\n";
    m_assetDir = assetDir;
    m_configs.clear();
    m_modelBaseScale.clear();
    m_modelIds.clear();
    m_stringConfigs.clear();
    m_stringModelIds.clear();
    m_stringModelBaseScale.clear();
    m_pinePath.clear();
    m_assetDir = assetDir;

    // NO hardcoded defaults, NO pine tree detection, NO procedural fallbacks.
    // The artist's JSON config is the SOLE source of truth. If a config entry
    // or asset file is missing, the artist is alerted (see rebuildWorldObjects
    // and loadModels). This is an artist-driven engine — no prebaked stand-ins.
    m_initialized = true;
    std::cout << "[WorldObjectManager] Ready for dynamic config (loadConfigFromFile / loadDynamicObject)\n";
}

// forUI.txt: Load a world_objects.json schema file. Replaces all configs.
bool WorldObjectManager::loadConfigFromFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "[WorldObjectManager] Warning: Failed to open config file: " << filepath << "\n";
        return false;
    }

    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string errors;
    if (!Json::parseFromStream(reader, file, &root, &errors)) {
        std::cerr << "[WorldObjectManager] JSON Parsing Error: " << errors << "\n";
        return false;
    }

    m_stringConfigs.clear();
    const Json::Value& objectsArray = root["world_objects"];

    for (const auto& objNode : objectsArray) {
        std::string typeKey = objNode["type"].asString();
        WorldObjectConfig cfg;
        cfg.modelPath = objNode["modelPath"].asString();
        cfg.minScale  = objNode.get("minScale", 0.8f).asFloat();
        cfg.maxScale  = objNode.get("maxScale", 1.2f).asFloat();
        cfg.windSway  = objNode.get("windSway", 0.0f).asFloat();

        m_stringConfigs[typeKey] = cfg;
        std::cout << "[WorldObjectManager] Dynamically Registered Asset: ["
                  << typeKey << "] -> " << cfg.modelPath << "\n";
    }

    loadModels();
    file.close();
    return true;
}

// forUI.txt: Save dynamic configs back to JSON.
bool WorldObjectManager::saveConfigToFile(const std::string& filepath) const {
    Json::Value root;
    Json::Value objectsArray = Json::arrayValue;

    for (const auto& [typeKey, cfg] : m_stringConfigs) {
        Json::Value objNode;
        objNode["type"]      = typeKey;
        objNode["modelPath"] = cfg.modelPath;
        objNode["minScale"]  = cfg.minScale;
        objNode["maxScale"]  = cfg.maxScale;
        objNode["windSway"]  = cfg.windSway;
        objectsArray.append(objNode);
    }

    root["world_objects"] = objectsArray;
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";

    std::ofstream file(filepath);
    if (!file.is_open()) return false;

    file << Json::writeString(builder, root);
    file.close();
    return true;
}

// forUI.txt: Runtime dynamic loader bound to strings rather than the enum.
int WorldObjectManager::loadDynamicObject(const std::string& typeKey,
                                          const std::string& modelPath,
                                          float minScale, float maxScale,
                                          float windSway) {
    WorldObjectConfig config;
    config.modelPath = modelPath;
    config.minScale  = minScale;
    config.maxScale  = maxScale;
    config.windSway  = windSway;

    // Populate the config table BEFORE the (sync) load so a failed parse still
    // leaves the authoring entry in place — mirrors the original behaviour
    // where m_stringConfigs[typeKey] was always set.
    m_stringConfigs[typeKey] = config;

    int modelId = m_renderer.loadModel(modelPath);
    recordModelForType(typeKey, modelPath, config, modelId, windSway);
    return modelId;
}

// Shared post-load bookkeeping for both the sync loadDynamicObject() and the
// async commitWorldObjectLoading() path: once a model id is known, record the
// string-keyed id, derive the reference-height base scale, apply wind sway, log,
// and mirror into the enum-keyed maps (backward-compat placeObject callers).
// Replicates the original loadDynamicObject math verbatim, including the
// intentional refHeight quirk (trees=75, all other string types=1.0) that
// differs from the enum referenceHeight() overload for rocks/etc.
void WorldObjectManager::recordModelForType(const std::string& typeKey,
                                            const std::string& modelPath,
                                            const WorldObjectConfig& cfg,
                                            int modelId, float windSway) {
    if (modelId < 0) return;

    m_stringModelIds[typeKey] = modelId;
    const float modelH = m_renderer.getModelSize(modelId).y;
    if (modelH > 0.0f) {
        // Reference height normalization based on naming hints
        float refHeight = 1.0f;
        if (typeKey.find("TREE") != std::string::npos || typeKey.find("Tree") != std::string::npos) {
            refHeight = 75.0f;
        }
        m_stringModelBaseScale[typeKey] = refHeight / modelH;
    }

    if (windSway > 0.0f) {
        m_renderer.setWindStrength(modelId, windSway);
    }
    std::cout << "[WorldObjectManager] Loaded dynamic object: ["
              << typeKey << "] (" << modelPath << ", ID " << modelId << ")\n";

    // Also mirror into the enum-keyed map for backward-compat callers
    // (placeObject(WorldObjectType)). This is a best-effort bridge: only
    // exact string matches for known enum names are mirrored.
    WorldObjectType enumType = WorldObjectType::COUNT;
    for (WorldObjectType t : {WorldObjectType::TREE_PINE, WorldObjectType::TREE_OAK,
                              WorldObjectType::TREE_BIRCH, WorldObjectType::ROCK_BOULDER,
                              WorldObjectType::ROCK_STONE, WorldObjectType::ROCK_CLIFF,
                              WorldObjectType::GRASS_CLUSTER, WorldObjectType::FLOWER_PATCH,
                              WorldObjectType::BUSH, WorldObjectType::LOG, WorldObjectType::STUMP}) {
        if (WorldObjectTypeToString(t) == typeKey) { enumType = t; break; }
    }
    if (enumType != WorldObjectType::COUNT) {
        m_configs[enumType] = cfg;
        m_modelIds[enumType] = modelId;
        const float modelH = m_renderer.getModelSize(modelId).y;
        if (modelH > 0.0f) m_modelBaseScale[enumType] = referenceHeight(enumType) / modelH;
    }
}

// ---- Async (worker-parse, main-GL) world-object model loading ----
// These delegate the heavy lifting to SimpleWorldRenderer's async pipeline;
// this layer only owns the config table + the string/enum bookkeeping that
// must run on the main thread once a model id is known.

void WorldObjectManager::addConfigEntry(const std::string& typeKey, const WorldObjectConfig& cfg) {
    m_stringConfigs[typeKey] = cfg;
}

void WorldObjectManager::loadModelsAsync() {
    // Reset any previous async run, then dispatch a non-blocking CPU parse for
    // every string-keyed config (skip disabled + already-loaded + missing).
    m_renderer.cancelAsyncModelLoads();

    for (const auto& [typeKey, cfg] : m_stringConfigs) {
        // Same "Disabled" guard as loadModels() (0..0 scale range).
        if (cfg.minScale == 0.0f && cfg.maxScale == 0.0f) {
            std::cout << "  [Disabled] " << typeKey << " (" << cfg.modelPath
                      << " - 0..0 scale range, not loaded)\n";
            continue;
        }
        // Skip types that already have a registered model id.
        if (m_stringModelIds.count(typeKey)) continue;

        // Mirror the asset-not-found alert from rebuildWorldObjects so the
        // artist isn't left waiting on a parse that will just fail.
        if (!cfg.modelPath.empty() && !std::filesystem::exists(cfg.modelPath)) {
            std::cout << "  [Missing] " << typeKey << " -> " << cfg.modelPath
                      << " (asset not found — please place the model in the assets folder)\n";
            continue;
        }

        m_renderer.beginAsyncModelLoad(typeKey, cfg.modelPath, 12000, cfg.windSway);
    }
    std::cout << "[WorldObjectManager] async model dispatch complete: "
              << m_renderer.getAsyncLoadProgress() << " (parsing on workers)\n";
}

float WorldObjectManager::commitWorldObjectLoading() {
    if (!m_renderer.isAsyncLoading() && !m_renderer.isAsyncLoadFinished()) return 1.0f;

    // One GL commit step: finishes whatever worker parses landed this frame.
    const float progress = m_renderer.commitAsyncModels();

    // Wire up WOM-level bookkeeping for every model committed since the last
    // pump (string ids, base scales, wind, enum mirror).
    for (const auto& result : m_renderer.drainCommittedModelIds()) {
        if (result.modelId < 0) continue;
        const auto it = m_stringConfigs.find(result.typeKey);
        if (it == m_stringConfigs.end()) continue;
        recordModelForType(result.typeKey, result.path, it->second,
                           result.modelId, result.windSway);
    }

    if (m_renderer.isAsyncLoadFinished()) {
        std::cout << "[WorldObjectManager] async world-object models committed: "
                  << m_stringModelIds.size() << " types registered\n";
    }
    return progress;
}

bool WorldObjectManager::isWorldObjectLoading() const {
    return m_renderer.isAsyncLoading();
}

bool WorldObjectManager::isWorldObjectLoadDone() const {
    return m_renderer.isAsyncLoadFinished();
}

float WorldObjectManager::getWorldObjectLoadProgress() const {
    return m_renderer.getAsyncLoadProgress();
}

void WorldObjectManager::cancelWorldObjectLoading() {
    m_renderer.cancelAsyncModelLoads();
}

// Backward-compat: load by enum (delegates to loadDynamicObject).
int WorldObjectManager::loadObject(WorldObjectType type, const std::string& modelPath,
                                    float minScale, float maxScale) {
    return loadDynamicObject(WorldObjectTypeToString(type), modelPath,
                             minScale, maxScale,
                             (type == WorldObjectType::GRASS_CLUSTER) ? 0.5f
                           : (type == WorldObjectType::FLOWER_PATCH) ? 0.35f
                           : (type == WorldObjectType::BUSH) ? 0.3f
                           : 0.0f);
}

// Load models for ALL string-keyed configs (used after loadConfigFromFile
// or applyConfigEntries). Only loads types not already loaded.
void WorldObjectManager::loadModels() {
    m_stringModelBaseScale.clear();
    m_stringModelIds.clear();

    for (const auto& [typeKey, config] : m_stringConfigs) {
        if (config.minScale == 0.0f && config.maxScale == 0.0f) {
            std::cout << "  [Disabled] " << typeKey << " (" << config.modelPath
                      << " - 0..0 scale range, not loaded)\n";
            continue;
        }

        // If already loaded (e.g. via loadDynamicObject), skip
        if (m_stringModelIds.count(typeKey)) continue;

        int modelId = m_renderer.loadModel(config.modelPath);
        if (modelId < 0) {
            // NO procedural fallback — the artist is alerted that their asset
            // path is missing so they can fix it, rather than silently
            // substituting a default model.
            std::cout << "  [Missing] " << typeKey << " -> " << config.modelPath
                      << " (asset not found — please place the model in the assets folder)\n";
        }
        if (modelId >= 0) {
            m_stringModelIds[typeKey] = modelId;
            const float modelH = m_renderer.getModelSize(modelId).y;
            if (modelH > 0.0f) {
                float refHeight = referenceHeight(typeKey);
                m_stringModelBaseScale[typeKey] = refHeight / modelH;
            }
            if (config.windSway > 0.0f)
                m_renderer.setWindStrength(modelId, config.windSway);
            std::cout << "  Loaded [" << typeKey << "] " << config.modelPath
                      << " (ID: " << modelId << ")\n";
        } else {
            std::cout << "  [Skipped] " << typeKey << " -> asset missing, not loaded\n";
        }
    }

    // Mirror into enum-keyed maps for backward-compat callers
    for (WorldObjectType t : {WorldObjectType::TREE_PINE, WorldObjectType::TREE_OAK,
                              WorldObjectType::TREE_BIRCH, WorldObjectType::ROCK_BOULDER,
                              WorldObjectType::ROCK_STONE, WorldObjectType::ROCK_CLIFF,
                              WorldObjectType::GRASS_CLUSTER, WorldObjectType::FLOWER_PATCH,
                              WorldObjectType::BUSH, WorldObjectType::LOG, WorldObjectType::STUMP}) {
        const std::string key = WorldObjectTypeToString(t);
        auto it = m_stringConfigs.find(key);
        if (it == m_stringConfigs.end()) continue;
        m_configs[t] = it->second;
        auto idIt = m_stringModelIds.find(key);
        if (idIt != m_stringModelIds.end()) {
            m_modelIds[t] = idIt->second;
            auto bsIt = m_stringModelBaseScale.find(key);
            if (bsIt != m_stringModelBaseScale.end())
                m_modelBaseScale[t] = bsIt->second;
        }
    }

    std::cout << "[WorldObjectManager] Models loaded: " << m_stringModelIds.size()
              << " types (" << m_modelIds.size() << " enum-mirrored)\n";
}

// Backward-compat enum-keyed placeObject.
void WorldObjectManager::placeObject(WorldObjectType type, const glm::vec3& position,
                                      float scale, float rotationY,
                                      const glm::vec3& colorTint) {
    placeObject(WorldObjectTypeToString(type), position, scale, rotationY, colorTint);
}

// Dynamic string-keyed placeObject.
void WorldObjectManager::placeObject(const std::string& typeKey, const glm::vec3& position,
                                      float scale, float rotationY,
                                      const glm::vec3& colorTint) {
    placeDynamicObjectByString(typeKey, position, scale, rotationY, colorTint);
}

// verify.txt Step 3: place a single object by string key, returning the
// instance index for picking, or -1 if the type isn't loaded.
int WorldObjectManager::placeDynamicObjectByString(const std::string& typeKey,
                                    const glm::vec3& position,
                                    float scale, float rotationY,
                                    const glm::vec3& colorTint) {
    if (!m_initialized) {
        std::cerr << "[WorldObjectManager] Not initialized!\n";
        return -1;
    }

    auto it = m_stringModelIds.find(typeKey);
    if (it == m_stringModelIds.end()) {
        return -1;  // type not loaded, skip silently
    }

    if (scale <= 0.0f) {
        scale = randomScale(typeKey);
    }

    auto bsIt = m_stringModelBaseScale.find(typeKey);
    if (bsIt != m_stringModelBaseScale.end() && bsIt->second > 0.0f) {
        scale *= bsIt->second;
    }

    const int preCount = m_renderer.getInstanceCount();
        m_renderer.addInstance(it->second, position, scale, rotationY,
                              glm::vec4(colorTint, 1.0f));
        registerStaticCollider(typeKey, position, scale, rotationY);
        return preCount;
}

// Backward-compat enum placeObjects.
void WorldObjectManager::placeObjects(WorldObjectType type,
                                       const std::vector<glm::vec3>& positions,
                                       const std::vector<float>& scales,
                                       const std::vector<float>& rotations) {
    for (size_t i = 0; i < positions.size(); i++) {
        float scale = (i < scales.size()) ? scales[i] : 0.0f;
        float rotation = (i < rotations.size()) ? rotations[i] : 0.0f;
        placeObject(type, positions[i], scale, rotation);
    }
}

// Dynamic string-keyed placeObjects.
void WorldObjectManager::placeObjects(const std::string& typeKey,
                                       const std::vector<glm::vec3>& positions,
                                       const std::vector<float>& scales,
                                       const std::vector<float>& rotations) {
    for (size_t i = 0; i < positions.size(); i++) {
        float scale = (i < scales.size()) ? scales[i] : 0.0f;
        float rotation = (i < rotations.size()) ? rotations[i] : 0.0f;
        placeObject(typeKey, positions[i], scale, rotation);
    }
}

void WorldObjectManager::ForceRebuildSpatialIndex() {
    if (!m_initialized) return;
    m_spatialOctree.RebuildIndex(
        m_renderer.getActiveInstances(),
        glm::vec3(-1500.0f, -100.0f, -1500.0f), glm::vec3(1500.0f, 1000.0f, 1500.0f));
}

bool WorldObjectManager::SpatialRaycast(const glm::vec3& orig, const glm::vec3& dir,
                                        float maxDist, float& outDist, size_t& outIndex) const {
    return m_spatialOctree.Raycast(orig, dir, m_renderer, maxDist, outDist, outIndex);
}

void WorldObjectManager::update(const glm::vec3& cameraPos, float dt) {
    if (!m_initialized) return;
    const float maxCullDistance = 120.0f;
    m_renderer.cullDistant(cameraPos, maxCullDistance);
}

void WorldObjectManager::render(const glm::mat4& view, const glm::mat4& projection,
                                 const glm::vec3& cameraPos,
                                 const LightingEnvironment& lighting) {
    if (!m_initialized) return;
    m_renderer.render(view, projection, lighting);
}

void WorldObjectManager::clear() {
    if (m_physicsWorld) {
        for (auto it = m_bodyHandles.rbegin(); it != m_bodyHandles.rend(); ++it)
            m_physicsWorld->removeBody(*it);
        m_bodyHandles.clear();
    }
    m_renderer.clearAll();
}

size_t WorldObjectManager::getObjectCount() const {
    return m_renderer.getInstanceCount();
}

float WorldObjectManager::randomScale(const std::string& typeKey) const {
    auto it = m_stringConfigs.find(typeKey);
    if (it == m_stringConfigs.end()) return 1.0f;

    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(it->second.minScale, it->second.maxScale);
    return dist(gen);
}

float WorldObjectManager::randomScale(WorldObjectType type) const {
    return randomScale(WorldObjectTypeToString(type));
}

float WorldObjectManager::referenceHeight(WorldObjectType type) const {
    switch (type) {
        case WorldObjectType::TREE_PINE:
        case WorldObjectType::TREE_OAK:
        case WorldObjectType::TREE_BIRCH:
            return 75.0f;
        case WorldObjectType::ROCK_BOULDER:
            return 2.0f;
        case WorldObjectType::ROCK_CLIFF:
            return 2.5f;
        case WorldObjectType::ROCK_STONE:
            return 0.8f;
        case WorldObjectType::GRASS_CLUSTER:
        case WorldObjectType::FLOWER_PATCH:
        case WorldObjectType::BUSH:
            return 1.0f;
        default:
            return 1.0f;
    }
}

float WorldObjectManager::referenceHeight(const std::string& typeKey) const {
    if (typeKey.find("TREE") != std::string::npos || typeKey.find("Tree") != std::string::npos)
        return 75.0f;
    if (typeKey.find("BUSH") != std::string::npos || typeKey.find("GRASS") != std::string::npos
        || typeKey.find("FLOWER") != std::string::npos || typeKey.find("PLANT") != std::string::npos)
        return 1.0f;
    // Match the enum-keyed referenceHeight: stone is short (0.8), cliff tall
    // (2.5). A blanket "ROCK"->2.0 collapsed ROCK_STONE/ROCK_CLIFF to 2.0 and
    // diverged from the enum contract (which CharWalksIntoOffsetStone pins the
    // stone scale to 0.8 / modelHeight).
    if (typeKey.find("ROCK_STONE") != std::string::npos)
        return 0.8f;
    if (typeKey.find("ROCK_CLIFF") != std::string::npos)
        return 2.5f;
    if (typeKey.find("ROCK") != std::string::npos || typeKey.find("BOULDER") != std::string::npos)
        return 2.0f;
    return 1.0f;
}

std::string WorldObjectManager::getDefaultModelPath(WorldObjectType type) const {
    // Return the pine path for tree types; other types have no default path
    // (they must come from the JSON config).
    switch (type) {
        case WorldObjectType::TREE_PINE:
        case WorldObjectType::TREE_OAK:
        case WorldObjectType::TREE_BIRCH:
            return m_pinePath;
        case WorldObjectType::ROCK_BOULDER:
            return m_assetDir + "../boulder/BOULDER.gltf";
        case WorldObjectType::GRASS_CLUSTER:
            return m_assetDir + "../grass/grass.gltf";
        case WorldObjectType::LOG:
            return m_assetDir + "../boulder/rocky_terrain.fbx";
        default:
            return "";
    }
}

void WorldObjectManager::registerStaticCollider(const std::string& typeKey,
                                                const glm::vec3& position,
                                                float instanceScale, float rotationY) {
    if (!m_physicsWorld) return;

    // Walk-through ground cover: grass/flowers/bushes never register bodies.
    if (typeKey.find("GRASS") != std::string::npos ||
        typeKey.find("FLOWER") != std::string::npos ||
        typeKey.find("BUSH") != std::string::npos) {
        return;
    }

    auto it = m_stringModelIds.find(typeKey);
    if (it == m_stringModelIds.end()) return;
    const BoundingBox bb = m_renderer.getModelBounds(it->second);
    if (!bb.IsValid() || instanceScale <= 0.0f) return;

    const glm::vec3 cRaw = bb.Center() * instanceScale;
    const float cr = glm::radians(rotationY);
    const float cs = std::cos(cr), sn = std::sin(cr);
    glm::vec3 center = position + glm::vec3(cRaw.x * cs - cRaw.z * sn,
                                            cRaw.y,
                                            cRaw.x * sn + cRaw.z * cs);
    glm::vec3 half = bb.Extent() * instanceScale;

    const float maxHalfXZ = std::max(half.y * 1.5f, 0.25f);
    half.x = std::min(half.x, maxHalfXZ);
    half.z = std::min(half.z, maxHalfXZ);
    if (half.x < 0.05f || half.y < 0.05f || half.z < 0.05f) return;

    RigidBody body;
    body.position = center;
    body.scale = half * 2.0f;
    body.isStatic = true;
    body.colliderType = ColliderType::BOX;
    body.mass = 0.0f;
    body.friction = 0.8f;
    m_bodyHandles.push_back(m_physicsWorld->addBody(body));
}
