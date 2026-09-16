#include "world_manager.h"
#include "world/Terrain.h"
#include "world/VegetationSystem.h"
#include "world/WorldObjectManager.h"
#include "physicsSystem/Physics.h"
#include "physicsSystem/Floor.h"
#include <iostream>
#include <chrono>
#include <algorithm>

// Authored displacement heightmap path. Defined alongside the other terrain
// asset paths in TerrainChunk.cpp. Declared here at GLOBAL scope (world_manager
// lives in namespace World) so unqualified lookup inside World resolves the
// symbol to the global definition — a block-scope extern would instead bind it
// to World::kTerrainHeightmapPath and fail to link.
extern const char* kTerrainHeightmapPath;

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

void WorldManager::setArenaMode(bool enabled) {
    if (m_arenaMode == enabled) return;  // no-op if unchanged
    m_arenaMode = enabled;
    std::cout << "[WorldManager] Arena mode "
              << (m_arenaMode ? "ENABLED (terrain disabled)" : "disabled")
              << "\n";
}

bool WorldManager::initialize(const Config::TerrainConfig& terrainConfig,
                             const Config::VegetationConfig& vegConfig) {
    std::cout << "[WorldManager] Initializing world systems..."
              << (m_arenaMode ? " [ARENA MODE]" : "") << std::endl;

    // Initialize Physics (always — needed for character collisions even
    // in arena mode: the flat floor + wall colliders keep the bot grounded).
    m_physicsWorld = std::make_unique<PhysicsWorld>();
    m_floor = std::make_unique<Floor>(glm::vec3(0, 0, 0), glm::vec2(50.0f, 50.0f));

    if (m_arenaMode) {
        // --- Arena mode: skip terrain / vegetation / world objects ---
        // The procedural Arena test stage replaces the heightmap world.
        // Physics stays alive so the character can collide with the arena
        // floor plane.
        m_arena = std::make_unique<Arena::ArenaScene>();
        if (!m_arena->initialize()) {
            std::cerr << "[WorldManager] Failed to initialize arena\n";
            return false;
        }
        std::cout << "[WorldManager] Arena initialized as default scene ("
                  << m_arena->wallCount() << " walls)\n";
        return true;
    }

    // Initialize World Objects
    m_worldObjects = std::make_unique<WorldObjectManager>();
    // Solid placed objects (boulders/rocks/trees/logs/stumps) register static
    // colliders in the physics world, so the play-mode character collides
    // with them instead of walking through (grass/flowers/bushes stay
    // walk-through). Must be wired BEFORE vegetation placement below.
    m_worldObjects->setPhysicsWorld(m_physicsWorld.get());
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
        // Wire the authored displacement heightmap into the GL_R32F master
        // texture so the terrain_splat VERTEX displacement (the visible
        // geometric displacement in bin/engine) is driven by an asset, not just
        // the procedural Perlin fallback. Only reached when arena mode is off
        // (the heightmap scene); the procedural path is used if the asset
        // fails to load on a given build.
        m_terrain->setHeightmapSource(kTerrainHeightmapPath);
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
        vc.pebbleDensity = vegConfig.pebbleDensity;
        vc.minTreeHeight = vegConfig.minTreeHeight;
        vc.maxTreeHeight = vegConfig.maxTreeHeight;
        vc.maxTreesPerChunk = vegConfig.maxTreesPerChunk;
        vc.maxRocksPerChunk = vegConfig.maxRocksPerChunk;
        vc.maxPebblesPerChunk = vegConfig.maxPebblesPerChunk;
        
        m_vegetation = std::make_unique<VegetationSystem>(vc);
        m_vegetation->clear();
        
        // Generate vegetation for initial chunks
        auto& tc = m_terrain->getConfig();
        for (int x = -tc.viewDistance; x <= tc.viewDistance; ++x) {
            for (int z = -tc.viewDistance; z <= tc.viewDistance; ++z) {
                m_vegetation->generateForChunk(x, z, tc.chunkSize, {}, tc.heightmapSize);
            }
        }
        
        // Transfer vegetation to WorldObjectManager for optimized rendering.
        // The vegetation generator is fed an EMPTY heightmap, so its positions
        // are y=0 - snap every placement onto the real terrain surface or the
        // objects spawn buried under 2-25m of terrain (invisible).
        if (m_worldObjects) {
            const auto snapToTerrain = [&](const glm::vec3& p) -> glm::vec3 {
                glm::vec3 pos = p;
                if (m_terrainInitialized && m_terrain) pos.y = m_terrain->getHeightAt(p.x, p.z);
                return pos;
            };

            for (const auto& tree : m_vegetation->getTrees()) {
                WorldObjectType type = WorldObjectType::TREE_PINE;
                if (tree.type == 1) type = WorldObjectType::TREE_OAK;
                else if (tree.type == 2) type = WorldObjectType::TREE_BIRCH;
                
                m_worldObjects->placeObject(type, snapToTerrain(tree.position), tree.height / 5.0f);
            }
            
            for (const auto& rock : m_vegetation->getRocks()) {
                WorldObjectType type = WorldObjectType::ROCK_BOULDER;
                if (rock.type == 1) type = WorldObjectType::ROCK_STONE;
                else if (rock.type == 2) type = WorldObjectType::ROCK_CLIFF;
                
                m_worldObjects->placeObject(type, snapToTerrain(rock.position), rock.scale.x, rock.rotation);
            }
            // Pebbles (tiny ground stones) reuse the stone model at pebble
            // scale - placed separately so rock counts stay bounded by
            // maxRocksPerChunk while ground scatter is still authored.
            for (const auto& pebble : m_vegetation->getPebbles()) {
                m_worldObjects->placeObject(WorldObjectType::ROCK_STONE,
                                            snapToTerrain(pebble.position),
                                            pebble.scale.x, pebble.rotation);
            }

            // Ground plants: grass clusters, flower patches (periwinkle /
            // celandine look-alike) and desert succulents (othonna). Their
            // models carry real textures, so each plant gets its matching
            // WorldObjectType below.
            for (const auto& plant : m_vegetation->getPlants()) {
                WorldObjectType type = WorldObjectType::GRASS_CLUSTER;
                if (plant.type == 1) type = WorldObjectType::FLOWER_PATCH;
                else if (plant.type == 2) type = WorldObjectType::BUSH;
                m_worldObjects->placeObject(type, snapToTerrain(plant.position),
                                            plant.scale, plant.rotation, plant.tint);
            }

            std::cout << "[WorldManager] Vegetation placed on terrain: "
                      << m_vegetation->getTrees().size() << " trees, "
                      << m_vegetation->getRocks().size() << " rocks, "
                      << m_vegetation->getPebbles().size() << " pebbles, "
                      << m_vegetation->getPlants().size() << " plants ("
                      << m_worldObjects->getObjectCount() << " instances)\n";
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
    m_arena.reset();  // GL context may already be torn down in static-destruction path
    m_terrain.reset();
    m_vegetation.reset();
    m_worldObjects.reset();
    m_physicsWorld.reset();
    m_floor.reset();
    m_terrainInitialized = false;
}

void WorldManager::update(const glm::vec3& cameraPosition, float dt) {
    auto start = std::chrono::high_resolution_clock::now();

    if (m_arenaMode) {
        // In arena mode there is no streaming terrain or world objects to
        // update — just step the physics (character capsules, etc.).
        if (m_physicsWorld) {
            m_physicsWorld->step(dt);
        }
        return;
    }

    // Update() ownership of Terrain + WorldObjectManager is handed off to the
    // ECS adapter systems (TerrainSystem / WorldObjectSystem) in the bin/engine
    // play path. Only update them here when the ECS layer hasn't taken over —
    // otherwise the ECS system is the single updater (no double-update).
    if (!m_ecsOwnsTerrainObjects && m_terrainInitialized && m_terrain) {
        m_terrain->update(cameraPosition, dt);
    }
    
    auto terrainEnd = std::chrono::high_resolution_clock::now();
    m_stats.terrainUpdateTime = std::chrono::duration<float, std::milli>(terrainEnd - start).count();

    if (!m_ecsOwnsTerrainObjects && m_worldObjects) {
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
                         const glm::vec3& cameraPosition, ShadowMapper* shadow,
                         const LightingEnvironment& lighting) {
    auto start = std::chrono::high_resolution_clock::now();

    if (m_arenaMode && m_arena) {
        // Arena mode: render the procedural test stage (cube walls + ground).
        // The skybox is already drawn inside renderScene() before we get here,
        // so the arena walls sit on top of it — exactly the blank-sky
        // "empty level" look.
        m_arena->render(view, projection, cameraPosition);
        return;
    }

    if (m_terrainInitialized && m_terrain) {
        // Propagate the editor's culling toggle (World Settings → Frustum Culling)
        // so the checkbox immediately takes effect on terrain chunks.
        if (m_terrain->isFrustumCullingEnabled() != m_enableCulling) {
            m_terrain->setFrustumCulling(m_enableCulling);
        }
        m_terrain->render(view, projection, cameraPosition, 45.0f, 16.0f/9.0f, 0.1f, 2000.0f, shadow, lighting);
    }

    auto terrainEnd = std::chrono::high_resolution_clock::now();
    m_stats.terrainRenderTime = std::chrono::duration<float, std::milli>(terrainEnd - start).count();

    if (m_worldObjects) {
        m_worldObjects->setFrustumCulling(m_enableCulling);
        m_worldObjects->render(view, projection, cameraPosition, lighting);
    }
}

float WorldManager::getHeightAt(float worldX, float worldZ) const {
    if (m_arenaMode) {
        // Arena floor sits at y ≈ 0.
        return 0.0f;
    }
    if (m_terrainInitialized && m_terrain) {
        return m_terrain->getHeightAt(worldX, worldZ);
    }
    return m_floor ? m_floor->position.y : 0.0f;
}

float WorldManager::getSurfaceHeightAt(float worldX, float worldZ) const {
    if (m_arenaMode) {
        // Arena floor at y=0; no static world-object colliders to check.
        return 0.0f;
    }
    float h = getHeightAt(worldX, worldZ);
    if (m_physicsWorld) {
        const float rockTop = m_physicsWorld->getStaticSurfaceHeightAt(worldX, worldZ);
        if (rockTop > h) h = rockTop;
    }
    return h;
}

bool WorldManager::resolveCharacterCollision(glm::vec3& feetPos, float radius,
                                             float totalHeight, glm::vec3& velocity,
                                             bool grounded) {
    if (!m_physicsWorld) return false;
    // Authoritative ground height under the feet, heightmap-aware: max of the
    // terrain heightmap (getHeightAt -> m_terrain) and static world-object
    // (tree/rock/boulder) tops (getStaticSurfaceHeightAt). Passed INTO
    // snapCharacterToGround so the solver never sinks a character perched on a
    // real heightmap down to the flat PhysicsWorld floor plane (y=0). A
    // grounded bot level-placed (or spawned) high above the terrain is settled
    // back onto this surface; an airborne (jumping) bot is only clamped at
    // actual contact.
    const float surfaceY = getSurfaceHeightAt(feetPos.x, feetPos.z);
    m_physicsWorld->snapCharacterToGround(feetPos, radius, totalHeight, velocity,
                                          surfaceY, grounded);
    return m_physicsWorld->resolveCharacterCapsule(feetPos, radius, totalHeight, velocity);
}

glm::vec3 WorldManager::getNormalAt(float worldX, float worldZ) const {
    if (m_arenaMode) {
        return glm::vec3(0, 1, 0);  // flat arena floor
    }
    if (m_terrainInitialized && m_terrain) {
        return m_terrain->getNormalAt(worldX, worldZ);
    }
    return glm::vec3(0, 1, 0);
}

// ---- NPC / enemy targeting ----

int WorldManager::spawnNPC(const glm::vec3& pos, bool isEnemy, float health) {
    NPC npc;
    npc.position = pos;
    npc.isEnemy = isEnemy;
    npc.health = health;
    npc.alive = true;
    npc.id = m_npcNextId++;
    m_npcs.push_back(npc);
    std::cout << "[WorldManager] Spawned NPC id=" << npc.id
              << (isEnemy ? " (enemy)" : " (ally)")
              << " at (" << pos.x << "," << pos.y << "," << pos.z << ")\n";
    return npc.id;
}

void WorldManager::pruneNPCs() {
    m_npcs.erase(
        std::remove_if(m_npcs.begin(), m_npcs.end(),
            [](const NPC& n) { return !n.alive || n.health <= 0.0f; }),
        m_npcs.end());
}

bool WorldManager::getNearestEnemy(const glm::vec3& from,
                                    const glm::vec3& forwardDir,
                                    float maxDist,
                                    glm::vec3& outTargetPos,
                                    float& outScore) {
    if (m_npcs.empty() || !m_physicsWorld) return false;

    float bestScore = -1.0f;
    const NPC* bestNPC = nullptr;

    // Phase 1: Intent Matrix Scoring
    // Multi-factor evaluation: angle (40%), distance (30%), height diff (15%),
    // alive/health (15%)
    for (const auto& npc : m_npcs) {
        if (!npc.isEnemy || !npc.alive) continue;

        glm::vec3 toNPC = npc.position - from;
        float dist = glm::length(toNPC);
        if (dist > maxDist || dist < 0.01f) continue;

        glm::vec3 dirToNPC = toNPC / dist;
        float angleRad = std::acos(glm::clamp(
            glm::dot(glm::normalize(forwardDir), dirToNPC), -1.0f, 1.0f));
        // Angle score: 1.0 when facing directly, 0.0 at 90°+
        float angleScore = 1.0f - (angleRad / glm::half_pi<float>());
        if (angleScore < 0.0f) angleScore = 0.0f;

        // Distance score: closer = higher (inverse, normalized to maxDist)
        float distScore = 1.0f - (dist / maxDist);

        // Height difference score: prefer targets at similar height
        float heightDiff = std::abs(npc.position.y - from.y);
        float heightScore = 1.0f - (heightDiff / 5.0f);
        if (heightScore < 0.0f) heightScore = 0.0f;

        // Health score: prefer damaged targets (finishers)
        float healthScore = 1.0f - (npc.health / 100.0f);

        // Weighted intent score: angle 40%, distance 30%, height 15%, health 15%
        float intentScore =
            angleScore * 0.40f +
            distScore * 0.30f +
            heightScore * 0.15f +
            healthScore * 0.15f;

        // Phase 2: Line-of-Sight Filtering (raycast through physics)
        bool hasLOS = true;
        glm::vec3 hitPoint, hitNormal;
        std::shared_ptr<RigidBody> hitBody;
        glm::vec3 rayDir = glm::normalize(toNPC);
        if (m_physicsWorld->raycast(from + glm::vec3(0, 1.5f, 0), rayDir,
                                     dist * 0.95f,  // don't hit self
                                     hitPoint, hitNormal, hitBody)) {
            // Ray hit something before reaching the NPC → occluded
            hasLOS = false;
        }

        // LOS weight: 30% of final score
        float losBonus = hasLOS ? 1.0f : 0.0f;
        float finalScore = intentScore * 0.7f + losBonus * 0.3f;

        if (finalScore > bestScore) {
            bestScore = finalScore;
            bestNPC = &npc;
        }
    }

    if (!bestNPC || bestScore < 0.01f) return false;

    outTargetPos = bestNPC->position;
    outScore = bestScore;
    return true;
}

} // namespace World
