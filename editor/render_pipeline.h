#pragma once
#include <glm/glm.hpp>
#include <memory>
#include <functional>
#include <vector>
#include <string>
#include "config.h"
#include "../../shaderSystem/Skybox.h"

class Renderer;
class Shader;
class Model;
struct GLFWwindow;

namespace Render {
    
    // Camera modes
    enum class CameraMode {
        FreeFly,       // Editor-style: right-click + WASD
        ThirdPerson,   // Follow character
        FirstPerson,   // First-person view
        Orbit,         // Fixed orbit around center
        Cinematic      // Slow orbit with smooth transitions
    };
    
    // Render statistics
    struct RenderStats {
        float fps = 0.0f;
        float frameTime = 0.0f;
        float gpuTime = 0.0f;
        size_t drawCalls = 0;
        size_t triangleCount = 0;
        size_t entityCount = 0;
    };
    
    // Render pipeline - manages all rendering operations
    class RenderPipeline {
    public:
        RenderPipeline();
        ~RenderPipeline();
        
        // Initialize rendering system
        bool initialize();
        void shutdown();
        
        // Begin frame rendering
        void beginFrame();
        
        // Render 3D scene to framebuffer
        void renderScene(const glm::mat4& view, const glm::mat4& projection,
                        const glm::vec3& cameraPosition, float fov);
        
        // Render UI overlay
        void renderUI();
        
        // End frame and present
        void endFrame();
        
        // Viewport management
        void resizeViewport(int width, int height);
        int getViewportWidth() const { return m_viewportWidth; }
        int getViewportHeight() const { return m_viewportHeight; }
        
        // Camera control
        void setCameraMode(CameraMode mode) { m_cameraMode = mode; }
        CameraMode getCameraMode() const { return m_cameraMode; }
        
        // Renderer access
        Renderer* getRenderer() { return m_renderer.get(); }
        const Renderer* getRenderer() const { return m_renderer.get(); }
        
        // Shader access
        Shader* getDefaultShader() { return m_defaultShader.get(); }
        Shader* getModelShader() { return m_modelShader.get(); }
        
        // Skybox
        void setSkyboxEnabled(bool enabled) { m_skyboxEnabled = enabled; }
        bool isSkyboxEnabled() const { return m_skyboxEnabled; }
        
        // Wireframe mode
        void setWireframeMode(bool enabled) { m_wireframeMode = enabled; }
        bool isWireframeMode() const { return m_wireframeMode; }
        
        // Get render statistics
        const RenderStats& getStats() const { return m_stats; }
        
        // Framebuffer operations
        unsigned int getFramebufferTexture() const;
        unsigned int getFramebuffer() const { return m_framebuffer; }
        
        // Singleton access
        static RenderPipeline& getInstance();
        
    private:
        // Core rendering
        std::unique_ptr<Renderer> m_renderer;
        std::unique_ptr<Shader> m_defaultShader;
        std::unique_ptr<Shader> m_modelShader;
        std::unique_ptr<Skybox> m_skybox;
        
        // Framebuffer for viewport
        unsigned int m_framebuffer = 0;
        unsigned int m_framebufferTexture = 0;
        unsigned int m_renderbuffer = 0;
        
        // Viewport dimensions
        int m_viewportWidth = 1280;
        int m_viewportHeight = 720;
        
        // Camera
        CameraMode m_cameraMode = CameraMode::FreeFly;
        
        // Options
        bool m_skyboxEnabled = true;
        bool m_wireframeMode = false;
        
        // Statistics
        RenderStats m_stats;
    };
    
    // Helper function
    inline RenderPipeline& getRenderPipeline() {
        return RenderPipeline::getInstance();
    }
}