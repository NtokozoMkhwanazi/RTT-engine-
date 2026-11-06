#pragma once
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include "config.h"
#include "lighting/LightingEnvironment.h"  // #3: world render thread env, not Instance()
#include "arena.h"
#include "shaderSystem/Shader.h"  // Shader* for shadow render callback

class Terrain;
class VegetationSystem;
class WorldObjectManager;
class PhysicsWorld;
class ShadowMapper;
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
        
        // Initialize with configuration (full boot path: terrain + props).
        bool initialize(const Config::TerrainConfig& terrainConfig,
                       const Config::VegetationConfig& vegConfig);

        // Load terrain ONLY — heightmap + splat textures + POM. Does NOT touch
        // world objects or vegetation. This is what the "Load Heightmap"
        // button calls. Alerts the artist (via return false + console) if the
        // heightmap path is missing or the asset doesn't exist.
        //
        // `preloadedHeightmap` / `preloadedSize`: optional height grid already
        // decoded off the main thread (Terrain::DecodeHeightmapFile). When
        // supplied, the terrain skips the blocking 4K file decode — used by the
        // editor's staged "Reload Complete Stage" job so the rebuild doesn't
        // freeze the viewport.
        bool initializeTerrain(const Config::TerrainConfig& terrainConfig,
                               const std::vector<float>* preloadedHeightmap = nullptr,
                               int preloadedSize = 0);

        // Rebuild world objects ONLY — loads props from the JSON config
        // (engine_config.json "world_objects" array) with NO hardcoded
        // defaults. Alerts the artist if the config is empty or an asset
        // path doesn't exist. Does NOT touch terrain or heightmap.
        //
        // This SYNCHRONOUS variant blocks the calling thread for the full
        // Assimp parse + GL upload — used by engine boot and the test path
        // (which expects props loaded before update()/placement runs).
        bool rebuildWorldObjects(const Config::VegetationConfig& vegConfig);

        // Non-blocking variant of rebuildWorldObjects: dispatches the heavy
        // Assimp parse + texture decode to worker threads and returns
        // immediately. Poll commitWorldObjectLoading() every frame (also pumped
        // automatically inside update()) until isWorldObjectLoadDone();
        // getWorldObjectLoadProgress() drives the UI "Resolving..." bar.
        bool rebuildWorldObjectsAsync(const Config::VegetationConfig& vegConfig);

        // Pump one main-thread (GL) commit step of the in-flight async prop
        // load. Safe to call every frame; no-ops when nothing is loading.
        float commitWorldObjectLoading();
        bool isWorldObjectLoading() const;
        bool isWorldObjectLoadDone() const;
        float getWorldObjectLoadProgress() const;
        void cancelWorldObjectLoading();

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
        
        // Depth-only render into the active shadow-map framebuffer (called by
        // RenderPipeline's shadow pass via the registered m_shadowRenderCallback).
        // Renders terrain chunks + world-object instances with the shadow shader
        // so they cast CSM shadows. `lightView` / `lightProj` are the light's
        // view and projection matrices for the current cascade.
        void renderForShadows(const glm::mat4& lightView,
                              const glm::mat4& lightProj,
                              Shader* shadowShader);
        
        // Terrain access
        Terrain* getTerrain() { return m_terrain.get(); }
        const Terrain* getTerrain() const { return m_terrain.get(); }
        
        // Vegetation access
        VegetationSystem* getVegetation() { return m_vegetation.get(); }
        const VegetationSystem* getVegetation() const { return m_vegetation.get(); }
        
        // World objects access
        WorldObjectManager* getWorldObjectManager() { return m_worldObjects.get(); }
        const WorldObjectManager* getWorldObjectManager() const { return m_worldObjects.get(); }
        
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

        // Ray-march a ray against the terrain heightfield; returns the first
        // crossing. (forUI.txt Pillar 2 foliage-brush raycasts.)
        bool raycastTerrain(const glm::vec3& origin, const glm::vec3& dir,
                            float maxDist, float& outDist, glm::vec3& outPoint) const;

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
        
        // Check if terrain is initialized
        bool isTerrainInitialized() const { return m_terrainInitialized; }
        
        // Get world statistics
        const WorldStats& getStats() const { return m_stats; }
        
        // Performance optimization settings
        void setLODEnabled(bool enabled) { m_enableLOD = enabled; }
        void setCullingEnabled(bool enabled) { m_enableCulling = enabled; }
        void setDrawDistance(float distance) { m_drawDistance = distance; }
        void setMaxVisibleInstances(int count) { m_maxVisibleInstances = count; }
        
        bool isLODEnabled() const { return m_enableLOD; }
        bool isCullingEnabled() const { return m_enableCulling; }
        float getDrawDistance() const { return m_drawDistance; }
        int getMaxVisibleInstances() const { return m_maxVisibleInstances; }
        
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
        
        void transferVegetationInstances();

        // Shared by rebuildWorldObjects() (sync) and rebuildWorldObjectsAsync()
        // (non-blocking): re-initialises the (empty) vegetation scatter pools so
        // the biome generator starts from a clean slate regardless of path.
        void initVegetationPools(const Config::VegetationConfig& vegConfig);

        // State
        bool m_terrainInitialized = false;
        bool m_enableLOD = true;
        bool m_enableCulling = true;
        float m_drawDistance = 500.0f;
        int m_maxVisibleInstances = 500;
        
        // Statistics
        WorldStats m_stats;
    };
    
    // Helper function
    inline WorldManager& getWorldManager() {
        return WorldManager::getInstance();
    }
}