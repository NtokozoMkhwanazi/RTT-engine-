#include <glad/glad.h>
#include "editor_application.h"
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>   // glm::yaw(quat) for the bot<->twin sync
#include <algorithm>
#include <cmath>
#include <functional>  // std::function for skeleton debug recursion
#include <iostream>
#include <cstdio>   // snprintf into UI char[] buffers (playCharacterModel/playClipsDir)
#include <cstring>  // std::memset to clear the clip search filter on hot-swap
#include "editor/console.h"   // EditorConsole::Log (viewport debug counter)
#include "shader_manager.h"   // ShaderManager::GetUnlitShaderProgram (Unlit viewport mode)
#include "editor/grid_renderer.h"  // GridRenderer::Draw (now in renderSceneOnly for FBO-correct rendering)
#include "ecs/components/LayerComponent.h"   // LayerComponent (layer-cull sync in renderSceneOnly)
#include "ecs/components/Components.h"      // ECS components for digital-twin binding (Part 2)
#include "ecs/components/AnimatedCharacterComponent.h" // geo Personnel -> animated NPC twin
#include "editor/entity_manager.h"   // CreatePrimitive/ValidateOrClear/DeleteEntity (bot twin)
#include "editor/AssetKindScale.h"  // characterParityScale / assetKindSizeMultiplier (scale parity w/ play bot)
#include "editor/CameraFocus.h"      // selection-driven camera focus (camera follows the selected entity)
#include "editor/ui_cache.h"         // EntityCache (entityCache().dirty outliner flag)
#include "editor/undo_redo.h"        // UndoRedo::RecordCreate/RecordDelete (Part 2 twin lifecycle)
#include "editor/profiler.h"        // PROFILE_CPU_SCOPE frame phases (Profiler panel)
#include <unordered_map>             // telemetry UUID -> ECS twin bridge (Part 2 sync)

#include <imgui.h>
#include "editor/ui.h"
#include "editor/editor_state.h"   // g_editor + Editor ECS/render accessors (NPC/primitive forward pass)
#include "world/WorldObjectManager.h"   // EditorApplication::getWorldRenderer routes here (existing renderer)
#include "world/Terrain.h"              // Terrain::DecodeHeightmapFile (worker-side heightmap decode)
#include "editor/scene_manager.h"       // CommitSceneModelLoads (async scene/world prop resolve + bar)
#include "modelSystem/Model.h"
#include "renderer/DebugRenderer.h"  // g_debugRenderer for skeleton debug
#include <cmath>
#include "boneSystem/Skeleton.h"    // Skeleton for skeleton debug
#include "shaderSystem/Shader.h"
#include "editor/AssetKindMesh.h"   // Todo 7: AssetKind -> render mesh mapping
#include "ui_system_command_processor.h"   // forUI.txt Part 3: centralized UI command processor
#include <filesystem>                       // skybox face existence check in the hot-swap callback

namespace Editor {

EditorApplication& EditorApplication::getInstance() {
    static EditorApplication instance;
    return instance;
}

EditorApplication::EditorApplication()
    : m_renderPipeline(Render::RenderPipeline::getInstance())
    , m_inputManager(Input::InputManager::getInstance())
    , m_worldManager(World::WorldManager::getInstance())
    , m_resourceManager(Resources::ResourceManager::getInstance()) {
}

EditorApplication::~EditorApplication() {
    shutdown();
}

bool EditorApplication::initialize(int windowWidth, int windowHeight) {
    m_windowWidth = windowWidth;
    m_windowHeight = windowHeight;
    m_state = AppState::Initializing;

    std::cout << "[EditorApplication] Initializing..." << std::endl;

    // Consume engine_config.json now (paths/render/terrain/vegetation/physics
    // + the "npcs" skinned-model list) BEFORE any system reads Config::, so the
    // editor boots from the file instead of the hardcoded struct defaults.
    Config::loadConfig("engine_config.json");

    // forUI.txt Step 4: re-hydrate the skybox / physical-sky authoring state
    // from the persisted "atmosphere_pipeline" block (absent -> struct defaults).
    Config::LoadAtmosphereSettings("engine_config.json",
                                   g_editor.uiState.skyboxParams,
                                   g_editor.uiState.physicalSkyParams);

    // Window creation should be done in main() for now as per test_refactored.cpp
    m_window = glfwGetCurrentContext();
    if (!m_window) {
        std::cerr << "[EditorApplication] Error: No GLFW context found!" << std::endl;
        return false;
    }

    if (!m_renderPipeline.initialize()) return false;

    // Viewport renders through the SINGLE forward renderer (RenderPipeline's
    // forward pass + the editor forward renderer share one FBO/depth/view) so
    // the gizmo + props + ECS + arena + skybox all land in one composite pass.
    // This kills the deferred double-render (ECS to the G-Buffer AND as a
    // forward overlay, with the deferred lighting composite sitting under the
    // forward props/gizmo) that disconnected the gizmo from the selected
    // prop/NPC. Live toggle: World Settings -> "Single Forward Renderer";
    // flip to Deferred to revert to the old two-renderer path.
    m_renderPipeline.setDeferredEnabled(false);
    
    m_inputManager.initialize(m_window);
    
    // Arena mode disabled — use terrain with rocks_ground_05_4k textures
    // and the Shaw character (male locomotion pack) as the default scene.
    m_worldManager.setArenaMode(false);
    if (!m_worldManager.initialize(Config::getTerrainConfig(), Config::getVegetationConfig())) {
        std::cerr << "[EditorApplication] WorldManager initialization had issues" << std::endl;
    }

    // Snap geospatial entities onto the real heightmap terrain (not the sine
    // placeholder): hand the WorldManager's live Terrain to the ECS
    // GeoTerrainSystem so its sampleTerrainHeight() delegates to
    // Terrain::getHeightAt. No-ops (sine fallback) if the terrain didn't load.
    if (auto* terrain = m_worldManager.getTerrain()) {
        g_editor.geoTerrainSystem().setTerrain(terrain);
    }

    // forRenderer.txt: wire the CSM shadow pass so terrain chunks and placed
    // world-object props (trees, rocks, plants) cast shadows into the cascade
    // array, not just the ECS primitives + Character.
    m_renderPipeline.setShadowRenderCallback(
        [this](const glm::mat4& lightView, const glm::mat4& lightProj) {
            m_worldManager.renderForShadows(lightView, lightProj,
                                            m_renderPipeline.getShadowShader());
        });

    // The follow camera must always stay BEHIND the bot: auto-orient it to
    // the bot's heading so it orbits around to the back the moment the bot
    // turns, instead of ending up in front of it ("the bot passes the
    // camera" visual glitch).
    m_playCamera.config.orientToCharacterForward = true;

    // Eye height scales with the bot: a 1.26 m-tall bot (70% visual scale)
    // needs the camera near its shoulders, not 1.6 m above its feet.
    // (Tracks AnimatedCharacter::visualScale — update both together.)
    m_playCamera.config.pivotHeight = g_editor.uiState.playCameraPivotHeight;  // driven by the World Settings Look-At Height slider

    // Pull the play character from engine_config.json ("character" section) so
    // the boot model + clip set are authored by the artist in config — NOT a
    // hardcoded stand-in (forUI.txt: "engine as a tool, no pre-baked model
    // paths; config drives loading"). An empty "character" section (the
    // default) must still boot a stand-in bot so the artist sees the scene (and
    // the follow camera has something to focus) immediately; resolve it through
    // resolvePlayCharacterMeshPath so an explicit artist path always wins and an
    // empty path falls back to the AssetKind stand-in (assets/bot.fbx for a
    // humanoid character). Previously the raw (empty) path skipped load()
    // entirely, so the bot never spawned and the follow camera resolved
    // m_playController.character() to a default (0,0,0) AnimatedCharacter —
    // focusing the world origin instead of the bot.
    m_playModelPath = resolvePlayCharacterMeshPath(
        Config::getCharacterConfig().modelPath,
        geo::AssetKind::Personnel);
    m_playClipsDir  = Config::getCharacterConfig().clipsDir;
    std::cout << "[Config] Play character boot: model=" << m_playModelPath
              << " clipsDir=" << m_playClipsDir << std::endl;

    // Default stand-in bot: the viewport boots with a loaded character so the
    // artist sees the scene (camera follow, grid, props) immediately. The
    // Character panel hot-reloads a new model+clips via syncPlayCharacterPath()
    // (async swap in stage 2); clearing the field + committing empties the bot.
    if (!m_playModelPath.empty()) {
        if (!m_playController.load(m_playModelPath, m_playClipsDir)) {
            std::cerr << "[EditorApplication] Failed to load character '"
                      << m_playModelPath << "' at startup" << std::endl;
        } else if (m_playController.isLoaded()) {
            // Spawn the bot ON the terrain surface right away (physics floor
            // snap) so the idle viewport bot never hovers above a valley.
            AnimatedCharacter& ch = m_playController.character();
            const float surf = m_worldManager.getSurfaceHeightAt(ch.position.x, ch.position.z);
            ch.position.y = surf;
            ch.grounded = true;
            std::cout << "[Spawn] bot clamped to surface y=" << surf
                      << " at (" << ch.position.x << "," << ch.position.z << ")\n";
        }
    }
    // Render mesh follows the (possibly empty) model path. Empty path => no
    // render model until the artist loads one.
    m_playRenderModel = (!m_playModelPath.empty())
        ? std::make_unique<Model>(m_playModelPath) : nullptr;
    if (m_playRenderModel && m_playRenderModel->GetMeshCount() == 0) {
        std::cerr << "[EditorApplication] Startup character '"
                  << m_playModelPath << "' has no meshes" << std::endl;
        m_playRenderModel.reset();
    }

    // Mirror the actually-loaded path into the Character inspector so the field
    // reflects the live bot (and edits hot-reload from here) instead of a stale
    // default that nothing reads.
    std::snprintf(g_editor.uiState.playCharacterModel,
                  sizeof(g_editor.uiState.playCharacterModel), "%s",
                  m_playModelPath.c_str());
    std::snprintf(g_editor.uiState.playClipsDir,
                  sizeof(g_editor.uiState.playClipsDir), "%s",
                  m_playClipsDir.c_str());

    m_state = AppState::Running;
    std::cout << "[EditorApplication] Initialized successfully" << std::endl;
    return true;
}

int EditorApplication::run() {
    m_lastTime = glfwGetTime();
    
    while (m_state != AppState::ShuttingDown && !glfwWindowShouldClose(m_window)) {
        float currentTime = glfwGetTime();
        float dt = currentTime - m_lastTime;
        m_lastTime = currentTime;

        // Frame boundary for the Profiler panel: opened here (outermost) and
        // closed after the UI hook + swap, so the reported frame time covers
        // input + update + scene + UI build + UI submit + present. The nested
        // RenderPipeline::beginFrame/endFrame scopes inside renderSceneOnly()
        // are depth-tracked and don't restart this clock.
        Profiler::Instance().beginFrame();
        {
            PROFILE_CPU_SCOPE("Frame.Input");
            processInput(dt);
        }
        update(dt);
        render(dt);
        {
            PROFILE_CPU_SCOPE("Frame.PollEvents");
            glfwPollEvents();
        }
        Profiler::Instance().endFrame();
    }
    return 0;
}

void EditorApplication::shutdown() {
    if (m_state == AppState::ShuttingDown) return;
    
    m_state = AppState::ShuttingDown;
    m_playController.shutdown();
    m_playRenderModel.reset();
    // Stage 2: join the hot-swap worker threads before other teardown so no
    // in-flight character-pack rebuild outlives the render context.
    m_jobSystem.shutdown();
    m_worldManager.shutdown();
    m_inputManager.shutdown();
    m_renderPipeline.shutdown();

    // Drop the model/shader/texture cache while the GL context is still
    // alive. ResourceManager is a Meyers singleton - if its cache survives to
    // static-destruction time it is destroyed AFTER main() has torn the
    // context down, and Mesh/Model destructors issue GL calls (a SIGSEGV at
    // exit, see crash.log). Clearing here makes the GL teardown order
    // deterministic.
    m_resourceManager.clearCache();
}

// ============================================================================
// Staged "Reload Complete Stage" (landscape + props)
// ============================================================================
// BEFORE: the Engine Config button called setArenaMode(false) +
// initializeTerrain() + rebuildWorldObjects() inline inside the ImGui button
// handler. Every Assimp parse, texture decode + GL upload and the whole chunk
// rebuild happened in ONE frame, so the window froze for seconds (the reported
// "60 FPS -> 1 FPS" stall) with no feedback at all.
//
// NOW: the button only QUEUES the job. update() runs one stage per frame:
//   stage 0 - landscape rebuild (one blocking, GL-bound call; timed + logged)
//   stage 1 - props via rebuildWorldObjectsAsync(): Assimp parse + texture
//             decode on worker threads, only the GL upload/registration
//             commits on the main thread, budgeted by
//             commitWorldObjectLoading() (already pumped by
//             WorldManager::update), so the viewport keeps drawing.
// uiState.stageReload drives the "Resolving..." bar while this runs.

void EditorApplication::requestStageReload(bool terrainOnly) {
    auto& sr = g_editor.uiState.stageReload;
    if (sr.active) return;              // one stage job at a time
    sr.active      = true;
    sr.stage       = 0;
    sr.terrainOnly = terrainOnly;
    sr.progress    = 0.0f;

    // Start the heightmap decode NOW, on a worker thread: it is the single
    // largest remaining cost of a landscape rebuild (measured 3 s for a 4K
    // EXR/PNG) and needs no GL context. The old terrain keeps rendering while
    // it runs, and pumpStageReload() builds the new one when it lands.
    m_stageHeightmapPending = false;
    const auto& tcfg = Config::getTerrainConfig();
    if (!tcfg.heightmapPath.empty() && std::filesystem::exists(tcfg.heightmapPath)) {
        const std::string path  = tcfg.heightmapPath;
        const float       scale = tcfg.heightScale;
        m_stageHeightmapFuture = std::async(std::launch::async, [path, scale]() {
            StageHeightmapDecode r;
            r.path = path;
            r.ok   = Terrain::DecodeHeightmapFile(path, scale, r.data, r.size);
            return r;
        });
        m_stageHeightmapPending = true;
    }

    EditorConsole::Log(terrainOnly
        ? "[Stage Sync] Queued: landscape rebuild (frame-sliced - editor stays live)."
        : "[Stage Sync] Queued: landscape + props rebuild (frame-sliced - editor stays live).");
}

bool EditorApplication::isStageReloading() const {
    return g_editor.uiState.stageReload.active;
}

void EditorApplication::pumpStageReload() {
    auto& sr = g_editor.uiState.stageReload;
    if (!sr.active) return;

    if (sr.stage == 0) {
        // The worker is still decoding the heightmap: do NOT block this frame,
        // the old terrain keeps rendering and the resolve bar keeps moving.
        if (m_stageHeightmapPending) {
            if (m_stageHeightmapFuture.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready) {
                sr.progress = 0.0f;
                return;
            }
            // Decode finished (or failed): join it on this frame.
            StageHeightmapDecode decoded = m_stageHeightmapFuture.get();
            m_stageHeightmapPending = false;
            m_stageHeightmap       = std::move(decoded.data);
            m_stageHeightmapSize   = decoded.ok ? decoded.size : 0;
            if (!decoded.ok) {
                EditorConsole::Log("[Stage Sync] Heightmap decode failed - falling back to the "
                                   "synchronous path.", 2);
            }
        }

        // Landscape: one blocking call (material texture uploads + chunk grid).
        // The 4K heightmap decode that used to dominate this is already done on
        // a worker, and the material textures come from the shared cache, so
        // this is now a few hundred ms instead of ~22 s.
        const auto t0 = std::chrono::steady_clock::now();
        m_worldManager.setArenaMode(false);
        const bool terrainOk = (m_stageHeightmapSize > 0)
            ? m_worldManager.initializeTerrain(Config::getTerrainConfig(),
                                               &m_stageHeightmap, m_stageHeightmapSize)
            : m_worldManager.initializeTerrain(Config::getTerrainConfig());
        sr.terrainMs = std::chrono::duration<float, std::milli>(
                           std::chrono::steady_clock::now() - t0).count();

        // Terrain::initialize() copied the grid into the terrain, so drop our
        // copy (a 4K grid is 64 MB) instead of holding it between rebuilds.
        m_stageHeightmap.clear();
        m_stageHeightmap.shrink_to_fit();
        m_stageHeightmapSize = 0;

        if (terrainOk) {
            EditorConsole::Log("[Stage Sync] Landscape matrix generated in " +
                               std::to_string((int)sr.terrainMs) + " ms.");
        } else {
            EditorConsole::Log("[Stage Sync] Landscape rebuild FAILED - check the heightmap path "
                               "in World Settings > Terrain.", 2);
        }

        if (sr.terrainOnly) {
            sr.progress = 1.0f;
            sr.active   = false;
            return;
        }

        // Props: dispatch the worker parses and let the normal per-frame commit
        // pump finish the GL side. Nothing heavy runs on this frame.
        const bool propsStarted =
            m_worldManager.rebuildWorldObjectsAsync(Config::getVegetationConfig());
        auto& async = g_editor.uiState.worldObjectsAsync;
        if (propsStarted) {
            async.isLoading = true;
            async.finished  = false;
            async.progress  = 0.0f;
            sr.stage        = 1;
            sr.progress     = 0.0f;
            EditorConsole::Log("[Stage Sync] Props parsing on worker threads - Resolving...");
        } else {
            EditorConsole::Log("[Stage Sync] Props rebuild failed - check console for missing assets.", 2);
            async.isLoading = false;
            sr.progress = 1.0f;
            sr.active   = false;
        }
        return;
    }

    if (sr.stage == 1) {
        sr.progress = m_worldManager.getWorldObjectLoadProgress();
        if (m_worldManager.isWorldObjectLoadDone()) {
            sr.progress = 1.0f;
            sr.active   = false;
            auto& async = g_editor.uiState.worldObjectsAsync;
            async.isLoading = false;
            async.finished  = true;
            async.progress  = 1.0f;
            EditorConsole::Log("[Stage Sync] Complete: landscape + props ready (landscape " +
                               std::to_string((int)sr.terrainMs) + " ms, props resolved in background).");
        }
    }
}

void EditorApplication::processInput(float dt) {
    m_inputManager.update();

    // With the editor UI panels visible, ImGui captures the keyboard/mouse
    // over any window - including the viewport. Release that capture while
    // the cursor is over the viewport (and always for the keyboard during
    // play mode) so WASD + right-drag + scroll keep driving the character /
    // follow camera, exactly like the engine's viewport input routing. Text
    // fields and active widgets keep ImGui's capture.
    if (ImGui::GetCurrentContext()) {
        ImGuiIO& io = ImGui::GetIO();
        const bool splitterDragging = g_editor.uiState.isDraggingLeftSplit
                                   || g_editor.uiState.isDraggingRightSplit
                                   || g_editor.uiState.isDraggingBottomSplit;
        // While a panel splitter is being dragged, keep the 3D viewport INERT for
        // input: do not release mouse/keyboard ownership to the viewport camera so
        // the splitter (which owns the left-mouse drag) can't fight the view. The
        // viewport still RENDERS every frame (window-sized FBO); only its input
        // routing is suppressed mid-resize. (processInput runs before the UI render
        // of the same frame, so this observes the flags set last frame; the drag
        // flags persist for the whole hold, so coverage is frame-accurate enough.)
        const bool overViewport = (!splitterDragging)
                                && !UI::IsSplitterHovering()
                                && UI::IsViewport3DHovered() && !io.WantTextInput;
        bool captureKb = io.WantCaptureKeyboard;
        bool captureMouse = io.WantCaptureMouse;
        if (overViewport) captureMouse = false;
        if ((overViewport || m_isPlaying) && !ImGui::IsAnyItemActive() && !io.WantTextInput) {
            captureKb = false;
        }
        m_inputManager.setImGuiCapture(captureKb, captureMouse);
    }

    // forUI.txt Mode Switcher Strip: F1/F2/F3 toggle the global editor mode
    // (Select / Landscape / Foliage), mirroring the Unreal-style toolstrip.
    // Raw GLFW key state (InputManager wraps glfwGetKey) so these shortcuts
    // fire regardless of ImGui widget focus.
    const auto& ks = m_inputManager.getInputState();
    if (ks.isKeyPressed(GLFW_KEY_F1)) g_editor.uiState.currentMode = ::Editor::EditorMode::Select;
    else if (ks.isKeyPressed(GLFW_KEY_F2)) g_editor.uiState.currentMode = ::Editor::EditorMode::Landscape;
    else if (ks.isKeyPressed(GLFW_KEY_F3)) g_editor.uiState.currentMode = ::Editor::EditorMode::Foliage;
}

glm::vec3 EditorApplication::worldUpdateCameraPos(bool playLoaded,
                                                   const ThirdPersonCamera& playCam) {
    // The camera is updated later in this frame (after character physics, per
    // the fixed pipeline order), so use its position from the previous frame -
    // one frame of lag is imperceptible for streaming/LOD.
    return playLoaded ? playCam.position : glm::vec3(0.0f);
}

void EditorApplication::UpdateAdaptiveBrushThrottle(float dt) {
    if (!ImGui::GetCurrentContext()) return;
    auto& gov = g_editor.uiState.brushGovernor;
    const float fps = ImGui::GetIO().Framerate;
    if (fps < gov.fpsFloorTarget) {
        gov.adaptiveDensityMultiplier = glm::max(0.15f, gov.adaptiveDensityMultiplier - 0.75f * dt);
        gov.lowFpsHysteresisTimer = 1.5f;
    } else if (fps > gov.fpsCeilingTarget) {
        if (gov.lowFpsHysteresisTimer > 0.0f) {
            gov.lowFpsHysteresisTimer -= dt;
        } else {
            gov.adaptiveDensityMultiplier = glm::min(1.0f, gov.adaptiveDensityMultiplier + 0.40f * dt);
        }
    }
}

// forUI.txt Pillar 4: End-key drop-to-floor + live surface snapping. Drops the
// active selection onto the terrain (ECS entities via TransformComponent, or
// instanced world-object props via SimpleWorldRenderer) when the End key fires
// or while the gizmo is being dragged with surface-snap enabled.
void EditorApplication::ProcessPlacementConstraints(float dt) {
    (void)dt;
    const auto& ks = m_inputManager.getInputState();
    auto& wm = m_worldManager;

    ecs::EntityID selectedEntity = EntityManager::ValidateOrClear(g_editor.selectedEntity());
    bool endKeyPressed = ks.isKeyPressed(GLFW_KEY_END);
    bool surfaceSnapActive = g_editor.uiState.surfaceSnappingEnabled;
    bool isGizmoDragging = GizmoRenderer::IsDragging();

    if (selectedEntity != ecs::INVALID_ENTITY_ID &&
        (endKeyPressed || (surfaceSnapActive && isGizmoDragging))) {
        auto* transform = g_editor.world().getComponent<ecs::TransformComponent>(selectedEntity);
        if (transform) {
            float targetY = wm.getSurfaceHeightAt(transform->position.x, transform->position.z);
            transform->position.y = targetY;
            transform->markDirty(); // clear matrix cache so bounding boxes update
            if (endKeyPressed) {
                EditorConsole::Log("Actor snapped safely down onto landscape topology base layer.");
            }
        }
    } else if (g_editor.hasSelectedPropInstance() &&
               (endKeyPressed || (surfaceSnapActive && isGizmoDragging))) {
        auto& simpleRenderer = wm.getWorldObjectManager()->getSimpleWorldRenderer();
        auto& propInstance = simpleRenderer.propInstance(g_editor.selectedPropInstance());
        glm::vec3 propWorldPos = glm::vec3(propInstance.modelMatrix[3]);
        float groundedY = wm.getSurfaceHeightAt(propWorldPos.x, propWorldPos.z);
        propInstance.modelMatrix[3].y = groundedY; // re-align position column entries
        if (endKeyPressed) {
            wm.getWorldObjectManager()->ForceRebuildSpatialIndex(); // refresh Octree index
            EditorConsole::Log("Instanced scene prop dropped down onto ground normal surface.");
        }
    }
}

// forGeo.txt - Direct Mesh-Attachment Sync Pipeline: Part 2.
// Procedurally binds incoming GeoAPI telemetry snapshots to ECS digital-twin
// actors in the viewport. Mirrors the decoupled telemetry -> state push so the
// editor viewport draws full 3D assets (resolved by AssetKind) snapping onto the
// landscape heightfield in lockstep with the 10 ms geo loop.
void EditorApplication::SynchronizeDigitalTwinTelemetryActors(float dt) {
    (void)dt;
    // Telemetry UUID (EntitySnapshot.id) -> ECS twin entity. Statically held so
    // the bridge never reallocates across frames (the map only grows/shrinks at
    // twin spawn/GC, never per-tick).
    static std::unordered_map<uint32_t, ecs::Entity> telemetry_to_ecs_bridge;

    auto& geoApi    = g_editor.geospatialSystem().getGeoAPI();
    const auto& snaps = geoApi.getSnapshots();
    ecs::World& world = g_editor.world();

    // Bind / transform each live telemetry twin into a viewport ECS actor.
    for (const auto& snap : snaps) {
        if (!snap.isValid) continue;

        ecs::Entity twin{ecs::INVALID_ENTITY_ID};
        if (const auto it = telemetry_to_ecs_bridge.find(snap.id); it != telemetry_to_ecs_bridge.end()) {
            twin = it->second;
        }

        if (!twin.isValid()) {
            // Lazy instantiation: spawn a fresh entity shell with the component
            // set the viewport renderer (ModelRenderSystem) consumes. Personnel
            // telemetry twins get an AnimatedCharacterComponent (-> Shaw.fbx +
            // locomotion) so the viewport animates geo personnel idle/walk/run by
            // feed speed instead of a static T-pose mesh; other kinds stay
            // ModelComponent-only. Branch the component SET up front so the
            // archetype ECS keeps stable pointers for the twin's lifetime.
            twin = (snap.kind == geo::AssetKind::Personnel)
                ? world.createEntityWithComponents<
                      ecs::TransformComponent,
                      ecs::ModelComponent,
                      ecs::AnimatedCharacterComponent,
                      ecs::NameComponent,
                      ecs::GeospatialComponent,
                      ecs::LayerComponent>()
                : world.createEntityWithComponents<
                      ecs::TransformComponent,
                      ecs::ModelComponent,
                      ecs::NameComponent,
                      ecs::GeospatialComponent,
                      ecs::LayerComponent>();
            if (!twin.isValid()) continue;

            telemetry_to_ecs_bridge[snap.id] = twin;

            // Corporate tracking label so the twin is identifiable in the outliner.
            if (auto* name = world.getComponentArchetype<ecs::NameComponent>(twin)) {
                char label[128];
                std::snprintf(label, sizeof(label), "Twin_Node_%u (%s)",
                              snap.id, geo::AssetKindToString(snap.kind));
                name->setName(std::string(label));
            }

            // Bucket the twin onto the Geo Tracked viewport layer for culling.
            if (auto* layer = world.getComponentArchetype<ecs::LayerComponent>(twin)) {
                layer->layer = ecs::LayerComponent::GeoTracked;
            }

            // Personnel: bind the animated NPC character (Shaw.fbx + locomotion
            // pack). It is geoDriven and ticked by ModelRenderSystem::
            // updateAnimatedCharacters; Step 2 below feeds it position/heading/
            // speed from the live snapshot so it walks/jogs/runs. Mirrors the
            // engine's CreateGeospatialTwin Personnel branch.
            if (snap.kind == geo::AssetKind::Personnel) {
                if (auto* ac = world.getComponentArchetype<ecs::AnimatedCharacterComponent>(twin)) {
                    const std::string charModel = "assets/Male Locomotion Pack/Shaw.fbx";
                    ac->renderHandle = ModelSystem::ModelRegistry::getInstance().load(charModel);
                    ac->clipsDir = "assets";
                    ac->character = std::make_unique<AnimatedCharacter>();
                    if (ac->character->load(charModel)) {
                        ac->character->loadLocomotion("assets");
                        ac->character->setMotionContext(AnimatedCharacter::MotionContext::LOCOMOTION);
                        ac->character->visualScale = 0.7f;   // 1.8 m authored -> 1.26 m render
                        ac->geoDriven = true;
                    } else {
                        EditorConsole::Log("SyncTwin: AnimatedCharacter failed to load "
                                           + charModel, 2);
                        ac->character.reset();
                    }
                }
            }

            g_editor.entityCache().dirty = true;  // Outliner lists the new twin
            UndoRedo::RecordCreate(twin.id);
        }

        // Resolve component handles (archetype-backed; primary storage path).
        auto* transform = world.getComponentArchetype<ecs::TransformComponent>(twin);
        auto* modelComp = world.getComponentArchetype<ecs::ModelComponent>(twin);
        auto* geoComp   = world.getComponentArchetype<ecs::GeospatialComponent>(twin);
        auto* ac        = world.getComponentArchetype<ecs::AnimatedCharacterComponent>(twin);
        if (!transform || !modelComp || !geoComp) continue;

        // Step 1: mirror the double-precision geodetic fix onto the component.
        geoComp->latitude  = snap.latitude;
        geoComp->longitude = snap.longitude;
        geoComp->altitude  = snap.altitude;
        geoComp->horizontalAccuracy = snap.accuracy;

        // Step 2: WGS84 (double) -> local ENU engine space (float), then snap
        // onto the terrain heightfield so assets don't clip below the landscape.
        glm::vec3 local = geoApi.getConverter().geospatialToLocal(
            snap.latitude, snap.longitude, snap.altitude);
        local.y = m_worldManager.getSurfaceHeightAt(local.x, local.z);

        // Animated NPC geo twins own position through their AnimatedCharacter
        // (TickAnimatedCharacter mirrors char->position -> transform each tick),
        // so geo drives the character's absolute ENU position + feed velocity
        // here; the FSM (idle/walk/jog/run by snap.speed) runs in
        // updateAnimatedCharacters. Static twins keep the direct transform write.
        const float h = glm::radians((float)snap.heading);
        if (ac && ac->character && ac->character->ready()) {
            ac->character->position = local;
            ac->character->heading = h;
            ac->geoMoveDir = glm::vec2(-std::sin(h), -std::cos(h));
            ac->geoMoveSpeed = float(std::clamp(snap.speed / 2.0, 0.0, 1.0));
            ac->geoDriven = true;
            transform->position = local;   // mirror for GeoViz cones / camera focus
            transform->setEulerAngles(glm::vec3(0.0f, h, 0.0f));
        } else {
            transform->position = local;
            transform->setEulerAngles(glm::vec3(0.0f, h, 0.0f));
        }
        transform->markDirty();  // invalidate model-matrix caches for position/rotation (scale applied after mesh bind)

        // Step 3: viewport mesh attachment + live hot-swap on asset-type drift.
        // resolvePlayCharacterMeshPath returns the canonical .fbx/.gltf for the
        // kind (empty modelPath falls back to the AssetKind stand-in).
        const std::string expectedMesh = resolvePlayCharacterMeshPath("", snap.kind);
        if (std::strcmp(modelComp->getModelPath(), expectedMesh.c_str()) != 0 ||
            !modelComp->isValid()) {
            modelComp->setModelPath(expectedMesh);
            // Bind the on-disk asset via the registry (cached, fast on repeat).
            // A missing file yields an invalid handle; isValid() stays false and
            // the ModelRenderSystem simply skips rendering that twin this frame.
            modelComp->modelHandle =
                ModelSystem::ModelRegistry::getInstance().load(expectedMesh);
            modelComp->visible = true;
            modelComp->castShadow = true;
            modelComp->receiveShadow = true;
            std::printf("[PIPELINE MESH BIND] Node ID %u -> 3D asset: %s (handle %s)\n",
                        snap.id, expectedMesh.c_str(),
                        modelComp->modelHandle.isValid() ? "ok" : "invalid");
        }

        // Personnel animated NPCs render Shaw.fbx (skinned); point the GeoViz gate
        // ModelComponent at it too so the bbox-based camera torso focus + any mesh
        // marker match the animated mesh, not the bot.fbx stand-in.
        if (ac && ac->character && ac->character->ready()) {
            const std::string charModel = "assets/Male Locomotion Pack/Shaw.fbx";
            if (std::strcmp(modelComp->getModelPath(), charModel.c_str()) != 0) {
                modelComp->setModelPath(charModel);
                modelComp->modelHandle = ac->renderHandle;
                modelComp->visible = true;
                modelComp->castShadow = true;
                modelComp->receiveShadow = true;
                std::printf("[PIPELINE MESH BIND] Node ID %u -> animated NPC asset: %s\n",
                            snap.id, charModel.c_str());
            }
        }

        // Scale parity with the play bot: normalize the twin's authored FBX height
        // to the same 1.26 m base AnimatedCharacter renders at, then the
        // AssetKind multiplier (see editor/AssetKindScale.h). The mesh was just
        // resolved above so modelComp->getModel() is live (invalid handle ->
        // characterParityScale returns 1.0f so the twin still renders at kind
        // scale). Same forward skin-shader path as the play bot -> no buffer split.
        Model* twinModel = modelComp->getModel();
        if (ac && ac->character && ac->character->ready()) {
            // Animated NPC: AnimatedCharacter::modelMatrix() (visualScale 0.7,
            // scale=targetHeight/rawSizeY -> 1.26 m) owns viewport scale;
            // renderAnimated() uses it so the ECS transform scale is unused.
            transform->scale = glm::vec3(1.0f);
        } else {
            transform->scale = characterParityScale(twinModel, glm::vec3(1.0f))
                             * assetKindSizeMultiplier(snap.kind);
        }
        transform->markDirty();
    }

    // Garbage collection: prune twins whose telemetry stream timed out (absent
    // from this snapshot batch) so the viewport doesn't accumulate stale actors.
    for (auto it = telemetry_to_ecs_bridge.begin(); it != telemetry_to_ecs_bridge.end();) {
        bool seen = false;
        for (const auto& snap : snaps) {
            if (snap.id == it->first) { seen = true; break; }
        }
        if (!seen) {
            UndoRedo::RecordDelete(it->second.id);
            world.destroyEntity(it->second);
            it = telemetry_to_ecs_bridge.erase(it);
            g_editor.entityCache().dirty = true;
        } else {
            ++it;
        }
    }
}

void EditorApplication::update(float dt) {
    PROFILE_CPU_SCOPE("Frame.Update");

    // forUI.txt Part 3: drain the decoupled UI command queue and apply it to the
    // ECS world. Runs BEFORE m_worldManager.update() and renderSceneOnly() so
    // every committed transform write is visible to this frame's render forEach.
    //   * UpdateTransform writes fan out to the world's ECS JobSystem workers
    //     (entity-disjoint ranges, barrier-waited) -- safe vs. the render pass.
    //   * SpawnPrimitive + SetAtmosphereCvar apply inline (structural/cvar).
    //   * HotSwapSkybox is bound here to the active render pipeline.
    {
        static UI::SystemCommandProcessor cmdProc(g_editor.world());
        static const bool skyboxWired = [p = &cmdProc]() {
            p->setSkyboxSwapCallback([](const std::string& dir) {
                std::string base = dir;
                if (!base.empty() && base.back() != '/') base += "/";
                const std::vector<std::string> faces = {
                    base + "px.jpg", base + "nx.jpg",
                    base + "py.jpg", base + "ny.jpg",
                    base + "pz.jpg", base + "nz.jpg"
                };
                for (const auto& f : faces) {
                    if (!std::filesystem::exists(f)) {
                        EditorConsole::Log("[Atmosphere] Skybox hot-swap failed: a cubemap face is missing (" + f + ").", 2);
                        return;
                    }
                }
                Render::RenderPipeline::getInstance().UpdateSkyboxTexture(faces);
                EditorConsole::Log("[Atmosphere] Skybox cubemap hot-swapped mid-frame (UI command).");
            });
            return true;
        }();
        (void)skyboxWired;
        cmdProc.process();
    }

    // The terrain streams chunks and computes LOD relative to this position,
    // so it must follow the actual camera, not the world origin.
    {
        PROFILE_CPU_SCOPE("Update.WorldManager");
        m_worldManager.update(worldUpdateCameraPos(m_playController.isLoaded(), m_playCamera), dt);
    }

    // Engine-layer async scene load (Open Scene): pump the deferred model
    // resolves (worker parse -> main-thread GL upload + ModelHandle attach).
    // Time-sliced inside CommitSceneModelLoads() so this stays >=30 FPS while a
    // heavy world of props / terrain-texture models commits. No-op until set.
    SceneManager::CommitSceneModelLoads(g_editor);

    // "Sync & Reload Complete Stage" / "Load Ground": advance the staged job
    // one step per frame instead of blocking inside the UI button handler.
    pumpStageReload();

    // Mirror the Character inspector's path/clips into the live bot and
    // hot-reload it when the artist commits an edit. Editor-time only.
    syncPlayCharacterPath();
    // Stage 2 (forhotswap.txt): adopt any worker-built staging pack that has
    // finished compiling. Runs pre-render so the new pose is used this frame;
    // falls back to the sync reload() path on timeout/failure (never regresses).
    tickCharacterSwap();
    UpdateAdaptiveBrushThrottle(dt); // forUI.txt Governor: pace brush density by frame rate
    ProcessPlacementConstraints(dt); // forUI.txt Pillar 4: End-key drop-to-floor + surface snapping

    // Drive the play-mode character. The bot is always loaded (idle outside
    // play mode); in play mode it reads the editor keyboard state.
    if (m_playController.isLoaded()) {
        // AAA gizmo hover easing (forGizmo.txt Step 2): per-axis glow target
        // smoothed with exponential decay so the gizmo breathes on hover.
        {
            auto& gi = g_editor.uiState.gizmoGlowValues;
            GizmoRenderer::GizmoAxis hover = g_editor.hoveredAxis();
            if (GizmoRenderer::IsDragging()) {
                hover = GizmoRenderer::GetDragAxis();
            }
            const float tX = (hover == GizmoRenderer::GizmoAxis::X) ? 1.0f : 0.0f;
            const float tY = (hover == GizmoRenderer::GizmoAxis::Y) ? 1.0f : 0.0f;
            const float tZ = (hover == GizmoRenderer::GizmoAxis::Z) ? 1.0f : 0.0f;
            const float f = 1.0f - std::exp(-gi.approachSpeed * dt);
            gi.x = glm::mix(gi.x, tX, f);
            gi.y = glm::mix(gi.y, tY, f);
            gi.z = glm::mix(gi.z, tZ, f);
            if (std::abs(gi.x - tX) < 0.001f) gi.x = tX;
            if (std::abs(gi.y - tY) < 0.001f) gi.y = tY;
            if (std::abs(gi.z - tZ) < 0.001f) gi.z = tZ;
        }
        CharacterInput input;
        if (m_isPlaying) {
            const auto& ks = m_inputManager.getInputState();
            glm::vec2 move(0.0f);
            // moveDirection is WORLD space (x->X, y->Z); the character turns
            // to face it and the follow camera orbits behind. The bot's local
            // forward is -Z, so W must push it AWAY from the camera (-Z), S
            // toward it (+Z), A strafe left (-X), D right (+X). Previously
            // W/S were inverted: pressing W walked the bot BACKWARD (toward
            // the camera) until it spun around to face +Z.
            if (ks.isKeyDown(GLFW_KEY_W)) move.y -= 1.0f;
            if (ks.isKeyDown(GLFW_KEY_S)) move.y += 1.0f;
            if (ks.isKeyDown(GLFW_KEY_A)) move.x -= 1.0f;
            if (ks.isKeyDown(GLFW_KEY_D)) move.x += 1.0f;
            const float len = glm::length(move);
            if (len > 0.001f) move /= len;
            input.moveDirection = move;
            input.moveMagnitude = std::min(1.0f, len);
            input.jump = ks.isKeyDown(GLFW_KEY_SPACE);
            input.crouch = ks.isKeyDown(GLFW_KEY_LEFT_CONTROL) || ks.isKeyDown(GLFW_KEY_C);
            input.sprint = ks.isKeyDown(GLFW_KEY_LEFT_SHIFT) || ks.isKeyDown(GLFW_KEY_RIGHT_SHIFT);
            input.grounded = true;
            input.verticalVelocity = 0.0f;
            // Motion-matching context switching (contextual databases):
            // 1 Locomotion, 2 Crouch, 3 Combat, 4 Capoeira, 5 Dance.
            // Edge-triggered; crouch (C) also auto-switches Locomotion<->Crouch.
            if (ks.isKeyPressed(GLFW_KEY_1)) input.motionContext = (int)AnimatedCharacter::MotionContext::LOCOMOTION;
            else if (ks.isKeyPressed(GLFW_KEY_2)) input.motionContext = (int)AnimatedCharacter::MotionContext::CROUCH;
            else if (ks.isKeyPressed(GLFW_KEY_3)) input.motionContext = (int)AnimatedCharacter::MotionContext::COMBAT;
            else if (ks.isKeyPressed(GLFW_KEY_4)) input.motionContext = (int)AnimatedCharacter::MotionContext::CAPOEIRA;
            else if (ks.isKeyPressed(GLFW_KEY_5)) input.motionContext = (int)AnimatedCharacter::MotionContext::DANCE;
        }

        // forPlayer.txt Theme 1: flag the controller while the viewport gizmo is
        // dragging the play-character twin, so updatePlayMode() pauses the FSM +
        // capsule resolver and syncPlayCharacterTwin() copies the twin's manual
        // edit into the bot this frame instead of the last locomotion tick
        // overwriting it. (Only meaningful in play mode: the flag is gated on
        // m_isPlaying because edit-mode twin->bot sync already wins every frame.)
        m_playController.isBeingManipulatedByGizmo =
            m_isPlaying &&
            GizmoRenderer::IsDragging() &&
            EntityManager::ValidateOrClear(g_editor.selectedEntity()) == m_playCharacterEntity;

        // Handle rewind (toolbar button) — reset the character animation clip
        if (g_editor.wantRewind()) {
            g_editor.setWantRewind(false);
            if (m_playController.isLoaded())
                m_playController.character().rewind();
        }

        updatePlayMode(dt, input);
    }

    // Keep the play-mode bot and its ECS gizmo-twin in lockstep so the viewport
    // gizmo can edit the (non-ECS) character. Runs after updatePlayMode() so a
    // playing bot's latest pose mirrors onto the twin each frame.
    syncPlayCharacterTwin();

    // forGeo.txt: project live digital-twin telemetry into viewport ECS actors
    // (mesh attach + hot-swap + terrain snap + GC) every frame. Runs after the
    // play-character twin sync so the geo viewport always reflects the newest
    // snapshot batch for the current frame's render pass.
    SynchronizeDigitalTwinTelemetryActors(dt);
}

void EditorApplication::syncPlayCharacterPath() {
    // Keep m_playModelPath consistent with the inspector even between reloads
    // so the source of truth never drifts.
    if (!g_editor.uiState.playReloadRequested) {
        m_playModelPath = g_editor.uiState.playCharacterModel;
        m_playClipsDir   = g_editor.uiState.playClipsDir;
        return;
    }
    g_editor.uiState.playReloadRequested = false;

    const std::string newPath  = g_editor.uiState.playCharacterModel;
    const std::string newClips = g_editor.uiState.playClipsDir;

    // An empty path can't be loaded - keep the previous bot rather than
    // nuking it the instant the artist clears the field with Backspace.
    if (newPath.empty()) {
        m_playModelPath = newPath;
        m_playClipsDir   = newClips;
        return;
    }

    m_playModelPath = newPath;
    m_playClipsDir   = newClips;

    // Preserve the AssetKind so renderPlayCharacter's Todo-7 mesh swap keeps
    // drawing the right twin kind, then swap Animator + viewport mesh together
    // from the SAME path so skeleton and mesh never mismatch.
    const geo::AssetKind prevKind = (m_playController.isLoaded()
        ? m_playController.character().geospatialAssetKind()
        : geo::AssetKind::Unknown);

    // Stage 2 (forhotswap.txt): dispatch the staging-pack build to a worker so
    // the motion KD-tree recompile doesn't stall the main thread. The live
    // character keeps animating its CURRENT pack until tickCharacterSwap()
    // adopts the worker-built RuntimeSystems (pre-render, atomic). If the bot
    // isn't loaded yet there's nothing to hot-swap in place, so fall back to a
    // fresh load() (the stage 1 path).
    if (m_playController.isLoaded()) {
        launchAsyncSwap(newPath, newClips, prevKind);
    } else {
        const bool loaded = m_playController.load(newPath, newClips);
        if (!loaded || !m_playController.isLoaded()) {
            std::cerr << "[EditorApplication] Failed to load character '"
                      << newPath << "' (kept previous bot)" << std::endl;
            return;
        }
        reloadRenderModel(prevKind, newPath);
    }
}

void EditorApplication::launchAsyncSwap(const std::string& modelPath,
                                        const std::string& clipsDir,
                                        geo::AssetKind prevKind) {
    // Ignore a re-entrant request while a swap is already in flight - the
    // inspector paths are mirrored every frame by syncPlayCharacterPath(), so
    // the next committed change lands once this one completes.
    if (m_charSwap.isLoading) {
        std::cerr << "[EditorApplication] Hot-swap already in progress for '"
                  << modelPath << "'; latest request will apply after the current one."
                  << std::endl;
        m_charSwap.prevKind = prevKind;
        return;
    }
    m_charSwap = CharacterSwapAsyncState{};
    m_charSwap.isLoading       = true;
    m_charSwap.prevKind        = prevKind;
    m_charSwap.timeoutFrames   = 600;  // ~10s @ 60fps before sync fallback
    m_charSwap.capsule         = std::make_shared<CharacterSwapAsyncState::Capsule>();
    const auto capsule = m_charSwap.capsule;

    // Worker: build a STAGING pack (load + locomotion KD-tree) and move its
    // systems out via ExtractRuntimeSystems(). The live character is NEVER
    // touched here - this closure only writes the shared capsule, which the
    // main thread reads after the JobHandle's future fulfils (happens-before).
    m_charSwap.handle = m_jobSystem.addJob([capsule, modelPath, clipsDir] {
        AnimatedCharacter staging;
        if (!staging.load(modelPath)) {
            capsule->failed.store(true);
            return;
        }
        if (staging.loadLocomotion(clipsDir) < 1) {
            capsule->failed.store(true);
            return;
        }
        capsule->systems = staging.ExtractRuntimeSystems();  // move out
        capsule->swapReady.store(true);
    });
}

void EditorApplication::tickCharacterSwap() {
    if (!m_charSwap.isLoading) return;

    // Poll completion (cheap; future-based). When isComplete() is true the
    // worker's promise is fulfilled, so reads of capsule are synchronized.
    if (!m_charSwap.handle.isComplete()) {
        if (--m_charSwap.timeoutFrames <= 0) {
            // Timed out building the staging pack - fall back to the verified
            // sync HotSwap path so the artist still gets their change.
            std::cerr << "[EditorApplication] Async hot-swap timed out for '"
                      << m_playModelPath << "'; falling back to sync reload"
                      << std::endl;
            m_charSwap.isLoading = false;
            m_charSwap.capsule.reset();
            m_playController.reload(m_playModelPath, m_playClipsDir);
            reloadRenderModel(m_charSwap.prevKind, m_playModelPath);

            // forhotswap.txt Step 4: timeout fell back to a sync reload — a new
            // character pack is active, so refresh the dynamic clip inspector.
            g_editor.uiState.characterUIData.uiMetadataNeedsRefresh = true;
            std::memset(g_editor.uiState.characterUIData.clipSearchFilter,
                        0, sizeof(g_editor.uiState.characterUIData.clipSearchFilter));
        }
        return;
    }

    // Worker finished - adopt the staging systems (atomic pre-render swap) or
    // fall back to the sync path if the build failed.
    if (m_charSwap.capsule && m_charSwap.capsule->swapReady.load()) {
        if (!m_playController.swapInRuntimeSystems(std::move(m_charSwap.capsule->systems))) {
            std::cerr << "[EditorApplication] swapInRuntimeSystems failed for '"
                      << m_playModelPath << "'; falling back to sync reload"
                      << std::endl;
            m_playController.reload(m_playModelPath, m_playClipsDir);
        }
    } else {
        std::cerr << "[EditorApplication] Async hot-swap build failed for '"
                  << m_playModelPath << "'; falling back to sync reload"
                  << std::endl;
        m_playController.reload(m_playModelPath, m_playClipsDir);
    }

    // forhotswap.txt Step 4: a new character pack is now active (whether adopted
    // via swapInRuntimeSystems or via a sync-reload fallback). Signal the
    // Details panel's dynamic clip inspector to re-pull the live clip name list
    // and clear any stale search filter. Placed after the swap/reload converge
    // so it covers both the swap-OK and build-fail fallback paths above.
    g_editor.uiState.characterUIData.uiMetadataNeedsRefresh = true;
    std::memset(g_editor.uiState.characterUIData.clipSearchFilter,
                0, sizeof(g_editor.uiState.characterUIData.clipSearchFilter));
    reloadRenderModel(m_charSwap.prevKind, m_playModelPath);
    m_charSwap.isLoading = false;
    m_charSwap.capsule.reset();
}

void EditorApplication::reloadRenderModel(geo::AssetKind prevKind,
                                          const std::string& modelPath) {
    if (!m_playController.isLoaded()) return;
    if (prevKind != geo::AssetKind::Unknown)
        m_playController.character().setGeospatialAssetKind(prevKind);
    m_playRenderModel = std::make_unique<Model>(modelPath);
    if (m_playRenderModel && m_playRenderModel->GetMeshCount() == 0) {
        std::cerr << "[EditorApplication] Character '" << modelPath
                  << "' has no meshes" << std::endl;
        m_playRenderModel.reset();
    }
    // Seed the mesh-swap guard so renderPlayCharacter leaves our mesh alone.
    m_lastRequestedMeshPath = meshPathForAssetKind(
        m_playController.character().geospatialAssetKind());
    // Re-clamp to the terrain so the freshly-loaded bot sits on the surface
    // instead of snapping back to y=0. swapInRuntimeSystems / reload()
    // preserved the live pose, so only the Y is nudged.
    AnimatedCharacter& ch = m_playController.character();
    const float surf = m_worldManager.getSurfaceHeightAt(ch.position.x, ch.position.z);
    ch.position.y = surf;
    ch.grounded = true;
}

void EditorApplication::syncPlayCharacterTwin() {
    // Bridge the play-mode bot (AnimatedCharacter, which is NOT itself an ECS
    // entity) and a lightweight ECS "twin" carrying a TransformComponent. The
    // viewport gizmo only operates on selectedEntity()'s TransformComponent, so
    // without this twin the bot is unreachable by the gizmo. Two rules:
    //   * EDIT MODE  ->  twin drives the bot (gizmo / Details edits flow in).
    //   * PLAY MODE  ->  the bot drives the twin (gizmo becomes a follower).
    // See forUI.txt "Propose Fix: Hooking the Bot to the Gizmo System".
    if (!m_playController.isLoaded()) {
        if (m_playCharacterEntity != ecs::INVALID_ENTITY_ID) {
            EntityManager::DeleteEntity(m_playCharacterEntity);
            m_playCharacterEntity = ecs::INVALID_ENTITY_ID;
        }
        // Also clear the editor-state handle so Details / outliner refresh
        g_editor.uiState.characterEntity = 0;
        return;
    }

    AnimatedCharacter& ch = m_playController.character();

    // Lazily (re)create the twin on demand. This must NOT happen in
    // initialize() — that runs before InitEditor()/g_editor.world().init()
    // (see editor_main.cpp main()), so the ECS World would be uninitialized.
    // By the time update() runs the world is live, making this frame the safe
    // point to instantiate the twin.
    ecs::EntityID twin = EntityManager::ValidateOrClear(m_playCharacterEntity);
    if (twin == ecs::INVALID_ENTITY_ID) {
        // A transform + name + layer twin only: NO MeshComponent, so it never
        // occludes the skinned bot (rendered separately by renderPlayCharacter)
        // yet it still appears in the Outliner AND the Layers panel.
        ecs::Entity e = g_editor.world().createEntityWithComponents<
            ecs::TransformComponent, ecs::NameComponent, ecs::LayerComponent>();
        if (!e.isValid()) return;
        if (auto* n = g_editor.world().getComponentArchetype<ecs::NameComponent>(e))
            n->setName("Play Character");
        if (auto* lc = g_editor.world().getComponentArchetype<ecs::LayerComponent>(e))
            lc->layer = ecs::LayerComponent::Characters;
        m_playCharacterEntity = e.id;
        twin = e.id;
        g_editor.uiState.characterEntity = e.id;
        g_editor.entityCache().dirty = true;  // so the Outliner lists the new twin
        // Seed the twin from the bot's current pose: this makes edit-mode's
        // twin->bot push a no-op on this frame (otherwise the bot would snap
        // to the world origin before the user touches the gizmo).
        if (auto* t0 = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e)) {
            t0->position = ch.position;
            t0->setEulerAngles(glm::vec3(0.0f, ch.heading, 0.0f));
            t0->scale = glm::vec3(ch.visualScale);
        }
    }

    ecs::TransformComponent* t =
        g_editor.world().getComponentArchetype<ecs::TransformComponent>(twin);
    if (!t) return;

    // forPlayer.txt Theme 1: also pull twin->bot during play-mode gizmo drags
    // (the FSM + capsule resolver are paused via isBeingManipulatedByGizmo above),
    // so a manual reposition/rotate/scale on the twin writes through to the bot
    // for the duration of the drag instead of the bot driving the twin.
    if (!m_isPlaying || m_playController.isBeingManipulatedByGizmo) {
        // Edit mode: the gizmo / Details panel edit the twin; flow it into the bot.
        ch.position = t->position;
        ch.heading = glm::yaw(t->rotation);                       // quat yaw -> heading (radians)
        ch.visualScale = (t->scale.x + t->scale.y + t->scale.z) / 3.0f;
        if (ch.visualScale < 0.01f) ch.visualScale = 0.7f;        // guard against 0-scale collapse
    } else {
        // Play mode: the bot drives the twin so the gizmo follows the actor.
        // (The gizmo drag still writes the twin this frame, but next frame's
        // edit-mode push is skipped while playing, so the bot stays authoritative.)
        t->position = ch.position;
        t->setEulerAngles(glm::vec3(0.0f, ch.heading, 0.0f));
        t->scale = glm::vec3(ch.visualScale);
    }
}

// forPlayer.txt Theme 2 (grounded correction): the world-object renderer is
// ALREADY drawn by WorldManager::render (world_manager.cpp:280) via its
// WorldObjectManager; this routes to that SAME instance so editor UI can
// raycast / enumerate props WITHOUT a duplicate renderer (no double-draw).
// No new member and no extra render() call - renderSceneOnly renders it
// transitively at m_worldManager.render(...) (line 520).
//
// Arena mode (bin/engine / test.cpp sets WorldManager::setArenaMode(true))
// skips WorldObjectManager creation inside WorldManager::initialize, so
// getWorldObjectManager() is null there. The viewport picking sites call
// getWorldRenderer() on EVERY click (raycastProps against prop AABBs) —
// dereferencing a null WorldObjectManager* here was the SIGSEGV (crash 11)
// on the first viewport click in the engine. Return a reference to a static
// default-constructed (empty-instance) SimpleWorldRenderer instead: raycastProps
// then iterates 0 instances and returns false (a no-op pick), so arena-mode
// clicks degrade to a graceful no-op rather than a crash. A default-constructed
// SimpleWorldRenderer is cheap (empty vectors, null WaterRenderer) and is never
// Render'd in arena mode (the arena scene owns drawing).
SimpleWorldRenderer& EditorApplication::getWorldRenderer() {
    if (WorldObjectManager* wom = m_worldManager.getWorldObjectManager()) {
        return wom->getSimpleWorldRenderer();
    }
    static SimpleWorldRenderer nullRenderer;
    return nullRenderer;
}
const SimpleWorldRenderer& EditorApplication::getWorldRenderer() const {
    if (const WorldObjectManager* wom = m_worldManager.getWorldObjectManager()) {
        return wom->getSimpleWorldRenderer();
    }
    static const SimpleWorldRenderer nullRenderer;
    return nullRenderer;
}

void EditorApplication::updatePlayMode(float dt, const CharacterInput& input) {
    if (!m_playController.isLoaded()) return;

    // Surface height function - the character snaps to the world floor. Uses
    // getSurfaceHeightAt (max of terrain AND boulder/rock/trunk tops) so the
    // character can also stand ON solid world objects, not just walk into
    // them.
    auto terrain = [this](float x, float z) -> float {
        return m_worldManager.getSurfaceHeightAt(x, z);
    };
    // Apply playback speed from the slider (0.25x–4x). Scales the
    // simulation dt so the character plays faster/slower.
    const float playbackSpeed = m_playController.isLoaded()
        ? m_playController.character().playbackSpeed : 1.0f;
    m_playController.update(dt * playbackSpeed, input, terrain);

    // Solid world objects (boulders/rocks/trees) carry static physics
    // colliders: resolve the character's capsule against them so it cannot
    // walk through - the feet get pushed out of any overlap and the velocity
    // component heading into the surface is killed (slides along / stops).
    // forPlayer.txt Theme 1: skip the capsule resolver while the gizmo drives
    // the bot this frame - otherwise resolveCharacterCollision pushes the feet
    // out from under the user's manual translate / scale.
    if (!m_playController.isBeingManipulatedByGizmo && m_worldManager.getPhysicsWorld()) {
        AnimatedCharacter& cc = m_playController.character();
        glm::vec3 feet = cc.position;
        glm::vec3 vel = cc.velocity;
        const float radius = 0.4f * cc.visualScale;     // capsule radius (m) - 70% of 0.4, tracks render scale
        const float totalHeight = 1.8f * cc.visualScale; // capsule height (m) - 70% of 1.8, matches the rendered bot
        if (m_worldManager.resolveCharacterCollision(feet, radius, totalHeight, vel, cc.grounded)) {
            cc.position = feet;
            cc.velocity = vel;
        }
    }

    // ---- diagnostic: confirm the character's base sits ON the terrain surface ----
    {
        static int botDbg = 0;
        if (++botDbg % 60 == 0) {
            const AnimatedCharacter& dbg = m_playController.character();
            const float th = terrain(dbg.position.x, dbg.position.z);
            std::cout << "[Bot] pos.y=" << dbg.position.y
                      << " terrain=" << th
                      << " diff=" << (dbg.position.y - th)
                      << " grounded=" << (dbg.grounded ? "YES" : "NO") << "\n";
        }
    }

    // Drive the third-person follow camera. Per the camera milestone the follow
    // camera is INDEPENDENT of the playable character: it tracks the SELECTED
    // entity (NPC / play-character twin / prop) when one is selected, and only
    // falls back to the live bot when nothing is selected. An animated /
    // playable focus is orbit-followed behind its heading; a static prop (e.g. a
    // tree) is framed in place with no orbit — it has no forward to chase. The
    // living bot remains the fallback focus, and its FSM animation state still
    // drives the state-aware smoothing exactly as before.
    //
    // [DEBUG] The follow camera is the one that should be moving here; the free
    // flyCamera stays inert in Free mode. If the wrong camera is moving, this is
    // the pivot point to inspect (selection resolution -> camIn -> update).
    const AnimatedCharacter& cc = m_playController.character();
    EntityFocus characterFocus;
    characterFocus.position = cc.position;
    characterFocus.forward = glm::vec3(-std::sin(cc.heading), 0.0f, -std::cos(cc.heading));
    characterFocus.animState = CameraStateForAnimationState(cc.state());
    characterFocus.moveMagnitude = cc.currentSpeed();
    characterFocus.velocity = cc.velocity;
    characterFocus.animated = true;
    characterFocus.valid = true;

    CameraFocusSystem focusSys;
    focusSys.setFallback(characterFocus);

    // Selection precedence: ECS entity > instanced prop > character fallback.
    // ECS entity selection and prop-instance selection are mutually exclusive
    // (selecting a prop clears selectedEntity_; see Editor::setSelectedPropInstance).
    const ecs::EntityID selE = EntityManager::ValidateOrClear(g_editor.selectedEntity());
    if (selE != ecs::INVALID_ENTITY_ID) {
        focusSys.setSelected(::Editor::focusFromEntity(g_editor.world(), selE));
    } else if (g_editor.hasSelectedPropInstance()) {
        const SimpleWorldRenderer& wr = getWorldRenderer();
        const std::size_t pidx = g_editor.selectedPropInstance();
        if (pidx < wr.getInstanceCount()) {
            EntityFocus prop;
            prop.position = glm::vec3(wr.propInstance(pidx).modelMatrix[3]);
            prop.animated = false;   // static scenery: focus-only, no orbit
            prop.valid = true;
            focusSys.setSelected(prop);
        }
    } else {
        focusSys.setSelected(EntityFocus{});   // clear -> fall back to character
    }

    // forCam.txt §3C: smooth EaseInOut "glide" to frame a freshly-selected entity
    // or prop on a viewport click / outliner row (selection precedence: ECS
    // entity > prop instance > character fallback). Gliding only on a selection
    // CHANGE (tracked per-member) so the follow loop isn't re-triggered every
    // frame. Animated/NPC focus gets a 2.0 m framing radius; static props a 3.0 m
    // radius (matches the spec's defaults). The bot fallback never glides — it
    // resumes continuous follow.
    const bool hasPropNow = g_editor.hasSelectedPropInstance();
    const std::size_t pidxNow = hasPropNow ? g_editor.selectedPropInstance() : std::size_t(0);
    if (selE != m_lastSelectedEntity || pidxNow != m_lastPropIdx) {
        m_lastSelectedEntity = selE;
        m_lastPropIdx        = pidxNow;
        if (focusSys.followingSelection()) {
            const EntityFocus& f = focusSys.activeFocus();
            m_playCamera.FocusOnEntity(f.position, f.animated ? 2.0f : 3.0f,
                                        f.forward, f.animated);
        }
    }

    // Orbit-behind-forward only while the focus is an animated/playable entity.
    m_playCamera.config.orientToCharacterForward = focusSys.activeFocus().animated;
    CameraInput camIn = focusSys.resolveCameraInput();
    const float aspect = (float)m_windowWidth / (float)std::max(1, m_windowHeight);

    // Drive the follow cam's resting pose from the World Settings sliders.
    // Distance = slider value + accumulated zoom offset (scroll zoom offsets
    // from the slider so they never fight). Yaw and pitch are the live
    // easing targets: right-drag tilts pitch / orbits yaw, and both ease back
    // to the slider values when idle. Applied every frame so slider changes
    // take effect immediately, not just on play-enter.
    m_playCamera.config.distance = g_editor.uiState.playCameraDistance + m_cameraZoomOffset;
    const float restingYaw   = g_editor.uiState.playCameraYaw;
    const float restingPitch = g_editor.uiState.playCameraPitch;
    const auto& in = m_inputManager.getInputState();
    // Don't orbit the play-camera while an editor widget (incl. a Translate /
    // Scale gizmo dragged with the left mouse, or a UI splitter) is actively
    // capturing the mouse — otherwise the camera fights the manipulation.
    // Mirrors the WantCapture guard already used at line 261.
    if (in.isMouseButtonDown(Input::MouseButton::RIGHT) && !ImGui::IsAnyItemActive()) {
        m_playCamera.yaw   -= in.mouseDelta.x;
        m_playCamera.pitch = glm::clamp(m_playCamera.pitch - in.mouseDelta.y, -20.0f, 55.0f);
    } else {
        // Ease back to the slider-defined resting yaw / pitch (was: only pitch
        // eased and yaw was never synced, so the Yaw slider did nothing).
        //
        // EXCEPT while the focus is an animated character: there
        // ThirdPersonCamera::update() owns the yaw and slews it behind the
        // character's heading (config.orientToCharacterForward). Easing toward
        // the absolute Yaw slider in the same frame fought that slew, so yaw
        // hovered at a shifting in-between value and the banking roll derived
        // from its per-frame delta kept kicking the viewport image (the
        // "camera shakes violently" report). One owner, no tug-of-war.
        if (!m_playCamera.config.orientToCharacterForward) {
            m_playCamera.yaw = glm::mix(m_playCamera.yaw, restingYaw,
                                        std::min(1.0f, 1.0f - std::exp(-4.0f * dt)));
        }
        m_playCamera.pitch = glm::mix(m_playCamera.pitch, restingPitch,
                                      std::min(1.0f, 1.0f - std::exp(-3.0f * dt)));
    }
    // Scroll zoom offsets from the slider-driven resting distance (tracked in
    // renderSceneOnly). Accumulate into m_cameraZoomOffset so renderSceneOnly's
    // distance = slider + offset re-derivation next frame preserves the zoom.
    if (in.scrollDelta.y != 0.0f) {
        m_cameraZoomOffset = glm::clamp(
            m_cameraZoomOffset - in.scrollDelta.y * 1.5f, -7.0f, 7.0f);
    }

    // Ground clamp: the camera never sinks below the terrain.
    m_playCamera.groundHeightFn = terrain;
    // Don't advance the view camera while an editor widget (incl. a Translate /
    // Scale gizmo being dragged) is actively capturing the mouse — otherwise
    // the camera fights the manipulation. IsAnyItemActive() covers ImGui widgets;
    // the editor's gizmo is GL-drawn (DrawImGui is the Vulkan overlay path, the GL
    // path is GizmoRenderer::Draw) and is NOT an ImGui item, so it sets no active
    // item — we must ALSO check GizmoRenderer::IsDragging() or the camera keeps
    // transforming during a gizmo drag (forPlayer.txt: isGizmoInteracting).
    // Play-mode headless runs no ImGui widgets and no gizmo, so both are false
    // there and normal camera motion is unaffected.
    if (!ImGui::IsAnyItemActive() && !GizmoRenderer::IsDragging()) {
        m_playCamera.update(dt, camIn, aspect);
    }


}

CameraState EditorApplication::CameraStateForAnimationState(AnimationState s) const {
    switch (s) {
        case AnimationState::IDLE:       return CameraState::IDLE;
        case AnimationState::WALK:       return CameraState::WALK;
        case AnimationState::RUN:        return CameraState::RUN;
        case AnimationState::JUMP:       return CameraState::JUMP;
        case AnimationState::FALL:       return CameraState::FALL;
        case AnimationState::CROUCH:
        case AnimationState::CROUCH_WALK: return CameraState::CROUCH;
        default: return CameraState::IDLE;
    }
}

void EditorApplication::render(float dt) {
    PROFILE_CPU_SCOPE("Frame.Render");

    // Apply the 1-frame-lagged viewport resize BEFORE the 3D draw so the FBO
    // is resized (then immediately drawn into) before ImGui::Image samples it
    // next frame -- kills the "blank on first click / at clamp limits"
    // splitter race (framebuffer re-alloc was happening post-draw).
    m_renderPipeline.applyPendingViewportResize();

    // verify.txt Module 1: advance the Time-of-Day sequencer clock in real
    // time when playback is active. This runs every frame regardless of which
    // bottom-panel tab is open, so the lighting stays live.
    auto& tod = g_editor.uiState.timeOfDay;
    if (tod.playing) {
        tod.currentTime += dt * tod.playbackSpeed;
        if (tod.currentTime >= 24.0f) tod.currentTime = std::fmod(tod.currentTime, 24.0f);
        if (tod.currentTime < 0.0f)  tod.currentTime += 24.0f;
    }

    renderSceneOnly(dt);
    if (m_uiFrameHook) {
        // The UI hook drives the ImGui frame (panels over the viewport FBO
        // texture) and performs the final buffer swap itself.
        m_uiFrameHook(dt);
    } else {
        glfwSwapBuffers(m_window);
    }
    m_frameCount++;
}

void EditorApplication::renderSceneOnly(float dt) {
    m_renderPipeline.beginFrame(dt);

    // #3: resolve the canonical lighting env ONCE per frame (beginFrame owns
    // the per-frame update); thread it into every renderer instead of letting
    // each renderer call LightingEnvironment::Instance().
    const LightingEnvironment& LEnv = LightingEnvironment::Instance();

    // verify.txt Module 1: evaluate the time-of-day sequencer every frame so
    // the lighting environment reflects the current keyframe interpolation
    // even when the ToD panel tab isn't open.
    UI::EvaluateTimeOfDay(g_editor);

    glm::mat4 view(1.0f);
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), (float)m_windowWidth/m_windowHeight, 0.1f, 1000.0f);
    glm::vec3 cameraPos(0.0f);
    float fov = 60.0f;
    if (m_playController.isLoaded()) {
        // Drive the follow cam's resting pose from the World Settings Distance
        // / Yaw / Pitch sliders every frame so slider changes take effect
        // immediately, not just on play-enter. Yaw was previously NOT synced
        // here (only test.cpp did it), so the Yaw slider had no effect in the
        // editor app. Distance = slider + accumulated zoom offset; the live
        // camera yaw/pitch ease toward the slider values in updatePlayMode.
        // NOTE: yaw/pitch/distance are NOT written here. updatePlayMode() is the
        // single owner of the resting pose: it eases yaw/pitch toward those same
        // World Settings sliders and derives distance = slider + zoom offset.
        // Hard-setting them here (after updatePlayMode already ran) reset the
        // yaw every frame, so the camera's own yaw-delta -> banking roll loop saw
        // a huge |delta| each frame and the viewport image rolled +/-3 deg per
        // frame in alternating directions -- the "camera shaking violently" bug.
        // Only pivot height is a pure render-time resting value with no easing
        // state, so it stays here.
        m_playCamera.config.pivotHeight = g_editor.uiState.playCameraPivotHeight;
        m_playCameraRestingSynced = true;
        const float aspect = (float)m_windowWidth / (float)std::max(1, m_windowHeight);
        view = m_playCamera.getViewMatrix();
        proj = m_playCamera.getProjectionMatrix(aspect);
        cameraPos = m_playCamera.position;
        fov = m_playCamera.currentFOV;          // live lens, expands with speed (forcam.txt §3)
    } else {
        m_playCameraRestingSynced = false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_renderPipeline.getFramebuffer());
    glViewport(0, 0, m_renderPipeline.getViewportWidth(), m_renderPipeline.getViewportHeight());
    glClearColor(0.08f, 0.09f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Wireframe toggle (Editor::setShowWireframe / renderMode). Previously this
    // flag was declared and set but never read by any renderer — so flipping
    // "Wireframe" had no visual effect. Route it to GL polygon mode so
    // primitives + meshes + the play character all wireframe together. Restored
    // to GL_FILL at the end of this call so the ImGui HUD (drawn after
    // renderSceneOnly returns) is never wireframed.
    const bool wf = g_editor.showWireframe();
    m_renderPipeline.setWireframeMode(wf);  // also propagate to the pipeline's flag
    if (wf) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    else    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    {
        PROFILE_CPU_SCOPE("Scene.Pipeline");   // deferred/skybox/vegetation + shadows
        m_renderPipeline.renderScene(view, proj, cameraPos, fov, LEnv);
    }
    // Pass the CSM shadow mapper through so the terrain (and any opt-in
    // splat shader) can both cast and receive cascaded shadow maps. The shadow
    // cascade array is fully populated inside renderScene() above, so it is
    // safe to sample during terrain rendering here.
    {
        PROFILE_CPU_SCOPE("Scene.Terrain");
        m_worldManager.render(view, proj, cameraPos, m_renderPipeline.getShadowMapper(), LEnv);
    }

    // Forward pass: render ECS mesh/model entities the deferred pipeline never
    // draws on its own — everything placed via the spawner (Primitives/Light/
    // Camera), the Content Browser, and Add Component. RenderPipeline above
    // owns the deferred terrain/skybox/vegetation; this layers the editor's
    // forward Renderer (RenderSystem + ModelRenderSystem feed Editor::renderer_
    // via AddRenderable) on top of the viewport FBO bound at line 329.
    // NOTE: bin/engine (test.cpp, the headless smoke) is untouched, so the geo
    // feed gate is unaffected — this only runs in the editor_app viewport.
    {
        PROFILE_CPU_SCOPE("Scene.ECS");   // forward pass: primitives, models, skinned NPCs
        auto& editor = g_editor;          // Editor::Editor (namespace Editor is current)
        // Push the Layers-panel visibility (layerVisible[8]) into both forward
        // renderers so the per-layer cull in RenderSystem/ModelRenderSystem
        // reflects the UI immediately. Done every frame (cheap: 8 bools).
        {
            bool lv[ecs::LayerComponent::kMaxLayers];
            for (int i = 0; i < ecs::LayerComponent::kMaxLayers; ++i)
                lv[i] = g_editor.uiState.layerVisible[i];
            editor.renderSystem().setLayerVisibility(lv);
            editor.modelRenderSystem().setLayerVisibility(lv);
        }
        // Flat viewport modes: renderMode()==2 (Normals) draws a per-face normal
        // overlay; renderMode()==3 (Unlit) draws solid albedo with no lighting.
        // Both swap the ECS static-forward shader, restored after the static
        // forward draw so Lit/Wire are unaffected. Skinned NPCs
        // (renderAnimated, below) keep the lit skin shader — flat skinning is
        // out of scope. Falls back to the main PBR shader if the flat program
        // failed to link at InitShaders time (returns 0).
        const int kViewportMode = g_editor.renderMode();
        const bool kFlatMode = (kViewportMode == 2 || kViewportMode == 3);
        if (kFlatMode) {
            GLuint flatProg = (kViewportMode == 3)
                ? ShaderManager::GetUnlitShaderProgram()
                : ShaderManager::GetNormalsShaderProgram();
            GLuint prog = flatProg ? flatProg : ShaderManager::GetMainShaderProgram();
            editor.renderSystem().setDefaultShaderProgram(prog);
            editor.modelRenderSystem().setDefaultShaderProgram(prog);
        }
        // Configure the forward renderer (fwd = Editor::renderer_) BEFORE any
        // render() call. render() internally calls Renderer::SubmitBatches() ->
        // Render() which flushes + clears the forward batches it just
        // accumulated (Renderer.cpp: SubmitBatches->Render->batches.clear()),
        // so SetCameraMatrices/SetLights/SetFrustumCulling/SetViewport MUST run
        // first. Otherwise forward static models — e.g. any mesh dropped in from
        // the Content Browser via CreateModel() — render with the default 800x600
        // viewport + a stale (identity) camera and never show up in the viewport
        // panel. (An earlier pass only pinned the viewport, but pinned it AFTER
        // render() had already flushed+cleared the batches, so it was a no-op.)
        // NOTE: bin/engine (test.cpp, the headless smoke) is untouched — this
        // only runs in the editor_app viewport.
        Renderer& fwd = editor.renderer();
        fwd.SetCameraMatrices(view, proj);
        fwd.SetLights(nullptr, 0, cameraPos);
        // Frustum Culling: the World Settings panel toggles the deferred
        // pipeline renderer, but the forward ECS renderer that draws spawned
        // Cubes/NPCs/Content-Browser models never got the flag. Drive it
        // from the same WorldManager toggle every frame.
        fwd.SetFrustumCulling(m_worldManager.isCullingEnabled());
        // fwd (Editor::renderer_) is distinct from RenderPipeline::m_renderer,
        // so renderScene()'s glViewport never reaches it and Renderer::Render()
        // would fall back to its 800x600 default viewport — clipping/dropping
        // static models whenever the panel isn't 800x600. Pin it to the panel.
        fwd.SetViewport(0, 0,
                        m_renderPipeline.getViewportWidth(),
                        m_renderPipeline.getViewportHeight());
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        editor.renderSystem().render();
        // Skinned NPCs: hand the skinning shader (the same one RenderPlayCharacter
        // uses) + camera to ModelRenderSystem, tick animator bones, then draw
        // animated models. Animated entities are skipped by render() (above) so
        // they only come out here, skinned via Model::Draw(shader, animator).
        editor.modelRenderSystem().setSkinShader(m_renderPipeline.getModelShader());
        editor.modelRenderSystem().setCameraPosition(cameraPos);
        editor.modelRenderSystem().setViewProjection(view, proj);
        editor.modelRenderSystem().update(ImGui::GetIO().DeltaTime);
        editor.modelRenderSystem().render();            // static ModelComponent batches (skips animated)
        // Safety net: render() above already flushed+cleared the forward batches,
        // so these are currently no-ops — kept so any future path that only
        // AddRenderable (without an inner Submit) still lands on the screen.
        fwd.SubmitBatches();
        fwd.Render();
        if (kFlatMode) {
            editor.renderSystem().setDefaultShaderProgram(ShaderManager::GetMainShaderProgram());
            editor.modelRenderSystem().setDefaultShaderProgram(ShaderManager::GetMainShaderProgram());
        }
        editor.modelRenderSystem().renderAnimated();    // overlay skinned NPCs (depth-tested)

        // T5 viewport error surface: under VDbg, scan positioned entities and
        // emit diagnostics the GL renderer can't report itself. The classic
        // "spawned model renders inside the viewport as a tiny/dragged-out
        // quad" is a stale transform cache (matrix col3 != position after a
        // direct field edit) OR a zero/NaN scale - surface it on screen.
        if (g_editor.uiState.viewportDebugStats) {
            g_editor.uiState.viewportErrors.clear();
            g_editor.world().forEach<ecs::TransformComponent, ecs::ModelComponent>(
                [&](ecs::EntityID eid, ecs::TransformComponent& t, ecs::ModelComponent&) {
                    const glm::mat4 m = t.getModelMatrix();
                    const glm::vec3 mt(m[3].x, m[3].y, m[3].z);
                    if (!std::isfinite(mt.x) || !std::isfinite(mt.y) || !std::isfinite(mt.z)) {
                        g_editor.uiState.viewportErrors.push_back(
                            "[Viewport] entity " + std::to_string(eid) +
                            " has NaN/inf model matrix (transform not finite)");
                    } else if (glm::length(mt - t.position) > 1e-4f) {
                        g_editor.uiState.viewportErrors.push_back(
                            "[Viewport] entity " + std::to_string(eid) +
                            " STALE transform: matrix (" + std::to_string(mt.x) +
                            "," + std::to_string(mt.y) + "," + std::to_string(mt.z) +
                            ") != position (" + std::to_string(t.position.x) + "," +
                            std::to_string(t.position.y) + "," + std::to_string(t.position.z) +
                            ") - cache not invalidated (world-vs-local mismatch)");
                    }
                    if (glm::compMin(glm::abs(t.scale)) <= 0.0f) {
                        g_editor.uiState.viewportErrors.push_back(
                            "[Viewport] entity " + std::to_string(eid) +
                            " zero/negative scale " + std::to_string(t.scale.x) + "," +
                            std::to_string(t.scale.y) + "," + std::to_string(t.scale.z));
                    }
                });
        }

        // Viewport diagnostic: confirm how many scene entities reached the
        // forward renderer (mesh/model/NPC/twin) + how many batches AddRenderable
        // queued. Read via the Console panel in-viewport to narrow Spawn Cube /
        // selected-entity "doesn't appear" bugs to renderer-vs-GL/FBO/depth.
        // (Read GetBatchCount BEFORE ClearBatches nulls the batch list.)
        if (g_editor.uiState.viewportDebugStats) {
            int meshN = 0, modelN = 0, twinN = 0;
            g_editor.world().forEach<ecs::TransformComponent, ecs::MeshComponent>(
                [&](auto&&...) { meshN++; });
            g_editor.world().forEach<ecs::TransformComponent, ecs::ModelComponent>(
                [&](auto&&...) { modelN++; });
            g_editor.world().forEach<ecs::TransformComponent, ecs::GeospatialComponent>(
                [&](auto&&...) { twinN++; });
            EditorConsole::Log("[ViewportDebug] " + std::string(g_editor.showWireframe() ? "WIRE" : "LIT")
                               + " | mesh=" + std::to_string(meshN)
                               + " model/NPC=" + std::to_string(modelN)
                               + " twins=" + std::to_string(twinN)
                               + " | forward batches=" + std::to_string(fwd.GetBatchCount()), 0);
        }
        fwd.ClearBatches();
    }

    {
        PROFILE_CPU_SCOPE("Scene.PlayCharacter");
        renderPlayCharacter(view, proj, cameraPos);
    }

    // ----- Grid + selection gizmo -----
    // Rendered DIRECTLY into the viewport FBO (still bound here) so they appear
    // on the FBO texture that ImGui displays via ImGui::Image in RenderViewport.
    // Previously these were called from RenderViewport AFTER the FBO was unbound
    // (default framebuffer), which meant ImGui's viewport texture image was drawn
    // ON TOP of them — the grid/gizmo were always hidden.
    GridRenderer::Draw(view, proj, ShaderManager::GetMainShaderProgram());

    if (g_editor.showGizmo() && g_editor.gizmoType() != GizmoRenderer::GizmoType::None) {
        // AAA multi-pass gizmo (forGizmo.txt Step 3): the gizmo shader carries
        // uHoverGlow (from the easing loop above) + uIsOccludedPass. GizmoRenderer::Draw
        // sets uView/uProjection/uModel/uColor per axis itself; we only layer the
        // X-Ray dual-pass depth state + composite hover glow. Falls back to a flat
        // single Draw if the AAA program failed to link (GetGizmoShaderProgram()==0).
        auto drawGizmoXRay = [&](const glm::vec3& pos, const glm::quat& rot) {
            // Outline halo first (additive, depth-test OFF) so it wraps the
            // handles even behind scene geometry, then the solid 2-pass X-Ray
            // gizmo draws on top. State is saved/restored inside DrawOutline.
            GizmoRenderer::DrawOutline(pos, 1.0f, rot, view, proj, g_editor.gizmoType());
            GLuint prog = ShaderManager::GetGizmoShaderProgram();
            if (!prog) {
                GizmoRenderer::Draw(pos, 1.0f, rot, view, proj, prog, g_editor.gizmoType());
                return;
            }
            const auto& gv = g_editor.uiState.gizmoGlowValues;
            float glow = glm::max(glm::max(gv.x, gv.y), gv.z);
            ShaderManager::SetFloat(prog, "uHoverGlow", glow);

            glEnable(GL_DEPTH_TEST);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            // Pass 1: X-Ray behind geometry (desaturated / dashed at 35% alpha)
            ShaderManager::SetFloat(prog, "uIsOccludedPass", 1.0f);
            glDepthFunc(GL_GREATER);
            glDepthMask(GL_FALSE);
            GizmoRenderer::Draw(pos, 1.0f, rot, view, proj, prog, g_editor.gizmoType());
            // Pass 2: solid front face (depth writes on)
            ShaderManager::SetFloat(prog, "uIsOccludedPass", 0.0f);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_TRUE);
            GizmoRenderer::Draw(pos, 1.0f, rot, view, proj, prog, g_editor.gizmoType());

            glDisable(GL_BLEND);
        };
        ecs::EntityID selected = EntityManager::ValidateOrClear(g_editor.selectedEntity());
        if (selected != ecs::INVALID_ENTITY_ID) {
            ecs::World& w = g_editor.world();
            ecs::TransformComponent* t =
                w.getComponentArchetype<ecs::TransformComponent>(selected);
            if (t) {
                glm::vec3 pos = t->position;
                glm::quat rot = (g_editor.spaceType() == GizmoRenderer::SpaceType::World)
                                   ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
                                   : glm::quat(t->rotation);
                drawGizmoXRay(pos, rot);
            }
        } else if (g_editor.hasSelectedPropInstance()) {
            // forRenderer.txt Step C (grounded; GizmoRenderer replaces the
            // spec's invented ImGuizmo::Manipulate). Draw the gizmo on the
            // selected instanced prop. pos = modelMatrix col3; rot = normalized
            // column basis -> quat (instance is T*R*S, uniform scale). GL-
            // runtime only: visual verified in-viewport by the editor user.
            const SimpleWorldRenderer& wr = getWorldRenderer();
            const SimpleModelInstance& inst =
                wr.propInstance(g_editor.selectedPropInstance());
            const glm::mat4& mm = inst.modelMatrix;
            const glm::vec3 pos = glm::vec3(mm[3]);
            glm::mat3 r = glm::mat3(mm);
            r[0] = glm::normalize(r[0]); r[1] = glm::normalize(r[1]);
            r[2] = glm::normalize(r[2]);
            const glm::quat rot =
                (g_editor.spaceType() == GizmoRenderer::SpaceType::World)
                    ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
                    : glm::quat_cast(r);
            drawGizmoXRay(pos, rot);
        }
    }

    // ---- verify.txt Module 3: Actor Layer Group Isolation — AABB highlights ----
    // When isolation mode is active, draw low-impact white wireframe boxes
    // around every prop instance whose layer matches the isolated layer, so
    // the artist instantly sees which objects are unlocked/selectable.
    if (g_editor.uiState.exclusiveIsolatedLayerIndex >= 0) {
        int activeLayer = g_editor.uiState.exclusiveIsolatedLayerIndex;
        if (WorldObjectManager* wom = m_worldManager.getWorldObjectManager()) {
            const SimpleWorldRenderer& srr = wom->getSimpleWorldRenderer();
            const auto& instances = srr.getActiveInstances();
            if (!instances.empty()) {
                g_debugRenderer.beginFrame(view, proj);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LEQUAL);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                const glm::vec3 highlightCol(0.9f, 0.9f, 0.95f);
                for (const auto& inst : instances) {
                    if (inst.layer != activeLayer) continue;
                    // Recover world-space AABB from the instance matrix +
                    // the model's local bounding box (model-space origin anchored
                    // at placement, uniform scale).
                    const glm::vec3 pos = glm::vec3(inst.modelMatrix[3]);
                    // Uniform scale = length of any basis column.
                    float scale = glm::length(glm::vec3(inst.modelMatrix[0]));
                    if (scale <= 0.0f) scale = 1.0f;
                    if (inst.model) {
                        const BoundingBox& bb = inst.model->GetBoundingBox();
                        glm::vec3 bmin = glm::vec3(bb.min) * scale + pos;
                        glm::vec3 bmax = glm::vec3(bb.max) * scale + pos;
                        glm::vec3 center = (bmin + bmax) * 0.5f;
                        glm::vec3 size = bmax - bmin;
                        g_debugRenderer.drawBox(center, size, highlightCol, true);
                    }
                }
                g_debugRenderer.endFrame();
            }
        }
    }

    // Restore solid rasterization so the ImGui viewport overlay / editor HUD
    // that runs after this call is not drawn as wireframes.
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // Expose this frame's camera state to the UI hook (viewport overlay).
    m_lastView = view;
    m_lastProjection = proj;
    m_lastCameraPos = cameraPos;
    m_lastCameraTarget = m_playController.isLoaded() ? m_playCamera.target : glm::vec3(0.0f);

    if (!m_uiFrameHook) {
        // No ImGui UI loop to display the viewport texture: blit the scene
        // FBO to the default framebuffer before the buffer swap.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_renderPipeline.getFramebuffer());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, m_renderPipeline.getViewportWidth(), m_renderPipeline.getViewportHeight(),
                          0, 0, m_windowWidth, m_windowHeight,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    // With a UI hook the ImGui overlay draws over the WINDOW's default
    // framebuffer, so unbind the viewport FBO before the UI pass.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_windowWidth, m_windowHeight);

    m_renderPipeline.renderUI();
    m_renderPipeline.endFrame();
}

void EditorApplication::renderPlayCharacter(const glm::mat4& view, const glm::mat4& proj,
                                             const glm::vec3& cameraPos) {
    if (!m_playController.isLoaded()) return;
    if (!m_playRenderModel) return;

    Shader* shader = m_renderPipeline.getModelShader();
    if (!shader) return;

    AnimatedCharacter& cc = m_playController.character();

    // Todo 7 + forUI.txt §1 fix: prefer the artist's explicitly-loaded model
    // (playCharacterModel / m_playModelPath) so the viewport mesh matches the
    // Animator (same FBX -> skeleton and mesh never mismatch). The AssetKind
    // stand-in (excavator/utility_car/etc.) is only the fallback when no model
    // has been loaded, and a missing kind-asset still falls through to the bot.fbx
    // fallback below so we always render a valid mesh.
    const std::string targetPath = resolvePlayCharacterMeshPath(
        m_playModelPath, cc.geospatialAssetKind());
    if (!m_playRenderModel || m_lastRequestedMeshPath != targetPath) {
        m_lastRequestedMeshPath = targetPath;
        m_playRenderModel = std::make_unique<Model>(targetPath);
        if (m_playRenderModel->GetMeshCount() == 0) {
            if (targetPath != fallbackMeshPath()) {
                std::cerr << "[EditorApplication] Mesh '" << targetPath
                          << "' unavailable, falling back to "
                          << fallbackMeshPath() << "\n";
                m_playRenderModel = std::make_unique<Model>(fallbackMeshPath());
                m_lastRequestedMeshPath = fallbackMeshPath();
            } else {
                m_playRenderModel.reset();
                std::cerr << "[EditorApplication] Play model '" << targetPath
                          << "' has no meshes\n";
            }
        }
    }
    if (!m_playRenderModel) return;

    // World transform: position + heading + scale. modelMatrix() applies the
    // 180-degree facing flip so the bot renders facing its logical forward
    // (the follow camera sits behind it, so the model must face away).
    const glm::mat4 model = cc.modelMatrix();

    shader->use();
    shader->setMat4("projection", proj);
    shader->setMat4("view", view);
    shader->setMat4("model", model);
    shader->setVec3("lightPos", glm::vec3(10.0f, 15.0f, 10.0f));
    shader->setVec3("viewPos", cameraPos);
    shader->setInt("uDisableInstancing", 1);
    shader->setInt("uDisableSkinning", 0);
    shader->setInt("uPaletteSize", 256);
    shader->setInt("uShowDebug", 0);

    Animator* animator = cc.animator();
    if (animator) m_playRenderModel->Draw(*shader, *animator);
    else m_playRenderModel->DrawStatic(*shader);

    // ---- Skeleton debug overlay ----
    if (g_editor.uiState.showSkeletonDebug && cc.skeleton() && animator) {
        const glm::mat4& modelMat = cc.modelMatrix();
        const auto& finalMats = cc.debugFinalBones();
        if (!finalMats.empty()) {
            const Skeleton* skel = cc.skeleton();
            std::function<void(const AssimpNodeData&, const glm::mat4&)>
                drawBoneTree = [&](const AssimpNodeData& node, const glm::mat4& parentWorld) {
                    glm::mat4 localTransform = node.transform;
                    glm::mat4 worldTransform = parentWorld * localTransform;

                    if (node.boneIndex >= 0 && node.boneIndex < (int)finalMats.size()) {
                        const glm::mat4& boneModel = finalMats[node.boneIndex];
                        glm::vec3 bonePos = modelMat * boneModel * glm::vec4(0, 0, 0, 1);
                        if (node.boneIndex == skel->rootBoneIndex) {
                            const float sz = 0.1f;
                            g_debugRenderer.drawLine(bonePos - glm::vec3(sz,0,0), bonePos + glm::vec3(sz,0,0), glm::vec3(1,0.3f,0.3f));
                            g_debugRenderer.drawLine(bonePos - glm::vec3(0,0,sz), bonePos + glm::vec3(0,0,sz), glm::vec3(1,0.3f,0.3f));
                            g_debugRenderer.drawLine(bonePos - glm::vec3(0,sz,0), bonePos + glm::vec3(0,sz,0), glm::vec3(1,0.3f,0.3f));
                        }
                        // Draw bone axes (colored cross at each joint)
                        if (g_editor.uiState.showBoneAxes) {
                            const float axisSz = 0.05f;
                            g_debugRenderer.drawLine(bonePos, bonePos + glm::vec3(axisSz, 0, 0), glm::vec3(1, 0.2f, 0.2f));
                            g_debugRenderer.drawLine(bonePos, bonePos + glm::vec3(0, axisSz, 0), glm::vec3(0.2f, 1, 0.2f));
                            g_debugRenderer.drawLine(bonePos, bonePos + glm::vec3(0, 0, axisSz), glm::vec3(0.2f, 0.2f, 1));
                        }
                        glm::vec3 parentPos = bonePos;
                        for (const auto& child : node.children) {
                            int childBoneIdx = child.boneIndex >= 0 ? child.boneIndex : 0;
                            if (childBoneIdx < (int)finalMats.size()) {
                                glm::vec3 childPos = modelMat * finalMats[childBoneIdx] * glm::vec4(0,0,0,1);
                                g_debugRenderer.drawLine(parentPos, childPos, glm::vec3(0.9f, 0.9f, 0.2f));
                            }
                            drawBoneTree(child, worldTransform);
                        }
                    } else {
                        for (const auto& child : node.children) {
                            drawBoneTree(child, worldTransform);
                        }
                    }
                };
            drawBoneTree(skel->rootNode, modelMat);
        }
    }
}

bool EditorApplication::loadScene(const std::string& scenePath) {
    std::cout << "[EditorApplication] Loading scene: " << scenePath << std::endl;
    return true;
}

bool EditorApplication::saveScene(const std::string& scenePath) {
    std::cout << "[EditorApplication] Saving scene: " << scenePath << std::endl;
    return true;
}

bool EditorApplication::newScene() {
    std::cout << "[EditorApplication] Creating new scene" << std::endl;
    return true;
}

void EditorApplication::enterPlayMode() {
    if (m_isPlaying) return;
    // The character is already loaded at startup - play mode just hands the
    // player control, so the bot stays visible when exiting.
    if (!m_playController.isLoaded()) {
        if (!m_playController.load(m_playModelPath, m_playClipsDir)) {
            std::cerr << "[EditorApplication] Play Mode: failed to load character '"
                      << m_playModelPath << "'" << std::endl;
            m_isPlaying = false;
            return;
        }
    }
    if (m_playController.isLoaded()) {
        // Spawn the bot ON the terrain surface (physics floor snap) so it
        // doesn't start hovering above a valley / tree line at y=0.
        AnimatedCharacter& ch = m_playController.character();
        const float surf = m_worldManager.getSurfaceHeightAt(ch.position.x, ch.position.z);
        ch.position.y = surf;
        ch.grounded = true;
        std::cout << "[Spawn] bot clamped to surface y=" << surf
                  << " at (" << ch.position.x << "," << ch.position.z << ")\n";
    }
    if (!m_playRenderModel) {
        m_playRenderModel = std::make_unique<Model>(m_playModelPath);
        if (m_playRenderModel->GetMeshCount() == 0) {
            std::cerr << "[EditorApplication] Play Mode: render model '"
                      << m_playModelPath << "' has no meshes" << std::endl;
            m_playRenderModel.reset();
        }
        m_lastRequestedMeshPath = m_playModelPath;   // seed mesh-swap cache (Todo 7)
    }
    m_isPlaying = true;
    m_cameraZoomOffset = 0.0f;  // reset zoom offset from the slider on play-enter
    std::cout << "[EditorApplication] Entering Play Mode (character ready, "
              << m_playController.character().boneCount() << " bones)" << std::endl;
}

void EditorApplication::exitPlayMode() {
    if (!m_isPlaying) return;
    m_isPlaying = false;
    std::cout << "[EditorApplication] Exiting Play Mode" << std::endl;
}

void EditorApplication::togglePlayMode() {
    if (m_isPlaying) exitPlayMode();
    else enterPlayMode();
}

void EditorApplication::onWindowResize(int width, int height) {
    m_windowWidth = width;
    m_windowHeight = height;
    m_renderPipeline.resizeViewport(width, height);
}

void EditorApplication::onKeyInput(int key, int action) {
    // Handle key input
}

void EditorApplication::createDefaultScene() {
    // Create default scene
}

} // namespace Editor
