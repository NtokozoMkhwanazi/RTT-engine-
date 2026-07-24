#pragma once
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include "config.h"

class Terrain;
class VegetationSystem;
class WorldObjectManager;
class PhysicsWorld;
struct Floor;

namespace World {
    
    // World statistics for profiling
    struct WorldStats {
        size_t activeTerrainChunks = 0;
        size_t totalTreeInstances = 0;
        size_t totalRockInstances = 0;
        size_t activeWorldObjects = 0;
        float terrainUpdateTime = 0.0f;
        float terrainRenderTime = 0.0f;
        float vegetationUpdateTime = 0.0f;
        float vegetationRenderTime = 0.0f;
    };
    
    // World manager - handles terrain, vegetation, and world objects
    class WorldManager {
    public:
        static WorldManager& getInstance();
        
        ~WorldManager();
        
        // Initialize with configuration
        bool initialize(const Config::TerrainConfig& terrainConfig,
                       const Config::VegetationConfig& vegConfig);
        
        // Shutdown and cleanup
        void shutdown();
        
        // Update world systems
        void update(const glm::vec3& cameraPosition, float dt);
        
        // Render world systems
        void render(const glm::mat4& view, const glm::mat4& projection,
                   const glm::vec3& cameraPosition);
        
        // Terrain access
        Terrain* getTerrain() { return m_terrain.get(); }
        const Terrain* getTerrain() const { return m_terrain.get(); }
        
        // Vegetation access
        VegetationSystem* getVegetation() { return m_vegetation.get(); }
        const VegetationSystem* getVegetation() const { return m_vegetation.get(); }
        
        // World objects access
        WorldObjectManager* getWorldObjectManager() { return m_worldObjects.get(); }
        const WorldObjectManager* getWorldObjectManager() const { return m_worldObjects.get(); }
        
        // Physics access
        PhysicsWorld* getPhysicsWorld() { return m_physicsWorld.get(); }
        Floor* getFloor() { return m_floor.get(); }
        
        // Height queries
        float getHeightAt(float worldX, float worldZ) const;
        glm::vec3 getNormalAt(float worldX, float worldZ) const;
        
        // Check if terrain is initialized
        bool isTerrainInitialized() const { return m_terrainInitialized; }
        
        // Get world statistics
        const WorldStats& getStats() const { return m_stats; }
        
        // Performance optimization settings
        void setLODEnabled(bool enabled) { m_enableLOD = enabled; }
        void setCullingEnabled(bool enabled) { m_enableCulling = enabled; }
        void setDrawDistance(float distance) { m_drawDistance = distance; }
        
        bool isLODEnabled() const { return m_enableLOD; }
        bool isCullingEnabled() const { return m_enableCulling; }
        float getDrawDistance() const { return m_drawDistance; }
        
    private:
        WorldManager();
        
        // World systems
        std::unique_ptr<Terrain> m_terrain;
        std::unique_ptr<VegetationSystem> m_vegetation;
        std::unique_ptr<WorldObjectManager> m_worldObjects;
        std::unique_ptr<PhysicsWorld> m_physicsWorld;
        std::unique_ptr<Floor> m_floor;
        
        // State
        bool m_terrainInitialized = false;
        bool m_enableLOD = true;
        bool m_enableCulling = true;
        float m_drawDistance = 500.0f;
        
        // Statistics
        WorldStats m_stats;
    };
    
    // Helper function
    inline WorldManager& getWorldManager() {
        return WorldManager::getInstance();
    }
}