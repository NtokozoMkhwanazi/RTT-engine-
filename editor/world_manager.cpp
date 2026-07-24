#include "world_manager.h"
#include "world/Terrain.h"
#include "world/VegetationSystem.h"
#include "world/WorldObjectManager.h"
#include "physicsSystem/Physics.h"
#include "physicsSystem/Floor.h"
#include <iostream>
#include <chrono>
#include <algorithm>

namespace World {

WorldManager& WorldManager::getInstance() {
    static WorldManager instance;
    return instance;
}

WorldManager::WorldManager() {
}

WorldManager::~WorldManager() {
    shutdown();
}

bool WorldManager::initialize(const Config::TerrainConfig& terrainConfig,
                             const Config::VegetationConfig& vegConfig) {
    std::cout << "[WorldManager] Initializing world systems..." << std::endl;

    // Initialize Physics
    m_physicsWorld = std::make_unique<PhysicsWorld>();
    m_floor = std::make_unique<Floor>(glm::vec3(0, 0, 0), glm::vec2(50.0f, 50.0f));

    // Initialize World Objects
    m_worldObjects = std::make_unique<WorldObjectManager>();
    m_worldObjects->initialize("./assets/World_objects/");

    // Initialize Terrain
    try {
        Terrain::TerrainConfig tc;
        tc.chunkSize = terrainConfig.chunkSize;
        tc.chunkResolution = terrainConfig.chunkResolution;
        tc.viewDistance = terrainConfig.viewDistance;
        tc.lodDistance = terrainConfig.lodDistance;
        tc.heightmapSize = terrainConfig.heightmapSize;
        tc.heightScale = terrainConfig.heightScale;
        
        m_terrain = std::make_unique<Terrain>(tc);
        m_terrain->initialize();
        m_terrainInitialized = true;
        std::cout << "[WorldManager] Terrain initialized." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[WorldManager] Failed to initialize terrain: " << e.what() << std::endl;
        m_terrainInitialized = false;
    }

    // Initialize Vegetation
    if (m_terrainInitialized) {
        VegetationSystem::VegetationConfig vc;
        vc.treeDensity = vegConfig.treeDensity;
        vc.grassDensity = vegConfig.grassDensity;
        vc.rockDensity = vegConfig.rockDensity;
        vc.minTreeHeight = vegConfig.minTreeHeight;
        vc.maxTreeHeight = vegConfig.maxTreeHeight;
        vc.maxTreesPerChunk = vegConfig.maxTreesPerChunk;
        vc.maxRocksPerChunk = vegConfig.maxRocksPerChunk;
        
        m_vegetation = std::make_unique<VegetationSystem>(vc);
        m_vegetation->clear();
        
        // Generate vegetation for initial chunks
        auto& tc = m_terrain->getConfig();
        for (int x = -tc.viewDistance; x <= tc.viewDistance; ++x) {
            for (int z = -tc.viewDistance; z <= tc.viewDistance; ++z) {
                m_vegetation->generateForChunk(x, z, tc.chunkSize, {}, tc.heightmapSize);
            }
        }
        
        // Transfer vegetation to WorldObjectManager for optimized rendering
        if (m_worldObjects) {
            for (const auto& tree : m_vegetation->getTrees()) {
                WorldObjectType type = WorldObjectType::TREE_PINE;
                if (tree.type == 1) type = WorldObjectType::TREE_OAK;
                else if (tree.type == 2) type = WorldObjectType::TREE_BIRCH;
                
                m_worldObjects->placeObject(type, tree.position, tree.height / 5.0f);
            }
            
            for (const auto& rock : m_vegetation->getRocks()) {
                WorldObjectType type = WorldObjectType::ROCK_BOULDER;
                if (rock.type == 1) type = WorldObjectType::ROCK_STONE;
                else if (rock.type == 2) type = WorldObjectType::ROCK_CLIFF;
                
                m_worldObjects->placeObject(type, rock.position, rock.scale.x, rock.rotation);
            }
        }
        
        std::cout << "[WorldManager] Vegetation initialized and transferred to WorldObjectManager." << std::endl;
    }
    
    std::cout << "[WorldManager] Initialization complete." << std::endl;
    return true;
}

void WorldManager::shutdown() {
    static bool alreadyShutdown = false;
    if (alreadyShutdown) return;
    alreadyShutdown = true;

    std::cout << "[WorldManager] Shutting down..." << std::endl;
    m_terrain.reset();
    m_vegetation.reset();
    m_worldObjects.reset();
    m_physicsWorld.reset();
    m_floor.reset();
    m_terrainInitialized = false;
}

void WorldManager::update(const glm::vec3& cameraPosition, float dt) {
    auto start = std::chrono::high_resolution_clock::now();

    if (m_terrainInitialized && m_terrain) {
        m_terrain->update(cameraPosition, dt);
    }
    
    auto terrainEnd = std::chrono::high_resolution_clock::now();
    m_stats.terrainUpdateTime = std::chrono::duration<float, std::milli>(terrainEnd - start).count();

    if (m_worldObjects) {
        m_worldObjects->update(cameraPosition, dt);
    }

    if (m_physicsWorld) {
        m_physicsWorld->step(dt);
    }

    // Update stats
    if (m_terrain) {
        m_stats.activeTerrainChunks = m_terrain->getActiveChunkCount();
    }
    
    if (m_vegetation) {
        m_stats.totalTreeInstances = m_vegetation->getTreeInstanceCount();
        m_stats.totalRockInstances = m_vegetation->getRockInstanceCount();
    }
    
    if (m_worldObjects) {
        m_stats.activeWorldObjects = m_worldObjects->getObjectCount();
    }
}

void WorldManager::render(const glm::mat4& view, const glm::mat4& projection,
                         const glm::vec3& cameraPosition) {
    auto start = std::chrono::high_resolution_clock::now();

    if (m_terrainInitialized && m_terrain) {
        m_terrain->render(view, projection, cameraPosition, 45.0f, 16.0f/9.0f, 0.1f, 2000.0f);
    }

    auto terrainEnd = std::chrono::high_resolution_clock::now();
    m_stats.terrainRenderTime = std::chrono::duration<float, std::milli>(terrainEnd - start).count();

    if (m_worldObjects) {
        m_worldObjects->render(view, projection, cameraPosition);
    }
}

float WorldManager::getHeightAt(float worldX, float worldZ) const {
    if (m_terrainInitialized && m_terrain) {
        return m_terrain->getHeightAt(worldX, worldZ);
    }
    return m_floor ? m_floor->position.y : 0.0f;
}

glm::vec3 WorldManager::getNormalAt(float worldX, float worldZ) const {
    if (m_terrainInitialized && m_terrain) {
        return m_terrain->getNormalAt(worldX, worldZ);
    }
    return glm::vec3(0, 1, 0);
}

} // namespace World
