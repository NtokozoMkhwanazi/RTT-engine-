#pragma once
#include <functional>
#include <future>    // staged stage-reload: worker-side heightmap decode
#include <memory>
#include <string>
#include <vector>
#include "config.h"
#include "render_pipeline.h"
#include "input_manager.h"
#include "world_manager.h"
#include "resource_manager.h"
#include "PlayModeController.h"
#include "cameraSystem/ThirdPersonCamera.h"
#include "ecs/ECS.h"                  // ecs::EntityID / ecs::INVALID_ENTITY_ID for the bot twin
#include "ecs/JobSystem.h"            // stage 2 (forhotswap.txt): async character hot-swap worker

struct GLFWwindow;
class Model;
class SimpleWorldRenderer;            // world-object renderer (see getWorldRenderer)

// ThirdPersonCamera selection-focus tracking (ecs::EntityID) is compiled in
// this translation unit, which already includes ECS headers via EditorApplication
// (ecs/ECS.h). Define the guard so the camera class resolves FocusEntityID /
// INVALID_ENTITY_ID to the real ECS types whenever it is included here.
#define THIRD_PERSON_CAMERA_HAS_ECS

namespace Editor {
    
    // Application state
    enum class AppState {
        Initializing,
        Running,
        Paused,
        ShuttingDown
    };
    
    // Editor application - main application class
    class EditorApplication {
    public:
        EditorApplication();
        ~EditorApplication();
        
        // Initialize the application
        bool initialize(int windowWidth = 1920, int windowHeight = 1080);
        
        // Run the main loop
        int run();

        // Drive the play-mode character and follow camera with explicit input
        // (instead of reading keyboard state). Used by the engine entry point
        // (test.cpp) for the scripted/cinematic demo path and headless runs.
        void updatePlayMode(float dt, const CharacterInput& input);

        // Render the 3D scene only - no buffer swap. The caller is responsible
        // for the final swap, which lets overlays (e.g. ImGui HUD) draw on top.
        void renderSceneOnly(float dt);

        // Queue a staged "Reload Complete Stage" (landscape + props) instead of
        // running it inline in the UI button handler - that blocked the frame
        // for seconds (worker-less Assimp parse + texture upload) and froze the
        // editor. update() pumps the stages; progress is published to
        // uiState.stageReload for the resolve bar. `terrainOnly` is the
        // "Load Ground" variant (skips the props stage).
        void requestStageReload(bool terrainOnly = false);
        // True while a staged reload is queued/running (UI gating).
        bool isStageReloading() const;

        // Optional per-frame UI hook called at the END of render() - after
        // renderSceneOnly() has drawn the scene into the viewport FBO and
        // before the buffer swap. The GL editor path uses it to drive the
        // shared ImGui editor UI (menu bar + panels + viewport) over the FBO
        // texture. When no hook is set, render() falls back to the legacy
        // fullscreen FBO blit + swap.
        void setUiFrameHook(std::function<void(float dt)> hook) { m_uiFrameHook = std::move(hook); }

        // Camera state of the frame just rendered by renderSceneOnly() - the
        // UI hook / viewport overlay uses these to show the active camera and
        // keep the grid/gizmo aligned with the rendered scene.
        const glm::mat4& lastViewMatrix() const { return m_lastView; }
        const glm::mat4& lastProjectionMatrix() const { return m_lastProjection; }
        const glm::vec3& lastCameraPosition() const { return m_lastCameraPos; }
        const glm::vec3& lastCameraTarget() const { return m_lastCameraTarget; }
        
        // Shutdown the application
        void shutdown();
        
        // Get application state
        AppState getState() const { return m_state; }
        
        // Window access
        GLFWwindow* getWindow() { return m_window; }
        
        // Get managers
        Render::RenderPipeline& getRenderPipeline() { return m_renderPipeline; }
        Input::InputManager& getInputManager() { return m_inputManager; }
        World::WorldManager& getWorldManager() { return m_worldManager; }

        // forPlayer.txt Theme 2 (grounded correction): the world-object renderer
        // (trees/rocks/props/water) is ALREADY drawn by WorldManager::render
        // (world_manager.cpp:280) via its WorldObjectManager. This exposes that
        // SAME instance so editor UI can raycast / enumerate props WITHOUT a
        // duplicate SimpleWorldRenderer (which would double-draw). No new member,
        // no extra render() call - renderSceneOnly already renders it at 520.
        SimpleWorldRenderer& getWorldRenderer();
        const SimpleWorldRenderer& getWorldRenderer() const;
        Resources::ResourceManager& getResourceManager() { return m_resourceManager; }
        
        // Scene management
        bool loadScene(const std::string& scenePath);
        bool saveScene(const std::string& scenePath);
        bool newScene();
        
        // Play mode
        void enterPlayMode();
        void exitPlayMode();
        void togglePlayMode();
        bool isInPlayMode() const { return m_isPlaying; }
        
        // Play-mode character controller (null when not in play mode)
        PlayModeController& getPlayController() { return m_playController; }
        const PlayModeController& getPlayController() const { return m_playController; }
        bool isPlayCharacterLoaded() const { return m_playController.isLoaded(); }
        
        // Static character configuration for play mode
        void setPlayCharacterModel(const std::string& path) { m_playModelPath = path; }
        void setPlayCharacterClipsDir(const std::string& dir) { m_playClipsDir = dir; }
        const std::string& getPlayCharacterModel() const { return m_playModelPath; }
        const std::string& getPlayCharacterClipsDir() const { return m_playClipsDir; }
        
        // Get application instance
        static EditorApplication& getInstance();

        // Position fed to WorldManager::update() each frame. The terrain
        // streams chunks and computes LOD around this position, so it must
        // follow the actual follow camera (the camera is updated after
        // character physics, so this uses its previous-frame position) rather
        // than the world origin. Falls back to the origin when the character
        // isn't loaded. Static so the rule is unit-testable without GL.
        static glm::vec3 worldUpdateCameraPos(bool playLoaded,
                                              const ThirdPersonCamera& playCam);
        
    private:
        // Main loop methods
        void processInput(float dt);
        void update(float dt);
        void render(float dt);

        // Advance the staged "Reload Complete Stage" job by one step. Called
        // from update() every frame while uiState.stageReload.active is set.
        void pumpStageReload();

        // Result of the worker-side heightmap decode that feeds the landscape
        // stage (the 4K EXR/PNG decode used to run on the main thread and was
        // the last multi-second freeze in a stage reload).
        struct StageHeightmapDecode {
            bool               ok   = false;
            std::vector<float> data;
            int                size = 0;
            std::string        path;
        };
        std::future<StageHeightmapDecode> m_stageHeightmapFuture;
        bool m_stageHeightmapPending = false;
        // Decoded grid handed to Terrain::initialize() (kept alive only for the
        // duration of the landscape stage).
        std::vector<float> m_stageHeightmap;
        int                m_stageHeightmapSize = 0;

        // forUI.txt Governor: frame-rate-controlled brush density throttle.
        void UpdateAdaptiveBrushThrottle(float dt);
        // forUI.txt Pillar 4: End-key drop-to-floor + surface snapping for selected actors/props.
        void ProcessPlacementConstraints(float dt);
        
        // Event handlers
        void onWindowResize(int width, int height);
        void onKeyInput(int key, int action);
        
        // Scene helpers
        void createDefaultScene();
        
        // Render the play-mode character mesh (if in play mode)
        void renderPlayCharacter(const glm::mat4& view, const glm::mat4& projection,
                                 const glm::vec3& cameraPos);

        // Sync the Character inspector's path/clips buffers into the live
        // play-mode bot and hot-reload it (Animator + viewport mesh) when the
        // artist edits the model path. Editor-time only — never mid play-tick.
        void syncPlayCharacterPath();

        // Bridge the play-mode bot (AnimatedCharacter, which is NOT itself an
        // ECS entity) and a lightweight ECS "twin" (TransformComponent) so the
        // viewport gizmo / Details panel can edit it. Edit mode pushes the
        // twin -> bot; play mode mirrors the bot -> twin. Called every frame
        // from update(). See forUI.txt (Steps A/B/C).
        void syncPlayCharacterTwin();

        // forGeo.txt "Direct Mesh-Attachment Sync Pipeline" - Part 2.
        // Procedurally binds incoming GeoAPI telemetry snapshots to ECS digital
        // twin actors in the viewport: lazily spawns an ECS entity (Transform +
        // Model + Name + Geospatial + Layer), hot-swaps its mesh when the asset
        // kind drifts, drives the transform from the ENU fix (snapped to the
        // terrain heightfield), and garbage-collects twins whose telemetry has
        // timed out. Runs every frame from update() in lockstep with telemetry.
        void SynchronizeDigitalTwinTelemetryActors(float dt);
        
        // Map an animation state to the follow camera's state enum
        CameraState CameraStateForAnimationState(AnimationState s) const;
        
        // Window
        GLFWwindow* m_window = nullptr;
        int m_windowWidth = 1920;
        int m_windowHeight = 1080;
        
        // State
        AppState m_state = AppState::Initializing;
        bool m_isPlaying = false;
        bool m_shouldClose = false;
        
        // Managers (using references to singletons for now)
        Render::RenderPipeline& m_renderPipeline;
        Input::InputManager& m_inputManager;
        World::WorldManager& m_worldManager;
        Resources::ResourceManager& m_resourceManager;
        
        // Play-mode character
        PlayModeController m_playController;
        std::unique_ptr<Model> m_playRenderModel;   // GPU mesh used to draw the character
        // forhotswap.txt stage 2 + the "engine stays empty till an artist loads
        // a model" redesign: DEFAULT-EMPTY. The viewport starts empty (no idle
        // bot) and the bot is created at runtime, exactly like its clips, via
        // the Character panel (syncPlayCharacterPath / launchAsyncSwap). Keep
        // m_lastRequestedMeshPath semantics identical for Todo-7.
        // Default stand-in bot: the viewport boots WITH a loaded character so
        // the artist sees the scene (follow cam, grid, props) immediately. The
        // Character panel hot-reloads a new model+clips via syncPlayCharacterPath()
        // (async swap, stage 2); clearing the field + committing empties the bot.
        std::string m_playModelPath;        // config-driven (engine_config.json "character")
        std::string m_playClipsDir;         // config-driven (engine_config.json "character")
        // Last mesh path actually requested for the play model (Todo 7). Used to
        // avoid reloading on every frame when the kind is unchanged; a failed
        // load is cached so we don't re-attempt a missing asset each frame.
        std::string m_lastRequestedMeshPath;

        // ECS twin entity that carries a TransformComponent the gizmo can read
        // and write. The play-mode bot (AnimatedCharacter) owns its own pose and
        // is not an ECS entity, so without this twin the gizmo has no transform
        // to bind to. Kept in lockstep with the bot via syncPlayCharacterTwin().
        ecs::EntityID m_playCharacterEntity = ecs::INVALID_ENTITY_ID;

        // Third-person follow camera used while in play mode
        ThirdPersonCamera m_playCamera;
        bool m_playCameraRestingSynced = false;   // one-shot resting cam sync on play-enter
        // forCam.txt §3C: frame-coherence of the last selection so FocusOnEntity
        // glides only on an actual selection CHANGE (viewport click / outliner
        // row), not on every frame.
        ecs::EntityID m_lastSelectedEntity = ecs::INVALID_ENTITY_ID;
        std::size_t   m_lastPropIdx        = 0;
        // Last focus target identity used to skip redundant FocusOnEntity calls
        // when the same selection stays held across frames (avoids re-arming the
        // glide timer so the camera can settle instead of oscillating).
        glm::vec3     m_lastFocusPosition  = glm::vec3(0.0f);
        bool          m_lastFocusAnimated  = false;
        // Accumulated scroll-zoom offset from the World Settings Distance
        // slider. The slider sets the resting distance; scroll zoom offsets
        // from it so the two never fight. Applied in renderSceneOnly() as
        // distance = uiState.playCameraDistance + m_cameraZoomOffset.
        float m_cameraZoomOffset = 0.0f;
        
        // Optional per-frame UI hook (see setUiFrameHook).
        std::function<void(float)> m_uiFrameHook;
        // Camera state of the last rendered frame (exposed for the UI hook).
        glm::mat4 m_lastView = glm::mat4(1.0f);
        glm::mat4 m_lastProjection = glm::mat4(1.0f);
        glm::vec3 m_lastCameraPos{0.0f};
        glm::vec3 m_lastCameraTarget{0.0f};

        // Timing
        float m_lastTime = 0.0f;
        float m_fpsTimer = 0.0f;
        int m_frameCount = 0;

        // -------------------------------------------------------------------
        // Stage 2 (forhotswap.txt): asynchronous character hot-swap.
        // A worker thread builds a SEPARATE staging AnimatedCharacter (load +
        // loadLocomotion, which compiles the motion KD-tree OFF the main
        // thread) and extracts its RuntimeSystems; the main thread then
        // atomically adopts them via swapInRuntimeSystems() before render. The
        // live character keeps animating its OLD pack until the worker
        // finishes, so there is never a "half-rebuilt" observable state. If the
        // worker fails or times out, we fall back to the sync reload() path
        // (stage 1), so the live character can NEVER break mid-play.
        // -------------------------------------------------------------------
        struct CharacterSwapAsyncState {
            // Captured by the worker lambda; only read on the main thread AFTER
            // handle.isComplete() fulfills the promise (happens-before).
            struct Capsule {
                AnimatedCharacter::RuntimeSystems systems;  // staging result
                std::atomic<bool> swapReady{false};          // worker built systems OK
                std::atomic<bool> failed{false};             // worker hit a load error
            };
            std::shared_ptr<Capsule> capsule;
            ecs::JobSystem::JobHandle handle;                      // pollable completion
            bool  isLoading = false;
            int   timeoutFrames = 600;                        // ~10s @ 60fps, then sync fallback
            geo::AssetKind prevKind = geo::AssetKind::Unknown;
        };
        CharacterSwapAsyncState m_charSwap;

        // Worker pool for async hot-swap + (future) async content-browser scan.
        ecs::JobSystem m_jobSystem;

    private:
        // Launch a worker to build the staging pack (called from
        // syncPlayCharacterPath when a new character path is requested in play
        // mode). No-op while a swap is already in flight.
        void launchAsyncSwap(const std::string& modelPath,
                             const std::string& clipsDir,
                             geo::AssetKind prevKind);
        // Poll the in-flight swap; apply on completion, fall back to sync on
        // timeout/failure. Called from update() each frame (pre-render).
        void tickCharacterSwap();
        // Adopt a worker-built pack onto the live character + refresh the GPU
        // render mesh + re-clamp to terrain. Single-threaded (main), pre-render.
        void applyPendingSwap();
        // (Re)build m_playRenderModel from path + reseed mesh/gizmo guards.
        void reloadRenderModel(geo::AssetKind prevKind, const std::string& modelPath);
    };
    
    // Helper function
    inline EditorApplication& getEditorApplication() {
        return EditorApplication::getInstance();
    }
}