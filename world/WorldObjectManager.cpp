#include "WorldObjectManager.h"
#include "ProceduralPine.h"
#include <iostream>
#include <chrono>
#include <random>
#include <filesystem>

WorldObjectManager::WorldObjectManager() {
}

WorldObjectManager::~WorldObjectManager() {
}

void WorldObjectManager::setPhysicsWorld(PhysicsWorld* physicsWorld) {
    m_physicsWorld = physicsWorld;
}

void WorldObjectManager::initialize(const std::string& assetDir) {
    std::cout << "[WorldObjectManager] Initializing...\n";

    // Pine tree asset: prefer the lightweight glTF (assets/pine_tree/
    // fir_tree_01_lt.gltf) - a single-tree variant of the source stand with 1K
    // textures (4K is overkill for instanced trees) and a trimmed 25MB bin
    // instead of 456MB. The source 4K glTF is an 8.7M-vertex / 12x4K-material
    // 3-tree stand that takes ~87s to import (Assimp builds every vertex),
    // which blows the init budget; the lightweight variant imports in ~8s and
    // the engine's 4k-tri DecimateStaticMesh trims render cost at load. Falls
    // back to the full 4K glTF (slow but visually complete), then the
    // procedural OBJ, then the quiver_tree glTF, so the tree scatter can never
    // go missing if a glTF is absent. The procedural tree is only grown when no
    // glTF is present (it is a load-time + disk side-effect, so skip it
    // entirely when a glTF is present).
    const std::string lightPine  = assetDir + "../pine_tree/fir_tree_01_lt.gltf";
    const std::string fullPine   = assetDir + "../pine_tree/fir_tree_01_4k.gltf";
    std::string pinePath;
    if (std::filesystem::exists(lightPine)) {
        pinePath = lightPine;
        std::cout << "[WorldObjectManager] Using lightweight glTF pine (1K textures, single tree)\n";
    } else if (std::filesystem::exists(fullPine)) {
        pinePath = fullPine;
        std::cout << "[WorldObjectManager] Using full-res glTF pine (slow import)\n";
    } else {
        std::string pineObj = generateProceduralPine(assetDir);
        pinePath = pineObj.empty() ? (assetDir + "../quiver_tree/q.gltf") : pineObj;
    }

    // Real assets. The trees/boulders are glTF models living next to the
    // World_objects dir (quiver_tree/q.gltf, boulder/BOULDER.gltf); the small
    // plant glTF sets cover bushes, grass and flower patches. Rock FBXs stay
    // local to World_objects. The 60MB grass/flowers FBX files are NOT used -
    // the glTF variants load in milliseconds.
    m_configs[WorldObjectType::TREE_PINE] = {
        pinePath, 0.8f, 1.5f, {15.0f, 40.0f, 80.0f}
    };
    m_configs[WorldObjectType::TREE_OAK] = {
        pinePath, 0.9f, 1.3f, {20.0f, 50.0f, 100.0f}
    };
    m_configs[WorldObjectType::TREE_BIRCH] = {
        pinePath, 0.85f, 1.2f, {15.0f, 45.0f, 90.0f}
    };

    // Rocks / boulders
    m_configs[WorldObjectType::ROCK_BOULDER] = {
        assetDir + "../boulder/BOULDER.gltf", 0.5f, 2.0f, {10.0f, 30.0f, 60.0f}
    };
    m_configs[WorldObjectType::ROCK_STONE] = {
        assetDir + "stone.fbx", 0.3f, 0.8f, {8.0f, 20.0f, 40.0f}
    };
    m_configs[WorldObjectType::ROCK_CLIFF] = {
        assetDir + "Rock1.fbx", 1.0f, 3.0f, {30.0f, 80.0f, 150.0f}
    };

    // Vegetation - real glTF plants (tiny vs. the 60MB grass/flowers FBX).
    m_configs[WorldObjectType::GRASS_CLUSTER] = {
        assetDir + "../grass/grass.gltf", 0.6f, 1.0f, {5.0f, 15.0f, 30.0f}
    };
    m_configs[WorldObjectType::FLOWER_PATCH] = {
        assetDir + "../periwinkle/periwinkle_plant_4k.gltf", 0.5f, 0.8f, {5.0f, 12.0f, 25.0f}
    };
    m_configs[WorldObjectType::BUSH] = {
        assetDir + "../othonna/othonna.gltf", 0.7f, 1.2f, {10.0f, 25.0f, 50.0f}
    };

    // Props
    // LOG prop: rocky terrain stump (path was previously doubled into
    // World_objects/assets/boulder/, which never existed -> silent fallback).
    m_configs[WorldObjectType::LOG] = {
        assetDir + "../boulder/rocky_terrain.fbx", 0.0f, 0.0f, {15.0f, 40.0f, 80.0f}
    };
    m_configs[WorldObjectType::STUMP] = {
        assetDir + "Rock3.fbx", 0.6f, 1.0f, {10.0f, 30.0f, 60.0f}
    };
    
    // Load all models, normalizing each to its reference height so the
    // vegetation placement scales (heights in meters) render at sane sizes
    // no matter how big the raw asset is. Ground plants (grass/flower/bush)
    // are planted in large numbers, so they load with a small per-mesh
    // triangle budget (they're low and far away); trees keep a tight budget
    // (the pine glTF meshes decimate from ~6.7M verts to a few thousand, but
    // 625 instances still draw ~17M triangles at the 12k prop budget, so cap
    // it for render speed); other rocks/props keep the full budget.
    m_modelBaseScale.clear();
    for (const auto& [type, config] : m_configs) {
        // A (minScale==0, maxScale==0) range is the disabled sentinel: the
        // object type is registered for completeness (e.g. LOG/stump has no
        // available asset in this build) but loads no model and therefore
        // placeObject() skips it - no instance, no physics body.
        if (config.minScale == 0.0f && config.maxScale == 0.0f) {
            std::cout << "  [Disabled] " << config.modelPath
                      << " (0..0 scale range - not loaded)\n";
            continue;
        }
        const size_t budget =
            (type == WorldObjectType::GRASS_CLUSTER ||
             type == WorldObjectType::FLOWER_PATCH ||
             type == WorldObjectType::BUSH)
                ? 600u
            : (type == WorldObjectType::TREE_PINE ||
               type == WorldObjectType::TREE_OAK ||
               type == WorldObjectType::TREE_BIRCH)
                ? 4000u
                : 12000u;
        int modelId = m_renderer.loadModel(config.modelPath, budget);
        if (modelId >= 0) {
            m_modelIds[type] = modelId;
            const float modelH = m_renderer.getModelSize(modelId).y;
            m_modelBaseScale[type] = referenceHeight(type) / modelH;
            std::cout << "  Loaded " << config.modelPath << " (ID: " << modelId
                      << ", modelH=" << modelH << "m, baseScale="
                      << m_modelBaseScale[type] << ")\n";
        } else {
            std::cout << "  [Optional] " << config.modelPath << " (not found, will use fallback)\n";
        }
    }
    
    // Wind sway per type: low plants bend in the breeze (strong on grass,
    // softer on flowers/bushes), trees sway a little at the crown, rocks stay
    // put. The vertex shader scales amplitude by local height, so a large
    // strength on a short plant reads as natural tip-sway.
    if (auto it = m_modelIds.find(WorldObjectType::GRASS_CLUSTER); it != m_modelIds.end())
        m_renderer.setWindStrength(it->second, 0.5f);
    if (auto it = m_modelIds.find(WorldObjectType::FLOWER_PATCH); it != m_modelIds.end())
        m_renderer.setWindStrength(it->second, 0.35f);
    if (auto it = m_modelIds.find(WorldObjectType::BUSH); it != m_modelIds.end())
        m_renderer.setWindStrength(it->second, 0.3f);
    if (auto it = m_modelIds.find(WorldObjectType::TREE_PINE); it != m_modelIds.end())
        m_renderer.setWindStrength(it->second, 0.0f);
    if (auto it = m_modelIds.find(WorldObjectType::TREE_OAK); it != m_modelIds.end())
        m_renderer.setWindStrength(it->second, 0.0f);
    if (auto it = m_modelIds.find(WorldObjectType::TREE_BIRCH); it != m_modelIds.end())
        m_renderer.setWindStrength(it->second, 0.0f);
    // Rocks/logs/stumps keep the default 0 (no sway).

    m_initialized = true;
    std::cout << "[WorldObjectManager] Initialized with " << m_modelIds.size() 
              << " object types\n";
}

int WorldObjectManager::loadObject(WorldObjectType type, const std::string& modelPath,
                                    float minScale, float maxScale) {
    WorldObjectConfig config;
    config.modelPath = modelPath;
    config.minScale = minScale;
    config.maxScale = maxScale;
    
    m_configs[type] = config;
    
    int modelId = m_renderer.loadModel(modelPath);
    if (modelId >= 0) {
        m_modelIds[type] = modelId;
    }
    
    return modelId;
}

void WorldObjectManager::placeObject(WorldObjectType type, const glm::vec3& position,
                                      float scale, float rotationY,
                                      const glm::vec3& colorTint) {
    if (!m_initialized) {
        std::cerr << "[WorldObjectManager] Not initialized!\n";
        return;
    }
    
    auto it = m_modelIds.find(type);
    if (it == m_modelIds.end()) {
        // Model not loaded, skip silently (use fallback polygons)
        return;
    }
    
    // Use random scale if not specified
    if (scale <= 0.0f) {
        scale = randomScale(type);
    }

    // Normalize: placement scales are meters-referenced; the raw model may be
    // any size, so convert via the per-type base scale.
    auto bs = m_modelBaseScale.find(type);
    if (bs != m_modelBaseScale.end() && bs->second > 0.0f) scale *= bs->second;
    
    m_renderer.addInstance(it->second, position, scale, rotationY,
                           glm::vec4(colorTint, 1.0f));
    registerStaticCollider(type, position, scale, rotationY);
}

void WorldObjectManager::registerStaticCollider(WorldObjectType type,
                                                const glm::vec3& position,
                                                float instanceScale, float rotationY) {
    if (!m_physicsWorld) return;

    // Walk-through ground cover: grass/flowers/bushes never register bodies.
    switch (type) {
        case WorldObjectType::GRASS_CLUSTER:
        case WorldObjectType::FLOWER_PATCH:
        case WorldObjectType::BUSH:
            return;
        default:
            break;
    }
    auto it = m_modelIds.find(type);
    if (it == m_modelIds.end()) return;   // unloaded model -> nothing visible
    const BoundingBox bb = m_renderer.getModelBounds(it->second);
    if (!bb.IsValid() || instanceScale <= 0.0f) return;

    // The instance matrix anchors the model ORIGIN at `position` and scales
    // uniformly (plus a Y rotation). The visible mesh's world-space box is
    // the raw bbox transformed the same way - so the collider sits on the
    // VISUAL mesh: its center is the (rotated) visual center, NOT the raw
    // bbox center (stone.fbx is authored ~10m from its local origin; a
    // collider at position + rawCenter would float on the far side).
    const glm::vec3 cRaw = bb.Center() * instanceScale;
    const float cr = glm::radians(rotationY);
    const float cs = std::cos(cr), sn = std::sin(cr);
    glm::vec3 center = position + glm::vec3(cRaw.x * cs - cRaw.z * sn,
                                            cRaw.y,
                                            cRaw.x * sn + cRaw.z * cs);
    glm::vec3 half = bb.Extent() * instanceScale;

    // Clamp the footprint to the visual height: a squat rock's raw bbox can
    // be many times wider than it is tall, and a full-width box becomes an
    // invisible wall the character hits meters before touching the rock.
    const float maxHalfXZ = std::max(half.y * 1.5f, 0.25f);
    half.x = std::min(half.x, maxHalfXZ);
    half.z = std::min(half.z, maxHalfXZ);
    if (half.x < 0.05f || half.y < 0.05f || half.z < 0.05f) return;

    RigidBody body;
    body.position = center;
    body.scale = half * 2.0f;   // PhysicsWorld treats scale as FULL box size
    body.isStatic = true;
    body.colliderType = ColliderType::BOX;
    body.mass = 0.0f;
    body.friction = 0.8f;
    m_bodyHandles.push_back(m_physicsWorld->addBody(body));
}

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

void WorldObjectManager::update(const glm::vec3& cameraPos, float dt) {
    if (!m_initialized) return;
    
    // Cull distant objects. 120m keeps the visible play area populated while
    // cutting most of the world-object draw cost for horizontal cameras (the
    // world spans 300m; combined with load-time mesh decimation this keeps
    // all camera modes comfortably at 60fps).
    //
    // The cull is NON-destructive: instances beyond the radius are parked in
    // a dormant cache, and cullDistant also respawns (cache-hit reactivates)
    // any instance the camera has come back within ~85% of the radius of - so
    // objects behind/around the camera or left behind at high speed reappear
    // the moment you approach them again instead of staying gone forever.
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
    // Remove every collider this manager registered, then wipe the active
    // instances AND the dormant respawn cache (the old hack of culling from a
    // far-away point only emptied the active list and leaked the cached
    // instances).
    if (m_physicsWorld) {
        // Handles were registered in ascending index order and PhysicsWorld
        // removes by SWAP-with-back, which invalidates every handle after the
        // first removal. Removing back-to-front makes each remove a tail pop
        // (no element moves, no stale handles), so all bodies go away.
        for (auto it = m_bodyHandles.rbegin(); it != m_bodyHandles.rend(); ++it)
            m_physicsWorld->removeBody(*it);
        m_bodyHandles.clear();
    }
    m_renderer.clearAll();
}

size_t WorldObjectManager::getObjectCount() const {
    return m_renderer.getInstanceCount();
}

float WorldObjectManager::randomScale(WorldObjectType type) const {
    auto it = m_configs.find(type);
    if (it == m_configs.end()) return 1.0f;
    
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(it->second.minScale, 
                                                it->second.maxScale);
    return dist(gen);
}

float WorldObjectManager::referenceHeight(WorldObjectType type) const {
    switch (type) {
        case WorldObjectType::TREE_PINE:
        case WorldObjectType::TREE_OAK:
        case WorldObjectType::TREE_BIRCH:
            return 75.0f;              // placement 1.0 == 5m tree
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
