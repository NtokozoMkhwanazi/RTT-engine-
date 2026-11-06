#ifndef EDITOR_STATE_H
#define EDITOR_STATE_H

#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "ecs/systems/Systems.h"
#include "ecs/systems/ModelRenderSystem.h"
#include "cameraSystem/flyCamera.h"

// ThirdPersonCamera selection-focus tracking (ecs::EntityID) is compiled in
// this translation unit, which already includes ECS headers via EditorState
// (ecs/ECS.h). Define the guard so the camera class resolves FocusEntityID /
// INVALID_ENTITY_ID to the real ECS types whenever it is included here.
#define THIRD_PERSON_CAMERA_HAS_ECS
#include "renderer/Renderer.h"
#include "geospatial/GPSTracker.h"
#include "gizmo_renderer.h"
#include "scene_panel_config.h"
#include "editor/config.h"           // forUI.txt: SkyboxUiState/PhysicalSkyUiState + Save/LoadAtmosphereSettings
#include "ecs/systems/GeoTerrainSystem.h"
#include "geo_config_panel.h"
#include "ui_cache.h"
#include "content_browser_assets.h"  // UI::AssetInfo for contentBrowserAssets vector
#include "ecs/JobSystem.h"            // stage 2 (forhotswap.txt): async content-browser scan worker
#include "blueprint_system.h"         // forUI.txt: BlueprintSystem (instantiate + capture)
#include "viewport_framebuffer.h"
#include "imgui_context.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <future>
#include <glm/glm.hpp>   // glm::vec3 in ContentBrowserSpawnAsyncState::Request spawn pose
#include "modelSystem/Model.h"  // A: Model::AsyncLoadHandle member of NPCSpawnAsyncState::Capsule
#include "world/WorldImporter.h"   // LevelImporterAsyncState::StagingCapsule holds WorldImporter::ImportedWorld (forUI.txt Step 1)

// Forward-declared (not pulled in here) so the NPC async Capsule can hold a
// unique_ptr<AnimatedCharacter> without bloating this widely-included header;
// its destructor is defined out-of-line in editor_state.cpp.
class AnimatedCharacter;

// ============================================================================
// Editor Class (RAII wrapper around all editor global state)
// ============================================================================
namespace Editor {

    // forUI.txt: editor interaction mode + viewport brush / governor state.
    enum class EditorMode { Select, Landscape, Foliage, Sequencer };

    // Brush Radius Falloff Curves config shared by the foliage painter.
    struct TerrainBrush {
        float radius = 5.0f;          // brush radius in meters
        float strength = 0.2f;        // vertical displacement intensity factor (forUI.txt Pillar 3)
        int   falloffType = 0;        // 0=Smooth, 1=Linear, 2=Spherical, 3=Flat
        float softFalloffWidth = 0.2f; // [0,1] inner cutoff for falloff curves
    };

    // forUI.txt Governor §A: frame-rate-controlled brush density throttle.
    struct BrushGovernorProfiler {
        float adaptiveDensityMultiplier = 1.0f; // [0.1=Ultra Safe .. 1.0=Max Density]
        float lowFpsHysteresisTimer = 0.0f;
        const float fpsFloorTarget = 58.0f;     // down-throttle trigger
        const float fpsCeilingTarget = 60.5f;   // up-throttle recovery
    };

    // forUI.txt Step 1: non-blocking level importation alongside async character loading.
    struct LevelImporterAsyncState {
        bool isImporting = false;
        bool importComplete = false;
        char sourceWorldPath[512] = "";
        char outputWorkspaceDir[256] = "assets/maps/imported_level";

        // Configurable generation parameters driven by the artist prior to import
        float globalScaleFactor = 1.0f;
        bool forceFloorClamping = true;
        bool autoGeneratePhysicsColliders = true;

        // Thread-safe request/response staging metrics
        struct StagingCapsule {
            WorldImporter::ImportedWorld worldData;
            std::atomic<bool> ready{false};
            std::atomic<bool> failed{false};
            std::string errorLog = "";
        };
        std::shared_ptr<StagingCapsule> activeCapsule;
        ecs::JobSystem::JobHandle taskWorkerHandle;

        // forUI.txt Point 2: thread-safe mesh-decimation telemetry registry.
        // Updated by the SimpleWorldRenderer::loadModel decimation loop; read by
        // the ImGui Progress Monitor in RenderLevelImporterPanel.
        struct ImporterProgressTracker {
            std::atomic<float> currentProgress{0.0f};
            std::atomic<int> processedMeshes{0};
            std::atomic<int> totalMeshesToProcess{0};
            std::atomic<size_t> trianglesBefore{0};
            std::atomic<size_t> trianglesAfter{0};
            std::atomic<const char*> activeMeshName{""};
        };
        ImporterProgressTracker progressTelemetry;
    };

class Editor {
public:
    Editor();
    ~Editor();

    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;

    bool initialize();
    void shutdown();
    bool isInitialized() const noexcept { return initialized_; }

    // ECS / rendering accessors
    ecs::World& world() noexcept { return world_; }
    const ecs::World& world() const noexcept { return world_; }
    Renderer& renderer() noexcept { return renderer_; }
    const Renderer& renderer() const noexcept { return renderer_; }
    flyCamera* camera() noexcept { return camera_.get(); }
    const flyCamera* camera() const noexcept { return camera_.get(); }

    ViewportFramebuffer& viewportFramebuffer() noexcept { return viewportFB_; }
    const ViewportFramebuffer& viewportFramebuffer() const noexcept { return viewportFB_; }

    EntityCache& entityCache() noexcept { return entityCache_; }
    const EntityCache& entityCache() const noexcept { return entityCache_; }

    // Subsystem accessors (kept public for minimal call-site churn)
    ecs::RenderSystem& renderSystem() noexcept { return renderSystem_; }
    const ecs::RenderSystem& renderSystem() const noexcept { return renderSystem_; }
    ecs::ModelRenderSystem& modelRenderSystem() noexcept { return modelRenderSystem_; }
    const ecs::ModelRenderSystem& modelRenderSystem() const noexcept { return modelRenderSystem_; }
    ecs::LightSystem& lightSystem() noexcept { return lightSystem_; }
    const ecs::LightSystem& lightSystem() const noexcept { return lightSystem_; }
    ecs::GeospatialSystem& geospatialSystem() noexcept { return geospatialSystem_; }
    const ecs::GeospatialSystem& geospatialSystem() const noexcept { return geospatialSystem_; }
    ecs::GeoTerrainSystem& geoTerrainSystem() noexcept { return geoTerrainSystem_; }
    const ecs::GeoTerrainSystem& geoTerrainSystem() const noexcept { return geoTerrainSystem_; }

    // Selection
    ecs::EntityID selectedEntity() const noexcept { return selectedEntity_; }
    void setSelectedEntity(ecs::EntityID id) noexcept;

    // forPlayer.txt Theme 3: selected instanced world-object (tree/rock/etc.).
    // Distinct from selectedEntity() because prop instances live in
    // SimpleWorldRenderer::m_instances, not the ECS world. Surfaced so the
    // Outliner/inspector can display the picked prop; the gizmo-on-prop overlay
    // is a follow-up. Index is into SimpleWorldRenderer's ACTIVE instance list.
    std::size_t selectedPropInstance() const noexcept { return m_selectedPropInstance; }
    bool hasSelectedPropInstance() const noexcept { return m_hasSelectedPropInstance; }
    void setSelectedPropInstance(std::size_t idx) noexcept {
        // forRenderer.txt Step A: selecting a prop releases the ECS entity
        // selection so the gizmo/overlay don't fight for control.
        selectedEntity_ = ecs::INVALID_ENTITY_ID;
        m_selectedPropInstance = idx; m_hasSelectedPropInstance = true;
    }
    void clearSelectedPropInstance() noexcept { m_hasSelectedPropInstance = false; }

    // Gizmo / viewport state
    GizmoRenderer::GizmoType gizmoType() const noexcept { return gizmoType_; }
    void setGizmoType(GizmoRenderer::GizmoType t) noexcept { gizmoType_ = t; }

    GizmoRenderer::SpaceType spaceType() const noexcept { return spaceType_; }
    void setSpaceType(GizmoRenderer::SpaceType t) noexcept { spaceType_ = t; }

    GizmoRenderer::GizmoAxis hoveredAxis() const noexcept { return hoveredAxis_; }
    void setHoveredAxis(GizmoRenderer::GizmoAxis a) noexcept { hoveredAxis_ = a; }

    bool showGrid() const noexcept { return showGrid_; }
    void setShowGrid(bool v) noexcept { showGrid_ = v; }

    bool showGizmo() const noexcept { return showGizmo_; }
    void setShowGizmo(bool v) noexcept { showGizmo_ = v; }

    // Play-character visibility. Hoisted out of test.cpp's runEditorApp so the
    // bot renders in the editor viewport (not only in Play Mode) and the toggle
    // is editor state the UI panel / keybind can read. True so the stand-in
    // character is visible by default while building.
    bool showCharacter() const noexcept { return showCharacter_; }
    void setShowCharacter(bool v) noexcept { showCharacter_ = v; }
    bool& showCharacterRef() noexcept { return showCharacter_; }

    int renderMode() const noexcept { return renderMode_; }
    void setRenderMode(int m) noexcept {
        renderMode_ = m;
        showWireframe_ = (m == 1) ? 1 : 0;  // Wire mode drives the legacy toggle
    }

    int showWireframe() const noexcept { return showWireframe_; }
    void setShowWireframe(int v) noexcept {
        showWireframe_ = v;
        if (v) {
            renderMode_ = 1;            // legacy toggle ON -> Wire
        } else if (renderMode_ == 1) {  // OFF from Wire -> back to Lit
            renderMode_ = 0;
        }
    }

    bool isViewing() const noexcept { return isViewing_; }
    void setIsViewing(bool v) noexcept { isViewing_ = v; }

    glm::vec2& lastMousePos() noexcept { return lastMousePos_; }
    const glm::vec2& lastMousePos() const noexcept { return lastMousePos_; }

    // Game state
    bool isPlaying() const noexcept { return isPlaying_; }
    void setPlaying(bool v) noexcept { isPlaying_ = v; }

    bool wasPlaying() const noexcept { return wasPlaying_; }
    void setWasPlaying(bool v) noexcept { wasPlaying_ = v; }

    float gameSpeed() const noexcept { return gameSpeed_; }
    void setGameSpeed(float s) noexcept { gameSpeed_ = s; }

    float fps() const noexcept { return fps_; }
    void setFps(float f) noexcept { fps_ = f; }

    int frameCount() const noexcept { return frameCount_; }
    void setFrameCount(int c) noexcept { frameCount_ = c; }

    float lastRenderTime() const noexcept { return lastRenderTime_; }
    void setLastRenderTime(float t) noexcept { lastRenderTime_ = t; }

    bool shouldClose() const noexcept { return shouldClose_; }
    void setShouldClose(bool v) noexcept { shouldClose_ = v; }

    // Set true (e.g. via the toolbar "Restart" button) to request a full engine
    // process re-exec. The main loop shuts down, then main() re-execs the
    // process with the original argv (see test.cpp).
    bool wantRestart() const noexcept { return wantRestart_; }
    void setWantRestart(bool v) noexcept { wantRestart_ = v; }

    // Rewind flag — set by the toolbar "Rewind" button to signal the main
    // loop to restart the current animation clip / playback from the beginning.
    bool wantRewind() const noexcept { return wantRewind_; }
    void setWantRewind(bool v) noexcept { wantRewind_ = v; }

    // Viewport selection pulse (forUI.txt "Selected Entity Viewport Pulse"):
    // a short on-screen flash when the selected entity changes, giving immediate
    // spatial feedback across panels. The main loop drives the countdown via
    // advancePulse(dt); RenderViewport reads isPulseActive()/pulseProgress() and
    // draws a flashing ring over the selected entity's world position.
    void triggerSelectionPulse(float seconds) noexcept { pulseTime_ = seconds; }
    bool isPulseActive() const noexcept { return pulseTime_ > 0.0f; }
    // Fractional life (1.0 at fire -> 0.0 at end) for alpha/radius easing.
    float pulseProgress() const noexcept {
        return pulseTime_ > 0.0f ? (pulseTime_ / kPulseDuration) : 0.0f;
    }
    void advancePulse(float dt) noexcept {
        if (pulseTime_ > 0.0f) { pulseTime_ -= dt; if (pulseTime_ < 0.0f) pulseTime_ = 0.0f; }
    }


    bool showAbout() const noexcept { return showAbout_; }
    void setShowAbout(bool v) noexcept { showAbout_ = v; }
    bool& showAboutRef() noexcept { return showAbout_; }

    // forhotswap.txt: lazy worker pool for async content-browser scan.
    // Meyers singleton: only constructed on first use, so headless tests that
    // never trigger an async scan never spawn worker threads.
    ecs::JobSystem& jobSystem() noexcept { static ecs::JobSystem js; return js; }

    // forUI.txt: Blueprint / Prefab authoring system — exposes scan,
    // instantiate, and entity-capture (createBlueprintFromEntity) through a
    // manager facade: editor.blueprintSystem().getManager().
    BlueprintSystem& blueprintSystem() noexcept { return blueprintSystem_; }
    const BlueprintSystem& blueprintSystem() const noexcept { return blueprintSystem_; }

    // Public sub-state structs (kept public to reduce churn; gradually migrate to private)
    struct UIState {
        char searchBuffer[128] = "";
        char pathBuffer[256] = "/Game/Assets";
        int leftPanelTab = 0;
        int bottomPanelTab = 0;
        float leftPanelWidth = 280.0f;
        float rightPanelWidth = 300.0f;
        float bottomPanelHeight = 180.0f;
        // forUI.txt Step 1: transient drag state for the invisible splitters
        // (Step 2 RenderInvisibleSplitter). Reset every frame when not dragging.
        bool isDraggingLeftSplit   = false;
        bool isDraggingBottomSplit = false;
        bool isDraggingRightSplit  = false;
        // forUI.txt escape hatch: set to false to disable the live edge splitters
        // (restores the stable pre-splitter layout) if dragging still interferes
        // with the viewport on a given machine/build. Toggled via g_editor.uiState.
        bool splittersEnabled = true;
        float toolbarHeight = 38.0f;
        float statusBarHeight = 24.0f;
        float minPanelWidth = 200.0f;
        float maxPanelWidth = 600.0f;
        float minBottomHeight = 100.0f;
        float maxBottomHeight = 400.0f;
        bool showOutliner = true;
        bool showDetails = true;
        bool showToolbox = true;
        bool showViewport = true;
        bool showConsole = false;
        bool showContentBrowser = false;
        bool showGameMode = false;
        bool showProfiler = false;
        bool showPreferences = false;
        bool showModelLoader = false;
        bool showSaveAs = false;
        bool showStatsOverlay = false;  // viewport FPS + entity count overlay
        bool showOpenScene = false;
        // Current scene file (relative to the project root).
        char sceneFile[128] = "scene.json";
        // Content browser state (path buffer, scan cache, model list, selection).
        char contentBrowserPath[256] = "assets";
        bool contentBrowserScanned = false;
        std::vector<std::string> contentBrowserModels;
        int contentBrowserSelectedAsset = -1;

        // ---- Categorized content browser ----
        // Asset categorization types (UI::AssetInfo, UI::AssetCategory,
        // UI::AnimationContext) live in content_browser_assets.h. This vector
        // stores the categorised scan results so they persist across frames.
        std::vector<struct UI::AssetInfo> contentBrowserAssets;
        bool contentBrowserAssetsBuilt = false;

        // Stage 2 (forhotswap.txt): asynchronous content-browser scan.
        // ScanAndCategorize() walks the filesystem on a JobSystem worker and
        // moves its result into Capsule::assets; the main thread commits it to
        // contentBrowserAssets once handle.isComplete() (happens-before). The
        // `generation` counter lets us discard a stale worker result if the
        // user re-scans before the previous one finished.
        struct ContentBrowserAsyncState {
            struct Capsule {
                std::vector<UI::AssetInfo> assets;
                std::atomic<bool> ready{false};
                std::atomic<bool> failed{false};
            };
            std::shared_ptr<Capsule> capsule;
            ecs::JobSystem::JobHandle handle;
            bool  isLoading = false;
            int   generation = 0;
        };
        ContentBrowserAsyncState contentBrowserAsync;

        // forUI.txt §3 (NPC pack spawner): async asset-structure resolution.
        // The filesystem walk (resolving the model path + counting clips) runs
        // on a JobSystem worker so the Load Pack button returns immediately; the
        // entity build — which touches the ECS world + ModelManager and must
        // stay on the main thread — commits once the worker finishes. Mirrors
        // ContentBrowserAsyncState so the commit gate is identical.
        // NOTE: ModelManager + ModelSystem::ModelRegistry are now thread-safe
        // (mutex-guarded double-checked load). The worker still stays fs::+clip-
        // count only: the sync Model(const std::string&) ctor performs GL buffer
        // uploads (Model::loadModel -> Mesh ctor -> glGenBuffers), so the heavy
        // FBX/Assimp parse cannot leave the main thread until the registries
        // adopt the LoadAsync/CPU + CreateFromAsync/GL pipeline. That full async
        // offload is the follow-up this thread-safety now unblocks.
        struct NPCSpawnAsyncState {
            struct Capsule {
                std::string resolvedModelPath;
                std::string resolvedClipsDir;
                int         clipCount = 0;
                std::atomic<bool> ready{false};
                std::atomic<bool> failed{false};

                // A: async NPC offload payload (worker thread, strictly GL-free).
                // asyncModel  — Model::AsyncLoadHandle from Model::LoadAsync(mp);
                //   consumed by the main thread's Model::CreateFromAsync() which
                //   does ONLY the GL upload (no FBX re-parse on the main thread).
                // stagedChar — AnimatedCharacter pre-built on the worker
                //   (load + loadLocomotion + setMotionContext, all GL-free) and
                //   moved straight into the entity on commit (no re-parse).
                Model::AsyncLoadHandle asyncModel;
                std::unique_ptr<AnimatedCharacter> stagedChar;
                ~Capsule();  // out-of-line: AnimatedCharacter forward-declared here
            };
            std::shared_ptr<Capsule> capsule;
            ecs::JobSystem::JobHandle handle;
            bool isLoading = false;
            int  npcIndex = -1;   // NPC config entry currently resolving
        };
        NPCSpawnAsyncState npcSpawnAsync;

        // World-object (props) bulk reload via the async pipeline (worker-parse,
        // main-GL commit). Driven by WorldManager::rebuildWorldObjectsAsync();
        // the actual load state lives on the renderer/WOM and is polled here
        // only to gate the "Resolving..." bar + block conflicting UI.
        struct WorldObjectsAsyncState {
            bool  isLoading = false;   // Rebuild Props dispatched, not yet done
            bool  finished = false;
            float progress = 0.0f;
        };
        WorldObjectsAsyncState worldObjectsAsync;

        // ----- Staged "Reload Complete Stage" job -----
        // The Engine Config button used to run initializeTerrain() +
        // rebuildWorldObjects() inline inside the ImGui button handler: the
        // Assimp parses, texture uploads and chunk rebuild all happened in one
        // frame, freezing the editor for seconds ("60 FPS -> 1 FPS"). The job
        // is now QUEUED by the button and pumped one stage per frame from
        // EditorApplication::update(); the landscape step is the single
        // remaining (inherently GL-bound) blocking call and the props step is
        // the existing worker-parse / main-GL-commit async path, so this state
        // drives a live "Resolving…" bar instead of a frozen window.
        struct StageReloadState {
            bool  active      = false;  // queued or running
            int   stage       = 0;      // 0 = landscape pending, 1 = props resolving
            bool  terrainOnly = false;  // Load Ground: skip the props stage
            float terrainMs   = 0.0f;   // measured landscape rebuild time (last run)
            float progress    = 0.0f;   // 0..1 for the resolve bar
        };
        StageReloadState stageReload;

        // Content-browser model spawn offloaded to the async pipeline: the heavy
        // Assimp parse + texture decode run on the bounded JobSystem worker pool
        // (with a 2K texture-streaming cap), while ONLY the GL upload + entity
        // wiring happen on the main thread — committed time-sliced per frame so
        // spawning a heavy prop from the browser never drops the editor to 2-3 FPS.
        // A "Resolving…" bar (ContentBrowserSpawnResolveBar) renders per request.
        struct ContentBrowserSpawnAsyncState {
            enum class Phase : uint8_t { Parsing, ReadyToCommit, Done, Failed };
            struct Request {
                std::string displayName;
                std::string assetPath;
                bool isCharacter = false;
                glm::vec3 pos{0,0,0}, scale{1,1,1}, rotation{0,0,0};
                // Worker-written parse progress (0..1) read by the bar.
                std::shared_ptr<std::atomic<float>> progress;
                // Worker delivers the parsed AsyncModelData here; the main
                // thread consumes it once ready == non-blocking (promise fulfilled
                // before the JobSystem completion future the worker also satisfies).
                std::shared_ptr<std::promise<std::unique_ptr<AsyncModelData>>> promise;
                std::future<std::unique_ptr<AsyncModelData>> dataFuture;
                Phase phase = Phase::Parsing;
                std::string error;
            };
            std::deque<Request> requests;
        };
        ContentBrowserSpawnAsyncState contentBrowserSpawn;

        // Generic async model loader (Load 3D Model dialog).
        struct ModelLoaderAsyncState {
            enum class Phase : uint8_t { Parsing, ReadyToCommit, Done, Failed };
            struct Request {
                std::string displayName;
                std::string assetPath;
                bool isAnimated = false;
                int animationIndex = 0;
                glm::vec3 pos{0,0,0}, scale{1,1,1}, rotation{0,0,0};
                std::shared_ptr<std::atomic<float>> progress;
                std::shared_ptr<std::promise<std::unique_ptr<AsyncModelData>>> promise;
                std::future<std::unique_ptr<AsyncModelData>> dataFuture;
                Phase phase = Phase::Parsing;
                std::string error;
            };
            std::deque<Request> requests;
        };
        ModelLoaderAsyncState modelLoaderAsync;

        // Engine-layer async scene/world load (SceneManager::LoadScene).
        // Mirrors the content-browser + NPC resolve: heavy Assimp parse + texture
        // decode cap run on the bounded JobSystem pool; the GL upload (CreateFromAsync
        // + registerModel) + ModelHandle attach to the restored entity commits on the
        // main thread, time-sliced per frame by SceneManager::CommitSceneModelLoads().
        // `active` is flipped on by the "Open Scene" UI before LoadScene() so the
        // synchronous boot path (WorldManager::initialize / WorldObjectPhysics tests)
        // is untouched — only editor-driven scene opens defer their model loads.
        struct SceneLoadAsyncState {
            enum class Phase : uint8_t { Parsing, ReadyToCommit, Done, Failed };
            struct Request {
                std::string assetPath;
                uint32_t     entityID = 0;                 // ModelComponent host entity
                std::shared_ptr<std::atomic<float>> progress;
                std::shared_ptr<std::promise<std::unique_ptr<AsyncModelData>>> promise;
                std::future<std::unique_ptr<AsyncModelData>> dataFuture;
                Phase phase = Phase::Parsing;
                std::string error;
            };
            std::deque<Request> requests;
            bool active = false;                            // opt-in to deferred model load
            bool isLoading = false;                        // bar is live while pending
            size_t totalIssued = 0;                        // enqueued this scene open
        };
        SceneLoadAsyncState sceneLoadAsync;

        // Play-mode camera: 0 Free, 1 Third-person, 2 First-person.
        int playCameraMode = 0;
        // Camera tuning (World Settings -> Camera Settings sliders). Distance is
        // two-way synced with the live cam (scroll zoom updates it); Pitch is the
        // resting look angle; Yaw is the world-space orbit heading (Third-person).
        // Shared by every viewport camera (Free / Third-person / First-person).
        float playCameraDistance = 4.0f;
        float playCameraPitch = 10.0f;
        float playCameraYaw = -90.0f;
        // Look-at height above the entity feet (chest/head level). Raising this
        // moves the look-at target up so the camera frames the entity's face/torso
        // rather than looking up from below. Artists tune this directly when the
        // default chest framing sits too low on a given character/prop.
        float playCameraPivotHeight = 1.5f;  // look-at height above entity feet
        // Gizmo snapping (viewport toolbar + World Settings panel). When
        // snapEnabled the transform gizmo drags snap to these increments:
        // translate in world units, rotate in degrees, scale as a factor.
        bool snapEnabled = false;
        float snapTranslate = 1.0f;
        float snapRotate = 15.0f;
        float snapScale = 0.5f;

        // ---- World grid settings (synced from UIConfig, used by GridRenderer) -----
        float gridSpacing = 1.0f;
        int gridSize = 100;

        // Auto-focus the free camera on selected entities (one-shot after select,
        // or persistent when focusSelectionOnSelect=true).
        bool focusSelectionOnSelect = true;

        // Skeleton debug visualization toggles (PlayMode HUD -> render pipeline)
        bool showSkeletonDebug = false;
        bool showBoneNames = false;
        bool showBoneAxes = false;

        // UI zoom / scale factor (0.5–2.0). When changed, the entire editor
        // layout is scaled proportionally: fonts, panel sizes, widget padding,
        // and tab/button heights all grow or shrink together. The panel
        // *structure* (which panels exist and where they dock) never changes.
        float uiScale = 1.0f;

        // Viewport diagnostic: when on, renderSceneOnly logs the forward
        // renderer's entity/batch counts to the in-viewport Console so Spawn
        // Cube / NPC / selected-entity visibility bugs can be narrowed to
        // renderer-vs-GL/FBO/depth instead of guessed at.
        bool viewportDebugStats = false;

        // forGizmo.txt: per-axis hover-glow targets driven by the editor update
        // loop (exponential decay -> frame-rate-independent "breathing"). Streamed
        // to the gizmo shader as a composite uHoverGlow (Draw draws all axes in
        // one call, so per-axis glow isn't possible without DrawAxisSegment).
        struct AxisInterpolator {
            float x = 0.0f, y = 0.0f, z = 0.0f;
            const float approachSpeed = 18.0f; // ~5-8 frames to settle at 60fps
        };
        AxisInterpolator gizmoGlowValues;

        // T5 in-viewport error surface: populated by renderSceneOnly when
        // viewportDebugStats is on (degenerate/stale transforms the GL renderer
        // can't report itself), drawn as a red overlay by RenderViewport so the
        // artist sees "entity X matrix != position" instead of guessing the
        // world-vs-local bleed that makes a prop render in the wrong quad.
        std::vector<std::string> viewportErrors;

        // ----- Layer visibility / lock state (Layers panel) -----
        bool layerVisible[8] = {true, true, true, true, true, true, true, true};
        bool layerLocked[8]   = {false, false, false, false, false, false, false, false};

        // Play-mode character config (engine-general, NOT geo-only). These drive
        // the editor viewport stand-in character: a general model path + clips
        // dir the artist can point at any skinned FBX. AssetKind is an optional
        // convenience preset (dropdown in the Character panel) that fills the
        // path; the path itself is an arbitrary string.
        char playCharacterModel[256] = "";   // empty: viewport stays empty till an artist loads a model (forhotswap.txt)
        char playClipsDir[256]       = "assets";
        bool showCharacterPanel      = true;
        // Set by the Character inspector's model-path / clips-dir InputText when
        // the user commits an edit (Enter or focus-lost). Drains once
        // EditorApplication::syncPlayCharacterPath() has hot-reloaded the bot.
        bool playReloadRequested     = false;

        // Handle to the character entity in the ECS outliner. When the model
        // path is reloaded, the old entity is re-spawned or re-pointed at the
        // new model so it always appears in the Outliner. Set to 0 (invalid)
        // when the model file is missing or the character is removed.
        ecs::EntityID characterEntity = 0;

        // ----- Layout toggles -----
        bool maximizeViewport = false;

        // ----- World / culling / LOD debug tuning (World Settings panel) -----
        struct WorldSettings {
            // Hard cap on the number of visible prop instances drawn per frame
            // (default 500). The culling pass never submits more than this many
            // instances across all prop meshes, keeping the viewport interactive
            // with huge procedural worlds. Exposed as a slider in World Settings.
            int maxVisibleInstances = 500;
            // LOD distance bands, expressed as a fraction of each prop mesh's cull
            // radius (radius is the per-mesh fade/drop distance). Surviving instances
            // are tinted by band when cullingDebug: (0, band1] green / LOD0, (band1,
            // band2] yellow / LOD1, (band2, band3] red / LOD2, past band3 dropped.
            float lodBand1 = 0.40f;
            float lodBand2 = 0.70f;
            float lodBand3 = 0.92f;
            // When true: tint each visible prop by its LOD band, draw a translucent
            // AABB bounds box around every visible prop, and print a visible-count
            // overlay so you can see exactly what the culler kept/dropped.
            bool cullingDebug = false;
        } worldSettings;

        // forhotswap.txt Step 1: dynamic (string-keyed, searchable) character
        // clip UI state, replacing the hardcoded ClipSlot (0..CLIP_SLOTS-1)
        // preview strip. Refreshed whenever a hot-swap finishes or the active
        // MotionContext changes, so the Details panel reflects the live asset set.
        struct DynamicCharacterUIState {
            // Tracks the active indices selected for previews across contexts.
            int selectedContextIndex = 0;
            int selectedPreviewClipIdx = -1;

            // Text filter typed by the artist to narrow the active-context list.
            // Cleared on every hot-swap completion (see syncPlayCharacterPath).
            char clipSearchFilter[128] = "";

            // Cached clip names for the active MotionContext, so we don't re-query
            // strings every frame while the inspector is open.
            std::vector<std::string> activeContextClipNames;

            // Dirty bit: activeContextClipNames is rebuilt next frame.
            bool uiMetadataNeedsRefresh = true;
        };
        DynamicCharacterUIState characterUIData;

        // ---- forUI.txt: brush / foliage painter / governor state ----
        EditorMode currentMode = EditorMode::Select;
        float foliageBrushRadius = 5.0f;
        int foliageBrushDensity = 24;
        int selectedFoliageType = 0;  // legacy index (kept for compatibility)
        // verify.txt Step 2/3: STRING-based foliage asset picker. Artists
        // select a type by name from the dynamic config dictionary (e.g.
        // "TREE_PINE", "M_StreetLamp"). The brush stroke routes this string
        // directly to placeDynamicObjectByString — NO enum hardcoded limits.
        char selectedFoliageTypeString[128] = "TREE_PINE";
        TerrainBrush terrainBrush;
        bool surfaceSnappingEnabled = false; // forUI.txt Pillar 4: End-key drop-to-floor + live gizmo snap
        BrushGovernorProfiler brushGovernor;
        LevelImporterAsyncState levelImporterAsync;

        // ── forUI.txt: Blueprint / Prefab authoring UI state ──────────────
        // Tracks the active blueprint manager staging, folder templates, and
        // in-viewport spawning queues used by the "Prefabs" tab in the bottom
        // panel and the Outliner context-menu export.
        struct BlueprintUIStaging {
            char bpNameBuffer[128] = "";       // name typed into the Save modal
            char bpSaveFileName[256] = "";      // last save path (reserved for future use)
            bool showSaveModal = false;        // open the "Save Blueprint Template" modal?
            bool metadataRefreshNeeded = true; // re-scan assets/blueprints/ next frame
            std::string selectedBlueprintPath = ""; // full path of the highlighted row
            std::vector<std::string> discoveredBlueprintFiles; // .json files on disk
        };
        BlueprintUIStaging blueprintState;

        // verify.txt: multi-selection buffer. When Ctrl+Click is used in the
        // Outliner, entities are added/removed here. RenderDetailsPanel shows
        // "[Mixed Values]" for properties that differ across the selection.
        std::vector<ecs::EntityID> selectedEntities;

        // Module 1 (verify.txt): Time-of-Day sequencer state.
        struct TimeOfDayState {
            float currentTime = 12.0f;        // 0–24 hour clock
            bool  playing = false;
            float playbackSpeed = 1.0f;       // hours per second of real time
            struct Keyframe {
                float hour = 12.0f;           // time of day (0–24)
                glm::vec3 sunDirection = glm::normalize(glm::vec3(0.5f, 1.0f, 0.3f));
                glm::vec3 sunColor = glm::vec3(1.0f, 0.98f, 0.92f);
                float sunIntensity = 1.0f;
                glm::vec3 fogHorizon = glm::vec3(0.45f, 0.38f, 0.32f);
                glm::vec3 fogZenith = glm::vec3(0.20f, 0.30f, 0.45f);
                float fogFar = 900.0f;
            };
            std::vector<Keyframe> keyframes = {
                Keyframe{ 6.0f, glm::normalize(glm::vec3(0.2f, 0.8f, 0.2f)), glm::vec3(1.0f, 0.82f, 0.60f), 2.0f, glm::vec3(0.50f, 0.38f, 0.32f), glm::vec3(0.20f, 0.30f, 0.45f), 800.0f },
                Keyframe{12.0f, glm::normalize(glm::vec3(0.5f, 1.0f, 0.3f)), glm::vec3(1.0f, 0.98f, 0.92f), 1.0f, glm::vec3(0.45f, 0.38f, 0.32f), glm::vec3(0.20f, 0.30f, 0.45f), 900.0f },
                Keyframe{18.0f, glm::normalize(glm::vec3(-0.2f, 0.6f, 0.8f)), glm::vec3(1.0f, 0.60f, 0.35f), 0.7f, glm::vec3(0.50f, 0.28f, 0.15f), glm::vec3(0.35f, 0.20f, 0.12f), 1200.0f },
                Keyframe{24.0f, glm::normalize(glm::vec3(-0.2f, 0.1f, 0.2f)), glm::vec3(0.60f, 0.65f, 1.0f), 0.3f, glm::vec3(0.20f, 0.20f, 0.30f), glm::vec3(0.10f, 0.12f, 0.25f), 2000.0f },
            };
        };
        TimeOfDayState timeOfDay;

        // Module 3 (verify.txt): Actor layer isolation.
        // When >= 0, only instances on this layer are visible/selectable in
        // the viewport; all other layers are dimmed / raycast-locked.
        int exclusiveIsolatedLayerIndex = -1;

        // forUI.txt: skybox + analytic physical sky authoring state (exposed in
        // RenderWorldSettingsPanel, persisted via Save/LoadAtmosphereSettings).
        Config::SkyboxUiState      skyboxParams;
        Config::PhysicalSkyUiState physicalSkyParams;
    } uiState;

    EditorUI::ScenePanelConfig scenePanelConfig;
    int scenePanelActiveTab = 0;
    UI::GeoPanelState geoPanelState;
    // Hot-reload status toast (set by the main loop when R is pressed).
    // Displayed briefly in the viewport overlay.
    std::string reloadStatus = "idle";

    struct DebugConfig {
        bool verbose = true;
    } debugConfig;

private:
    bool initialized_ = false;

    ecs::World world_;
    BlueprintSystem blueprintSystem_{world_};  // forUI.txt: prefab authoring facade
    Renderer renderer_;
    ecs::RenderSystem renderSystem_;
    ecs::ModelRenderSystem modelRenderSystem_;
    ecs::LightSystem lightSystem_;
    ecs::GeospatialSystem geospatialSystem_;
    ecs::GeoTerrainSystem geoTerrainSystem_;

    std::unique_ptr<flyCamera> camera_;
    ViewportFramebuffer viewportFB_;
    EntityCache entityCache_;

    ecs::EntityID selectedEntity_ = ecs::INVALID_ENTITY_ID;

    // forPlayer.txt Theme 3: selected instanced world-object index (separate
    // from ECS selectedEntity_ - props live in SimpleWorldRenderer::m_instances,
    // not the ECS world). Recorded by viewport prop picking and surfaced to the
    // Outliner/inspector. The gizmo-on-prop overlay is a documented follow-up.
    std::size_t m_selectedPropInstance = 0;
    bool        m_hasSelectedPropInstance = false;
    GizmoRenderer::GizmoType gizmoType_ = GizmoRenderer::GizmoType::Translate;
    GizmoRenderer::SpaceType spaceType_ = GizmoRenderer::SpaceType::World;
    GizmoRenderer::GizmoAxis hoveredAxis_ = GizmoRenderer::GizmoAxis::None;

    bool showGrid_ = true;
    bool showGizmo_ = true;
    // True while the stand-in animated character is rendered in the viewport.
    // Independent of isPlaying_: toggled by F6 and the Character panel.
    bool showCharacter_ = true;
    int renderMode_ = 0;        // 0=Lit, 1=Wire, 2=Normals, 3=Unlit
    int showWireframe_ = 0;
    bool isViewing_ = false;
    glm::vec2 lastMousePos_{0.0f, 0.0f};

    bool isPlaying_ = false;
    bool wasPlaying_ = false;
    float gameSpeed_ = 1.0f;

    float fps_ = 0.0f;
    int frameCount_ = 0;
    float lastRenderTime_ = 0.0f;

    bool shouldClose_ = false;
    bool wantRestart_ = false;   // set by the toolbar Restart button
    bool wantRewind_ = false;     // set by the toolbar Rewind button
    bool showAbout_ = false;

    // Selection pulse lifetime (seconds of countdown). See triggerSelectionPulse.
    static constexpr float kPulseDuration = 0.5f;
    float pulseTime_ = 0.0f;
};

} // namespace Editor

// Legacy global accessor. New code should prefer dependency injection.
extern Editor::Editor g_editor;

// Legacy free functions preserved for compatibility.
void InitEditor();
void CleanupEditor();

#endif // EDITOR_STATE_H
