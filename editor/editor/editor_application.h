#pragma once
#include <functional>
#include <memory>
#include <string>
#include "config.h"
#include "render_pipeline.h"
#include "input_manager.h"
#include "world_manager.h"
#include "resource_manager.h"
#include "PlayModeController.h"
#include "cameraSystem/ThirdPersonCamera.h"

struct GLFWwindow;
class Model;

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
        
        // Event handlers
        void onWindowResize(int width, int height);
        void onKeyInput(int key, int action);
        
        // Scene helpers
        void createDefaultScene();
        
        // Render the play-mode character mesh (if in play mode)
        void renderPlayCharacter(const glm::mat4& view, const glm::mat4& projection,
                                 const glm::vec3& cameraPos);

        // Render NPC target markers as colored spheres (debug visualization)
        void renderNPCs(const glm::mat4& view, const glm::mat4& projection,
                        const glm::vec3& cameraPos);
        
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
        std::string m_playModelPath = "assets/bot.fbx";
        std::string m_playClipsDir = "assets";
        
        // Third-person follow camera used while in play mode
        ThirdPersonCamera m_playCamera;

        // Debug sphere VAO for rendering NPC targets
        unsigned int m_npcSphereVAO = 0;
        unsigned int m_npcSphereVBO = 0;
        unsigned int m_npcSphereIndexCount = 0;
        bool m_npcSphereInitialized = false;
        // Off by default: the NPC debug spheres are viewport-only diagnostics
        // that read as stray white boulders in-engine and still render even
        // while showCharacter hides the play character (they are world NPCs,
        // not the play model). Toggle only when actively debugging combat.
        bool m_showNPCSpheres = false;
        
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
    };
    
    // Helper function
    inline EditorApplication& getEditorApplication() {
        return EditorApplication::getInstance();
    }
}