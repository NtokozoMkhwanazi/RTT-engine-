#pragma once
#include <glm/glm.hpp>
#include <memory>
#include <functional>
#include <vector>
#include <string>
#include "config.h"
#include "../animationSystem/BoneMatrixBuffer.h"
#include "../../shaderSystem/Skybox.h"
#include "../../renderer/ShadowMapper.h"
#include "../../renderer/PostProcess.h"
#include "../../renderer/SSAO.h"
#include "../../renderer/DeferredRenderer.h"
#include "../../renderer/SSR.h"
#include "../../lighting/LightingEnvironment.h"  // #3: renderScene takes the env ref, not Instance()

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
        // Culling / LOD debug stats (pushed from engine UI each frame)
        size_t visibleInstances = 0;
        size_t culledInstances = 0;
    };
    
    // Render pipeline - manages all rendering operations
    class RenderPipeline {
    public:
        RenderPipeline();
        ~RenderPipeline();
        
        // Initialize rendering system
        bool initialize();
        void shutdown();
        
        // Begin frame rendering. `dt` drives per-frame lighting fades
        // (LightingEnvironment::update) — default keeps any caller safe.
        void beginFrame(float dt = 1.0f / 60.0f);
        
        // Render 3D scene to framebuffer.
        // #3: the canonical LightingEnvironment is passed in by the frame owner
        // (EditorApplication::renderSceneOnly -> beginFrame owns the update);
        // renderScene threads it to every renderer below instead of calling
        // Instance() internally.
        void renderScene(const glm::mat4& view, const glm::mat4& projection,
                        const glm::vec3& cameraPosition, float fov,
                        const LightingEnvironment& lighting =
                            LightingEnvironment::Instance());
        
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
        
        // Camera mode persistence
        static constexpr const char* kCameraModeConfigFile = "camera_mode.cfg";
        bool saveCameraModeToDisk();
        bool loadCameraModeFromDisk();
        
        // Renderer access
        Renderer* getRenderer() { return m_renderer.get(); }
        const Renderer* getRenderer() const { return m_renderer.get(); }

        // Shadow mapper access (terrain/vegetation render outside the batched
        // forward pass but can still receive CSM shadows via this accessor).
        ShadowMapper* getShadowMapper() { return m_shadowMapper.get(); }
        const ShadowMapper* getShadowMapper() const { return m_shadowMapper.get(); }

        // Register a callback for scene-level world rendering (terrain,
        // vegetation, etc.) so it executes inside the geometry pass of
        // renderScene() — bound to the correct FBO BEFORE post-processing,
        // not after (which caused world geometry to render as a raw overlay
        // on top of the post-processed image).
        void setSceneRenderCallback(std::function<void(const glm::mat4&,
                                                       const glm::mat4&,
                                                       const glm::vec3&,
                                                       const LightingEnvironment&)> cb) {
            m_sceneRenderCallback = std::move(cb);
        }
        
        // Shader access
        Shader* getDefaultShader() { return m_defaultShader.get(); }
        Shader* getModelShader() { return m_modelShader.get(); }
        Shader* getPbrShader() { return m_pbrShader.get(); }
        Shader* getShadowShader() { return m_shadowShader.get(); }
        Shader* getSSAOShader() { return m_ssaoShader.get(); }
        Shader* getSSAOBlurShader() { return m_ssaoBlurShader.get(); }
        Shader* getGBufferShader() { return m_gBufferShader.get(); }
        
        // Skybox
        void setSkyboxEnabled(bool enabled) { m_skyboxEnabled = enabled; }
        bool isSkyboxEnabled() const { return m_skyboxEnabled; }
        
        // Wireframe mode
        void setWireframeMode(bool enabled) { m_wireframeMode = enabled; }
        bool isWireframeMode() const { return m_wireframeMode; }
        
        // Culling / LOD debug controls (wired from World Settings UI)
        void setCullingDebug(bool on) { m_cullingDebug = on; }
        bool isCullingDebug() const { return m_cullingDebug; }
        void setMaxVisibleInstances(int n) { m_maxVisibleInstances = n; }
        int getMaxVisibleInstances() const { return m_maxVisibleInstances; }
        void setLODBands(float b1, float b2, float b3) {
            m_lodBand1 = b1; m_lodBand2 = b2; m_lodBand3 = b3;
        }
        float getLODBand(int i) const {
            return i == 0 ? m_lodBand1 : (i == 1 ? m_lodBand2 : m_lodBand3);
        }
        void setLODCount(int n) { m_lodCount = n; }
        
        // Post-processing controls
        void setBloomEnabled(bool enabled) { m_bloomEnabled = enabled; }
        bool isBloomEnabled() const { return m_bloomEnabled; }
        void setShadowEnabled(bool enabled) { m_shadowEnabled = enabled; }
        bool isShadowEnabled() const { return m_shadowEnabled; }
        void setSSAOEnabled(bool enabled) { m_ssaoEnabled = enabled; }
        bool isSSAOEnabled() const { return m_ssaoEnabled; }
        void setExposure(float e) { m_exposure = e; }
        void setAutoExposure(bool on) { m_autoExposure = on; }
        bool isAutoExposure() const { return m_autoExposure; }
        void setLuminanceRange(float minL, float maxL) { m_lumMin = minL; m_lumMax = maxL; }
        void setBloomStrength(float s) { m_bloomStrength = s; }
        void setSSAORadius(float r) { m_ssaoRadius = r; }
        void setSSAOBias(float b) { m_ssaoBias = b; }
        void setSSAOKernelSize(int n) { m_ssaoKernelSize = n; }
        void setDeferredEnabled(bool enabled) { m_deferredEnabled = enabled; }
        bool isDeferredEnabled() const { return m_deferredEnabled; }
        DeferredRenderer* getDeferredRenderer() { return m_deferredRenderer.get(); }
        void setSSREnabled(bool enabled) { m_ssrEnabled = enabled; }
        bool isSSREnabled() const { return m_ssrEnabled; }
        void setReflectionStrength(float s) { m_reflectionStrength = s; }

        // Light accumulation (up to 8 lights)
        struct Light {
            glm::vec4 position;  // xyz + type (0=dir, 1=point, 2=spot)
            glm::vec4 color;     // rgb + intensity
        };
        void addLight(const glm::vec3& position, const glm::vec3& color,
                      float intensity, int type = 1) {
            Light l;
            l.position = glm::vec4(position, (float)type);
            l.color = glm::vec4(color, intensity);
            m_lights.push_back(l);
        }
        void clearLights() { m_lights.clear(); }
        const std::vector<Light>& getLights() const { return m_lights; }
        
        // Set a callback to render vegetation/terrain during shadow pass
        void setShadowRenderCallback(std::function<void(const glm::mat4&, const glm::mat4&)> cb) {
            m_shadowRenderCallback = std::move(cb);
        }
        
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
        
        // Shared bone matrix buffer for batched GPU transfers (Phase 2).
        // Replaces N per-Animator Update() calls with one UpdateBatched() per frame.
        std::unique_ptr<BoneMatrixBuffer> m_sharedBoneBuffer;

    public:
        // ── Phase 2: Batched GPU Transfers ──────────────────────────────
        // Collects every active character's final bone matrices and uploads
        // them in a SINGLE glBufferSubData call via BoneMatrixBuffer::UpdateBatched.
        // This replaces the prior per-character Update() path that issued one
        // sub-data per animator — compressing driver overhead by N×.
        void uploadBatchedBoneMatrices(
            const std::vector<const std::vector<glm::mat4>*>& boneMatrices) {
            if (!m_sharedBoneBuffer) {
                m_sharedBoneBuffer = std::make_unique<BoneMatrixBuffer>();
                m_sharedBoneBuffer->Initialize(256, BoneBufferConfig());
            }
            if (m_sharedBoneBuffer) {
                m_sharedBoneBuffer->UpdateBatched(boneMatrices, /*forceUpdate=*/false);
                m_sharedBoneBuffer->Bind(0);
            }
        }
        
        // PBR + Shadow + Post-Processing
        std::unique_ptr<Shader> m_pbrShader;
        std::unique_ptr<Shader> m_shadowShader;
        std::unique_ptr<Shader> m_bloomExtractShader;
        std::unique_ptr<Shader> m_blurShader;
        std::unique_ptr<Shader> m_compositeShader;
        std::unique_ptr<Shader> m_luminanceShader;   // auto-exposure (log-luma)
        std::unique_ptr<ShadowMapper> m_shadowMapper;
        std::unique_ptr<PostProcess> m_postProcess;
        
        // SSAO
        std::unique_ptr<Shader> m_gBufferShader;
        std::unique_ptr<Shader> m_ssaoShader;
        std::unique_ptr<Shader> m_ssaoBlurShader;
        std::unique_ptr<SSAO> m_ssao;
        
        // Deferred renderer (optional path)
        std::unique_ptr<DeferredRenderer> m_deferredRenderer;
        std::unique_ptr<Shader> m_deferredLightingShader;
        bool m_deferredEnabled = true;

        // Screen-Space Reflections
        std::unique_ptr<Shader> m_ssrShader;
        std::unique_ptr<Shader> m_ssrCompositeShader;
        std::unique_ptr<SSR> m_ssr;
        bool m_ssrEnabled = true;
        float m_reflectionStrength = 0.3f;

        std::vector<Light> m_lights;

        // Shared GPULightData SSBO (binding 0) for the forward PBR path (#1).
        // Allocated once in initialize(); the per-frame light set is pushed
        // with a single glBufferSubData (mirrors DeferredRenderer::m_lightSSBO).
        GLuint m_forwardLightSSBO = 0;
        
        // Vegetation shadow callback (renders vegetation/terrain for shadow pass)
        std::function<void(const glm::mat4&, const glm::mat4&)> m_shadowRenderCallback;
        // Scene-level world render callback (terrain/vegetation, etc.) —
        // invoked inside renderScene() during the active FBO binding segment
        // so world geometry is subject to lighting and post-processing.
        std::function<void(const glm::mat4&, const glm::mat4&,
                           const glm::vec3&, const LightingEnvironment&)>
            m_sceneRenderCallback;
        bool m_ssaoEnabled = true;
        float m_ssaoRadius = 0.5f;
        float m_ssaoBias = 0.025f;
        int m_ssaoKernelSize = 64;
        
        bool m_bloomEnabled = true;
        bool m_shadowEnabled = true;
        float m_exposure = 1.0f;
        bool  m_autoExposure = true;      // adapt exposure to log-average luminance
        float m_lumMin = 0.0001f;
        float m_lumMax = 8.0f;
        float m_bloomStrength = 0.15f;
        
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
        
        // Culling / LOD debug state (synced from World Settings UI)
        bool m_cullingDebug = false;
        int m_maxVisibleInstances = 2000;
        float m_lodBand1 = 10.0f;   // distance where LOD 1 kicks in
        float m_lodBand2 = 30.0f;   // distance where LOD 2 kicks in
        float m_lodBand3 = 80.0f;   // distance where LOD 3 kicks in
        int m_lodCount = 3;
        
        // Statistics
        RenderStats m_stats;
    };
    
    // Helper function
    inline RenderPipeline& getRenderPipeline() {
        return RenderPipeline::getInstance();
    }
}
