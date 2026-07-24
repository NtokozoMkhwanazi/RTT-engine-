#pragma once
#include <memory>
#include <string>
#include "config.h"
#include "render_pipeline.h"
#include "input_manager.h"
#include "world_manager.h"
#include "resource_manager.h"

struct GLFWwindow;

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
        
        // Get application instance
        static EditorApplication& getInstance();
        
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