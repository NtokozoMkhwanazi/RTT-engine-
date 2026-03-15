#include "WorldObjectManager.h"
#include <iostream>
#include <random>

WorldObjectManager::WorldObjectManager() {
}

WorldObjectManager::~WorldObjectManager() {
}

void WorldObjectManager::initialize(const std::string& assetDir) {
    std::cout << "[WorldObjectManager] Initializing...\n";

    // Define default object configurations
    // Trees
    m_configs[WorldObjectType::TREE_PINE] = {
        assetDir + "pine_tree.fbx", 0.8f, 1.5f, {15.0f, 40.0f, 80.0f}
    };
    m_configs[WorldObjectType::TREE_OAK] = {
        assetDir + "oak_tree.fbx", 0.9f, 1.3f, {20.0f, 50.0f, 100.0f}
    };
    m_configs[WorldObjectType::TREE_BIRCH] = {
        assetDir + "birch_tree.fbx", 0.85f, 1.2f, {15.0f, 45.0f, 90.0f}
    };

    // Rocks
    m_configs[WorldObjectType::ROCK_BOULDER] = {
        assetDir + "Rock0.fbx", 0.5f, 2.0f, {10.0f, 30.0f, 60.0f}
    };
    m_configs[WorldObjectType::ROCK_STONE] = {
        assetDir + "stone.fbx", 0.3f, 0.8f, {8.0f, 20.0f, 40.0f}
    };
    m_configs[WorldObjectType::ROCK_CLIFF] = {
        assetDir + "Rock1.fbx", 1.0f, 3.0f, {30.0f, 80.0f, 150.0f}
    };


    // Vegetation - Use actual files from assets/World_objects/
    m_configs[WorldObjectType::GRASS_CLUSTER] = {
        assetDir + "grass.fbx", 0.6f, 1.0f, {5.0f, 15.0f, 30.0f}
    };
    m_configs[WorldObjectType::FLOWER_PATCH] = {
        assetDir + "flowers.fbx", 0.5f, 0.8f, {5.0f, 12.0f, 25.0f}
    };
    m_configs[WorldObjectType::BUSH] = {
        assetDir + "stone.fbx", 0.7f, 1.2f, {10.0f, 25.0f, 50.0f}
    };

    // Props - Bear and Datsun models
    m_configs[WorldObjectType::LOG] = {
        assetDir + "Bear_DEMO.fbx", 0.8f, 1.5f, {15.0f, 40.0f, 80.0f}
    };
    m_configs[WorldObjectType::STUMP] = {
        assetDir + "datsun.fbx", 0.6f, 1.0f, {10.0f, 30.0f, 60.0f}
    };
    
    // Load all models
    for (const auto& [type, config] : m_configs) {
        int modelId = m_renderer.loadModel(config.modelPath);
        if (modelId >= 0) {
            m_modelIds[type] = modelId;
            std::cout << "  Loaded " << config.modelPath << " (ID: " << modelId << ")\n";
        } else {
            std::cout << "  [Optional] " << config.modelPath << " (not found, will use fallback)\n";
        }
    }
    
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
    
    m_renderer.addInstance(it->second, position, scale, rotationY);
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
    
    // Cull distant objects - use larger distance
    float maxCullDistance = 300.0f;  // Increased from 150m
    m_renderer.cullDistant(cameraPos, maxCullDistance);
}

void WorldObjectManager::render(const glm::mat4& view, const glm::mat4& projection,
                                 const glm::vec3& cameraPos) {
    if (!m_initialized) return;
    
    m_renderer.render(view, projection);
}

void WorldObjectManager::clear() {
    m_renderer.cullDistant(glm::vec3(999999, 0, 0), 0.0f);  // Clear all
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
