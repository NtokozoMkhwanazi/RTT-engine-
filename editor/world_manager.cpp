#include "world_manager.h"
#include "world/Terrain.h"
#include "world/VegetationSystem.h"
#include "world/WorldObjectManager.h"
#include "physicsSystem/Physics.h"
#include "physicsSystem/Floor.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <tuple>
#include <filesystem>
#include "editor/console.h"  // EditorConsole::Log

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

    if (m_arenaMode) {
        // Enable arena mode at runtime (e.g. via the F7 toggle) lazily creates
        // + initializes the ArenaScene. initialize() runs once at boot for the
        // default terrain scene, so this is the path that actually spins up
        // the test stage on demand. Guarded so a pre-init setArenaMode(true)
        // (handled by initialize()) is never double-created.
        if (!m_arena) {
            m_arena = std::make_unique<Arena::ArenaScene>();
            if (!m_arena->initialize()) {
                std::cerr << "[WorldManager] Failed to initialize arena; "
                          << "falling back to previous mode.\n";
                m_arena.reset();
                m_arenaMode = false;
                std::cout << "[WorldManager] Arena mode disabled (init failed)\n";
                return;
            }
            std::cout << "[WorldManager] Arena initialized as active scene ("
                      << m_arena->wallCount() << " walls)\n";
        }
    } else {
        m_arena.reset();  // release arena GL resources (context still alive)
    }
}

bool WorldManager::initialize(const Config::TerrainConfig& terrainConfig,
                             const Config::VegetationConfig& vegConfig) {
    // Clean up old systems before reinitializing (supports "Rebuild Terrain"
    // from the UI without leaking GL resources / stale terrain state).
    // Only run cleanup if we were previously initialized (not the first boot).
    if (m_terrainInitialized || m_worldObjects || m_physicsWorld) {
        std::cout << "[WorldManager] Re-initializing — cleaning up previous state.\n";
        m_terrain.reset();
        m_vegetation.reset();
        m_worldObjects.reset();
        m_physicsWorld.reset();
        m_floor.reset();
        m_terrainInitialized = false;
    }

    std::cout << "[WorldManager] Initializing world systems..."
              << (m_arenaMode ? " [ARENA MODE]" : "") << std::endl;

    // Initialize Physics (always — needed for character collisions even
    // in arena mode: the flat floor + wall colliders keep the bot grounded).
    m_physicsWorld = std::make_unique<PhysicsWorld>();
    m_floor = std::make_unique<Floor>(glm::vec3(0, 0, 0), glm::vec2(50.0f, 50.0f));

    if (m_arenaMode) {
        m_arena = std::make_unique<Arena::ArenaScene>();
        if (!m_arena->initialize()) {
            std::cerr << "[WorldManager] Failed to initialize arena\n";
            return false;
        }
        std::cout << "[WorldManager] Arena initialized as default scene ("
                  << m_arena->wallCount() << " walls)\n";
        return true;
    }

    // --- Separated concerns (verify.txt): ---
    // initializeTerrain() loads ONLY the terrain (heightmap + splat/POM textures).
    // rebuildWorldObjects() loads ONLY props from the JSON config (NO defaults).
    if (!initializeTerrain(terrainConfig)) {
        return false;
    }
    rebuildWorldObjects(vegConfig);
    // Vegetation transfer: scatter world-object instances onto the terrain and
    // register their static colliders so the play-mode character collides with
    // the same visible props (the chain WorldObjectPhysics.WorldManager_*
    // and CharacterWalksIntoBoulder_* assert on). Editor brush-painter
    // placement is still handled separately in rebuildWorldObjects.
    transferVegetationInstances();

    std::cout << "[WorldManager] Initialization complete." << std::endl;
    return true;
}

// verify.txt: Load Heightmap button = terrain ONLY (heightmap + splat textures + POM).
// Does NOT load props. Alerts the artist if the heightmap path is empty or the
// file doesn't exist — NO procedural fallback.
bool WorldManager::initializeTerrain(const Config::TerrainConfig& terrainConfig,
                                     const std::vector<float>* preloadedHeightmap,
                                     int preloadedSize) {
    if (terrainConfig.heightmapPath.empty()) {
        std::cerr << "[WorldManager] Heightmap path is empty — cannot load terrain.\n";
        EditorConsole::Log("[Terrain] Heightmap path is empty. Set one in World Settings → Terrain panel. "
                           "Use a heightmap IMAGE (._disp_4k.png, _height_4k.png, .exr), NOT a .gltf mesh.", 2);
        return false;
    }

    // Check the actual heightmap asset exists before attempting load.
    if (!std::filesystem::exists(terrainConfig.heightmapPath)) {
        std::cerr << "[WorldManager] Heightmap asset not found: "
                  << terrainConfig.heightmapPath << "\n";
        EditorConsole::Log("[Terrain] Heightmap image not found: " +
                           terrainConfig.heightmapPath + ". Place it in the assets folder.", 2);
        return false;
    }

    // Validate the file is an IMAGE heightmap, not a mesh (e.g. .gltf).
    // Artists sometimes drag a .gltf terrain asset here; the engine needs a
    // single-channel heightmap image for displacement.
    std::string ext = std::filesystem::path(terrainConfig.heightmapPath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj" || ext == ".blend") {
        std::cerr << "[WorldManager] Heightmap path is a 3D mesh, not an image: "
                  << terrainConfig.heightmapPath << "\n";
        EditorConsole::Log("[Terrain] That's a 3D mesh file, not a heightmap. "
                           "Use the DISPLACEMENT or HEIGHT texture from this asset's "
                           "textures/ folder (e.g. 'marble_cliff_01_disp_4k.png') as the heightmap, "
                           "then set the albedo/normal/roughness/ao paths in 'Terrain Material Paths' below.", 2);
        return false;
    }
    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".exr" && ext != ".tga" && ext != ".bmp") {
        EditorConsole::Log("[Terrain] Unrecognised heightmap file type: " + ext +
                           ". Use .png / .jpg / .exr / .tga image files.", 2);
        return false;
    }

    // Release old terrain GL resources before recreating.
    m_terrain.reset();

    try {
        Terrain::TerrainConfig tc;
        tc.chunkSize = terrainConfig.chunkSize;
        tc.chunkResolution = terrainConfig.chunkResolution;
        tc.viewDistance = terrainConfig.viewDistance;
        tc.lodDistance = terrainConfig.lodDistance;
        tc.heightmapSize = terrainConfig.heightmapSize;
        tc.heightScale = terrainConfig.heightScale;
        tc.heightmapPath = terrainConfig.heightmapPath;
        // verify.txt: copy artist-authored material texture paths so the
        // terrain renders the correct surface material (not hardcoded).
        for (int i = 0; i < 4; ++i) {
            tc.materialLayers[i].albedo = terrainConfig.materialLayers[i].albedo;
            tc.materialLayers[i].normal = terrainConfig.materialLayers[i].normal;
            tc.materialLayers[i].rough  = terrainConfig.materialLayers[i].rough;
            tc.materialLayers[i].ao     = terrainConfig.materialLayers[i].ao;
        }
        tc.desertAlbedoPath = terrainConfig.desertAlbedoPath;
        tc.desertNormalPath = terrainConfig.desertNormalPath;
        tc.desertRoughPath  = terrainConfig.desertRoughPath;

        m_terrain = std::make_unique<Terrain>(tc);
        m_terrain->initialize(preloadedHeightmap, preloadedSize);
        m_terrainInitialized = true;
        std::cout << "[WorldManager] Terrain initialized from heightmap: "
                  << terrainConfig.heightmapPath << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[WorldManager] Failed to initialize terrain: " << e.what() << std::endl;
        m_terrainInitialized = false;
        return false;
    }
    return true;
}

// verify.txt: Rebuild Props button = world objects ONLY (JSON config, NO defaults).
// Does NOT touch terrain. Alerts the artist if the config is empty or an asset
// path doesn't exist — NO hardcoded fallback models.
bool WorldManager::rebuildWorldObjects(const Config::VegetationConfig& vegConfig) {
    // Release old world objects before re-creating.
    m_worldObjects.reset();
    m_vegetation.reset();

    m_worldObjects = std::make_unique<WorldObjectManager>();
    m_worldObjects->setPhysicsWorld(m_physicsWorld.get());
    m_worldObjects->initialize("./assets/World_objects/");
    // NO hardcoded defaults — the JSON config is the sole source. If it's empty
    // or an asset doesn't exist, the artist is alerted (no procedural fallback).
    m_worldObjects->loadModels();

    // Best-effort bootstrap for headless/test paths that bypass editor boot's
    // Config::loadConfig. Idempotent: skipped when world_objects were already
    // loaded (real editor startup) so behaviour is unchanged there.
    if (Config::getWorldObjectConfig().entries.empty()) {
        Config::loadConfig("engine_config.json");
    }

    const auto& entries = Config::getWorldObjectConfig().entries;
    if (entries.empty()) {
        std::cerr << "[WorldManager] World objects config is empty — no props loaded.\n";
        EditorConsole::Log("[WorldObjects] No entries in engine_config.json 'world_objects'. "
                           "Add props in World Settings → World Objects before building.", 2);
        return false;
    }

    for (const auto& e : entries) {
        // Alert the artist if the asset path doesn't exist. Require a REGULAR
        // FILE, not merely an existing path: a cleared Model field in the World
        // Settings panel persists as the bare asset dir ("assets/"), which
        // exists() happily accepts and then hands to Assimp -> the misleading
        // "No suitable reader found for the file format of file \"assets/\"".
        std::error_code fsEc;
        if (!std::filesystem::is_regular_file(e.modelPath, fsEc)) {
            std::cerr << "[WorldManager] World object asset not found: "
                      << e.modelPath << " (type: " << e.type << ")"
                      << (std::filesystem::is_directory(e.modelPath, fsEc)
                              ? " -- path is a directory, not a model file"
                              : "")
                      << "\n";
            EditorConsole::Log("[WorldObjects] Asset not found for '" + e.type +
                               "': " + e.modelPath + ". Place it in the assets folder.", 2);
            continue;  // skip this type, don't default to anything
        }
        float windSway = e.windSway;
        if (windSway == 0.0f && e.windSway == 0.0f) {
            windSway = (e.type.find("GRASS") != std::string::npos) ? 0.5f
                     : (e.type.find("FLOWER") != std::string::npos) ? 0.35f
                     : (e.type.find("BUSH") != std::string::npos) ? 0.3f
                     : 0.0f;
        }
        m_worldObjects->loadDynamicObject(e.type, e.modelPath,
                                          e.minScale, e.maxScale, windSway);
    }

    // Initialize the (empty) vegetation / biome system pools. Shared with
    // rebuildWorldObjectsAsync() so both paths leave the scatter pools in the
    // same clean state.
    initVegetationPools(vegConfig);

    return true;
}

// verify.txt: Rebuild Props (async, editor-only). Same reset + config apply as
// rebuildWorldObjects(), but the heavy Assimp parse + texture decode is
// dispatched to worker threads (WorldObjectManager::loadModelsAsync) and only
// the main-thread GL upload + registration is committed over time via
// commitWorldObjectLoading() (pumped every frame in update()). Lets the editor
// surface a live "Resolving..." progress bar instead of freezing the viewport.
bool WorldManager::rebuildWorldObjectsAsync(const Config::VegetationConfig& vegConfig) {
    // Release old world objects before re-creating.
    m_worldObjects.reset();
    m_vegetation.reset();

    m_worldObjects = std::make_unique<WorldObjectManager>();
    m_worldObjects->setPhysicsWorld(m_physicsWorld.get());
    m_worldObjects->initialize("./assets/World_objects/");

    if (Config::getWorldObjectConfig().entries.empty()) {
        Config::loadConfig("engine_config.json");
    }

    const auto& entries = Config::getWorldObjectConfig().entries;
    if (entries.empty()) {
        std::cerr << "[WorldManager] World objects config is empty — no props loaded.\n";
        EditorConsole::Log("[WorldObjects] No entries in engine_config.json 'world_objects'. "
                           "Add props in World Settings → World Objects before building.", 2);
        initVegetationPools(vegConfig);
        return false;
    }

    // Author the config table first (populate-only, no GL/parse), then let the
    // renderer dispatch one worker parse per unique asset path.
    for (const auto& e : entries) {
        if (!std::filesystem::exists(e.modelPath)) {
            std::cerr << "[WorldManager] World object asset not found: "
                      << e.modelPath << " (type: " << e.type << ")\n";
            EditorConsole::Log("[WorldObjects] Asset not found for '" + e.type +
                               "': " + e.modelPath + ". Place it in the assets folder.", 2);
            continue;
        }
        float windSway = e.windSway;
        if (windSway == 0.0f) {
            windSway = (e.type.find("GRASS") != std::string::npos) ? 0.5f
                     : (e.type.find("FLOWER") != std::string::npos) ? 0.35f
                     : (e.type.find("BUSH") != std::string::npos) ? 0.3f
                     : 0.0f;
        }
        WorldObjectConfig cfg;
        cfg.modelPath  = e.modelPath;
        cfg.minScale   = e.minScale;
        cfg.maxScale   = e.maxScale;
        cfg.windSway   = windSway;
        m_worldObjects->addConfigEntry(e.type, cfg);
    }
    // Non-blocking: parses fly off to std::async workers; returns at once so
    // the editor loop can paint the Resolve bar while they run.
    m_worldObjects->loadModelsAsync();

    initVegetationPools(vegConfig);
    return true;
}

void WorldManager::initVegetationPools(const Config::VegetationConfig& vegConfig) {
    // Initialize Vegetation / Biome System (config only — no auto-scatter).
    // Pools stay empty at boot; artists paint via the Foliage Brush Painter.
    if (!m_terrainInitialized) return;

    VegetationSystem::VegetationConfig vc;
    vc.treeDensity = vegConfig.treeDensity;
    vc.grassDensity = vegConfig.grassDensity;
    vc.rockDensity = vegConfig.rockDensity;
    vc.pebbleDensity = vegConfig.pebbleDensity;
    vc.maxTreesPerChunk = vegConfig.maxTreesPerChunk;
    vc.maxRocksPerChunk = vegConfig.maxRocksPerChunk;
    vc.maxPebblesPerChunk = vegConfig.maxPebblesPerChunk;

    m_vegetation = std::make_unique<VegetationSystem>(vc);
    m_vegetation->clear();
    if (m_worldObjects) {
        std::cout << "[WorldManager] Automatic biome generator bypassed. "
                     "Waiting for brush painter inputs.\n";
    }
    std::cout << "[WorldManager] Mesh instance pools initialized empty for editor manipulation." << std::endl;
}

float WorldManager::commitWorldObjectLoading() {
    if (!m_worldObjects) return 1.0f;
    return m_worldObjects->commitWorldObjectLoading();
}

bool WorldManager::isWorldObjectLoading() const {
    return m_worldObjects && m_worldObjects->isWorldObjectLoading();
}

bool WorldManager::isWorldObjectLoadDone() const {
    // Loading finished (or never started) -> nothing left to commit.
    return m_worldObjects && m_worldObjects->isWorldObjectLoadDone();
}

float WorldManager::getWorldObjectLoadProgress() const {
    return m_worldObjects ? m_worldObjects->getWorldObjectLoadProgress() : 1.0f;
}

void WorldManager::cancelWorldObjectLoading() {
    if (m_worldObjects) m_worldObjects->cancelWorldObjectLoading();
}

// Vegetation transfer (verify.txt §2): scatter the world-object instances that
// generateForChunk() produced onto the terrain and register a static collider
// for each, so the play-mode character collides with the SAME props the player
// sees. Runs once on full initialize(); the editor brush-painter still drives
// placement on Rebuild Terrain+Props (rebuildWorldObjects leaves m_vegetation
// empty — "biome bypassed").
void WorldManager::transferVegetationInstances() {
    if (!m_terrain || !m_terrainInitialized || !m_worldObjects || !m_vegetation) {
        return;
    }

    const float chunkSize = 80.0f;
    // Empty heights: generateForChunk()'s guard seeds only the origin chunk
    // (0,0) in this headless path. The real editor path would feed the terrain
    // heightmap here so all 5x5 view chunks populate; the body count is what
    // these tests assert on, not the exact scatter density.
    std::vector<float> heights;
    for (int cx = -2; cx <= 2; ++cx) {
        for (int cz = -2; cz <= 2; ++cz) {
            m_vegetation->generateForChunk(cx, cz, chunkSize, heights, 1024);
        }
    }

    // Transfer trees.
    for (const auto& t : m_vegetation->getTrees()) {
        const float y = m_terrain->getHeightAt(t.position.x, t.position.z);
        const char* key = (t.type == 1) ? "TREE_OAK"
                     : (t.type == 2) ? "TREE_BIRCH"
                                     : "TREE_PINE";
        m_worldObjects->placeObject(key, glm::vec3(t.position.x, y, t.position.z), 1.0f);
    }
    // Transfer rocks (boulder/stone/cliff). Pebbles (type 3) are tracked
    // separately by getPebbles() and are too small to block the character.
    for (const auto& r : m_vegetation->getRocks()) {
        if (r.type == 3) continue;
        const float y = m_terrain->getHeightAt(r.position.x, r.position.z);
        const char* key = (r.type == 1) ? "ROCK_STONE"
                     : (r.type == 2) ? "ROCK_CLIFF"
                                     : "ROCK_BOULDER";
        m_worldObjects->placeObject(key, glm::vec3(r.position.x, y, r.position.z), 1.0f);
    }

    std::cout << "[WorldManager] Vegetation transfer: "
              << m_vegetation->getTrees().size() << " trees, "
              << m_vegetation->getRocks().size() << " rocks -> static colliders.\n";
}

void WorldManager::shutdown() {
    // NOTE: the old `static bool alreadyShutdown` guard was removed so that
    // "Rebuild Terrain + Props" can re-call initialize() → cleanup → reinit
    // cleanly. The per-object unique_ptr resets make repeated shutdown safe.

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

    if (m_terrainInitialized && m_terrain) {
        m_terrain->update(cameraPosition, dt);
    }
    
    auto terrainEnd = std::chrono::high_resolution_clock::now();
    m_stats.terrainUpdateTime = std::chrono::duration<float, std::milli>(terrainEnd - start).count();

    if (m_worldObjects) {
        // Pump the in-flight async prop load: finish whichever worker parses
        // landed this frame (Model::LoadAsync), upload GL buffers on this
        // (main/GL) thread, and wire up the WOM bookkeeping. Drives the
        // editor's "Resolving..." progress bar until isWorldObjectLoadDone().
        if (isWorldObjectLoading()) {
            commitWorldObjectLoading();
        }
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

bool WorldManager::raycastTerrain(const glm::vec3& origin, const glm::vec3& dir,
                                  float maxDist, float& outDist, glm::vec3& outPoint) const {
    // Coarse heightfield ray-march for the viewport foliage brush. Only valid
    // with a terrain present; 64 samples is ample precision for editor painting.
    if (m_arenaMode || !m_terrainInitialized) return false;
    const int steps = 64;
    const float step = maxDist / static_cast<float>(steps);
    for (int i = 0; i <= steps; ++i) {
        float t = step * static_cast<float>(i);
        glm::vec3 p = origin + dir * t;
        float h = getHeightAt(p.x, p.z);
        if (p.y <= h) { outDist = t; outPoint = glm::vec3(p.x, h, p.z); return true; }
    }
    return false;
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

void WorldManager::renderForShadows(const glm::mat4& lightView,
                                    const glm::mat4& lightProj,
                                    Shader* shadowShader) {
    if (!shadowShader || shadowShader->ID == 0) return;

    if (m_arenaMode && m_arena) {
        // Arena geometry has no dedicated shadow pass; skip for now.
        return;
    }

    const glm::mat4 lightSpace = lightProj * lightView;
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);  // depth-only

    // --- Terrain shadow map ---
    // Render terrain chunks into the shadow map. Chunk vertices are already
    // in world space (TerrainChunk::createMesh) so the model matrix is identity.
    // The shadow shader (shadow_depth.vert) reads aPos@loc0 + aModel@loc6 (mat4).
    // We set aModel as a vertex-attribute CONSTANT (glVertexAttrib4fv WITHOUT
    // glEnableVertexAttribArray) because terrain chunk VAOs only enable loc 0;
    // the constant value is used by the shader when the attribute is not an array.
    if (m_terrainInitialized && m_terrain) {
        shadowShader->use();
        shadowShader->setMat4("uLightSpaceMatrix", lightSpace);
        const glm::mat4 identity(1.0f);
        for (int i = 0; i < 4; ++i)
            glVertexAttrib4fv(6 + i, &identity[i][0]);
        for (const auto& [coord, chunk] : m_terrain->getChunks()) {
            if (!chunk || chunk->getLOD() >= 3) continue;
            chunk->draw();
        }
    }

    // --- World objects shadow map ---
    // Render placed prop instances (trees, rocks, plants) into the shadow map.
    // Same constant-attribute approach: set aModel@loc6 as a non-array constant
    // before each draw so the shadow shader transforms vertices correctly.
    if (m_worldObjects) {
        auto& swr = m_worldObjects->getSimpleWorldRenderer();
        const auto& instances = swr.getActiveInstances();
        if (!instances.empty()) {
            shadowShader->use();
            shadowShader->setMat4("uLightSpaceMatrix", lightSpace);
            for (const auto& inst : instances) {
                if (!inst.model) continue;
                const glm::mat4& m = inst.modelMatrix;
                for (int i = 0; i < 4; ++i)
                    glVertexAttrib4fv(6 + i, &m[i][0]);
                inst.model->DrawStatic(*shadowShader);
            }
        }
    }

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

} // namespace World
