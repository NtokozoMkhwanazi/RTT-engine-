#pragma once
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include "config.h"
#include "lighting/LightingEnvironment.h"  // #3: world render thread env, not Instance()
#include "arena.h"

class Terrain;
class VegetationSystem;
class WorldObjectManager;
class PhysicsWorld;
class ShadowMapper;
struct Floor;

namespace World {
    
    // Simple NPC/enemy tracker — supports intent matrix scoring and
    // line-of-sight filtering for combat targeting.
    struct NPC {
        glm::vec3 position{0.0f};
        float health = 100.0f;
        bool isEnemy = true;
        bool alive = true;
        float radius = 0.5f;  // collision radius for LOS checks
        int id = 0;
    };
       
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
        
        // Render world systems.
        // #3: `lighting` is threaded from the frame owner (EditorApplication)
        // instead of each renderer calling LightingEnvironment::Instance().
        void render(const glm::mat4& view, const glm::mat4& projection,
                   const glm::vec3& cameraPosition,
                   ShadowMapper* shadow = nullptr,   // forwarded to terrain (CSM receive)
                   const LightingEnvironment& lighting = LightingEnvironment::Instance());
        
        // Terrain access
        Terrain* getTerrain() { return m_terrain.get(); }
        const Terrain* getTerrain() const { return m_terrain.get(); }
        
        // Vegetation access
        VegetationSystem* getVegetation() { return m_vegetation.get(); }
        const VegetationSystem* getVegetation() const { return m_vegetation.get(); }
        
        // World objects access
        WorldObjectManager* getWorldObjectManager() { return m_worldObjects.get(); }
        const WorldObjectManager* getWorldObjectManager() const { return m_worldObjects.get(); }

        // Update()-ownership handoff: when true, WorldManager::update() skips
        // the Terrain::update / WorldObjectManager::update calls and lets the
        // ECS adapter systems (TerrainSystem / WorldObjectSystem) drive them
        // instead. Defaults to false so every entry path keeps the original
        // behavior unchanged; only the bin/engine play path opts in. The flag
        // is only meaningful when the modules have actually been instantiated
        // (i.e. a non-arena scene — in the default arena scene both are null
        // and the adapters are no-ops either way).
        void setEcsOwnsTerrainObjects(bool enabled) { m_ecsOwnsTerrainObjects = enabled; }
        bool ecsOwnsTerrainObjects() const { return m_ecsOwnsTerrainObjects; }
        
        // -----------------------------------------------------------------
        // Arena mode — the default engine start scene.
        // When enabled, the WorldManager skips terrain / vegetation / world-
        // object rendering and instead draws the procedural Arena test stage
        // (cube walls + ground platform) as the priority renderer. This keeps
        // the arena in the proper WorldManager render pipeline (not a separate
        // ad-hoc renderer in the entry point) while keeping clear separation:
        // Arena owns its geometry + shader; WorldManager owns when/where it
        // draws.
        // -----------------------------------------------------------------
        void setArenaMode(bool enabled);
        bool isArenaMode() const { return m_arenaMode; }
        Arena::ArenaScene& getArena() { return *m_arena; }
        const Arena::ArenaScene& getArena() const { return *m_arena; }
        
        // Physics access
        PhysicsWorld* getPhysicsWorld() { return m_physicsWorld.get(); }
        Floor* getFloor() { return m_floor.get(); }
        
        // Height queries
        float getHeightAt(float worldX, float worldZ) const;
        glm::vec3 getNormalAt(float worldX, float worldZ) const;

        // Surface height INCLUDING static world-object colliders: the max of
        // the terrain and any boulder/rock/trunk top at (worldX, worldZ), so
        // the character can stand ON rocks just like terrain. Returns terrain
        // height when no solid object covers the point.
        float getSurfaceHeightAt(float worldX, float worldZ) const;

        // Resolve the play-mode character's capsule against the static
        // world-object colliders (boulders/rocks/trees): pushes feetPos out of
        // any overlap and zeroes the velocity component heading into the
        // surface. `grounded` is forwarded to PhysicsWorld's new velocity-
        // constraint ground clamp (snapCharacterToGround): a grounded
        // character that was level-placed/spawned high above the terrain is
        // settled onto the surface (no "sitting above the terrain"), while an
        // airborne character is left to land naturally. Returns true if a
        // contact was resolved.
        bool resolveCharacterCollision(glm::vec3& feetPos, float radius,
                                       float totalHeight, glm::vec3& velocity,
                                       bool grounded = true);

        // ---- NPC / enemy targeting ----
        // Spawn an NPC (enemy or ally) at the given world position.
        // Returns the NPC's unique ID for later reference.
        int spawnNPC(const glm::vec3& pos, bool isEnemy = true, float health = 100.0f);
        // Remove all dead / out-of-range NPCs.
        void pruneNPCs();
        // Get live NPC count (for diagnostics).
        size_t npcCount() const { return m_npcs.size(); }
        const std::vector<NPC>& npcs() const { return m_npcs; }

        // Intent Matrix Scoring + Line-of-Sight Filtering target selection.
        // Finds the best enemy target near `from` within `maxDist` in the
        // cone around `forwardDir`. Scores by angle (40%), distance (30%),
        // and visibility/LOS (30%), then raycasts through physics to confirm
        // the target is not occluded by terrain/world objects.
        bool getNearestEnemy(const glm::vec3& from, const glm::vec3& forwardDir,
                             float maxDist, glm::vec3& outTargetPos,
                             float& outScore);
        
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
        
        // Arena test stage (active when m_arenaMode is true). Owned here so
        // the GL VAOs/shader are torn down with the WorldManager.
        std::unique_ptr<Arena::ArenaScene> m_arena;
        bool m_arenaMode = false;
        bool m_ecsOwnsTerrainObjects = false;   // see setEcsOwnsTerrainObjects
        
        // State
        bool m_terrainInitialized = false;
        bool m_enableLOD = true;
        bool m_enableCulling = true;
        float m_drawDistance = 500.0f;
        
        // NPC / enemy tracking for combat targeting
        std::vector<NPC> m_npcs;
        int m_npcNextId = 1;
        
        // Statistics
        WorldStats m_stats;
    };
    
    // Helper function
    inline WorldManager& getWorldManager() {
        return WorldManager::getInstance();
    }
}