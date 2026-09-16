#include "render_pipeline.h"
#include "camera_controls.h"
#include "renderer/Renderer.h"
#include "renderer/ShadowMapper.h"
#include "lighting/LightingEnvironment.h"   // canonical sun/fog/ambient standard
#include "lighting/LightData.h"            // GPULightData / LightUBO (suggestions.txt #1)
#include "lighting/CVar.h"                  // live-tunable config (config/cvars.ini)
#include "shaderSystem/Shader.h"
#include <iostream>
#include <fstream>
#include <GLFW/glfw3.h>

namespace Render {

// ----------------------------------------------------------------------------
// Camera mode persistence (camera_mode.cfg)
// ----------------------------------------------------------------------------
bool RenderPipeline::saveCameraModeToDisk() {
    std::ofstream f(kCameraModeConfigFile);
    if (!f) return false;
    f << static_cast<int>(m_cameraMode) << "\n";
    f << (CameraControls::gAutoOrbit ? 1 : 0) << "\n";
    f << (CameraControls::gCursorLock ? 1 : 0) << "\n";
    return f.good();
}

bool RenderPipeline::loadCameraModeFromDisk() {
    std::ifstream f(kCameraModeConfigFile);
    if (!f) return false;

    int mode = -1;
    int autoOrbit = -1, cursorLock = -1;
    int idx = 0;
    std::string line;
    while (std::getline(f, line) && idx < 3) {
        if (idx == 0) mode = std::atoi(line.c_str());
        else if (idx == 1) autoOrbit = std::atoi(line.c_str());
        else if (idx == 2) cursorLock = std::atoi(line.c_str());
        ++idx;
    }

    if (mode >= static_cast<int>(CameraMode::FreeFly) &&
        mode <= static_cast<int>(CameraMode::Cinematic)) {
        m_cameraMode = static_cast<CameraMode>(mode);
    }

    if (autoOrbit == 0 || autoOrbit == 1) CameraControls::gAutoOrbit = (autoOrbit == 1);
    if (cursorLock == 0 || cursorLock == 1) CameraControls::gCursorLock = (cursorLock == 1);

    return true;
}

RenderPipeline& RenderPipeline::getInstance() {
    static RenderPipeline instance;
    return instance;
}

RenderPipeline::RenderPipeline()
    : m_renderer(nullptr)
    , m_defaultShader(nullptr)
    , m_modelShader(nullptr)
    , m_framebuffer(0)
    , m_framebufferTexture(0)
    , m_renderbuffer(0)
    , m_viewportWidth(1280)
    , m_viewportHeight(720)
    , m_cameraMode(CameraMode::FreeFly)
    , m_skyboxEnabled(true)
    , m_wireframeMode(false) {
}

RenderPipeline::~RenderPipeline() {
    shutdown();
}

bool RenderPipeline::initialize() {
    std::cout << "[RenderPipeline] Initializing..." << std::endl;

    // --- Lighting environment: single source of truth for sun / fog / sky ----
    // Load runtime-tunable values (artists edit config/cvars.ini, no rebuild),
    // then bake them into the shared LightingEnvironment that every renderer
    // (terrain, meshes, vegetation) reads. Missing keys fall back to the
    // shipped defaults inside LightingEnvironment.
    CVar::Instance().loadFromFile("config/cvars.ini");
    LightingEnvironment::Instance().applyCvars();
    LightingEnvironment& LEnv = LightingEnvironment::Instance();
    std::cout << "[LightingEnvironment] sun_dir = (" << LEnv.sunDirection.x << ", "
              << LEnv.sunDirection.y << ", " << LEnv.sunDirection.z << ") | fog="
              << (LEnv.fogEnabled ? "on" : "off")
              << " | shadows=" << (LEnv.shadowsEnabled ? "on" : "off") << std::endl;

    // Create the renderer
    m_renderer = std::make_unique<Renderer>();
    m_renderer->Initialize();

    // Create default shader (legacy Lambert fallback)
    try {
        m_defaultShader = std::make_unique<Shader>(
            Config::getShaderPath("VS.glsl").c_str(),
            Config::getShaderPath("FS.glsl").c_str()
        );
        std::cout << "[RenderPipeline] Default shader loaded" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[RenderPipeline] Failed to load default shader: " << e.what() << std::endl;
        return false;
    }

    // Create model shader (same as default for now)
    try {
        m_modelShader = std::make_unique<Shader>(
            Config::getShaderPath("VS.glsl").c_str(),
            Config::getShaderPath("FS.glsl").c_str()
        );
        std::cout << "[RenderPipeline] Model shader loaded" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[RenderPipeline] Failed to load model shader: " << e.what() << std::endl;
        return false;
    }

    // ---- PBR Shader --------------------------------------------------------
    try {
        m_pbrShader = std::make_unique<Shader>(
            Config::getShaderPath("pbrVS.glsl").c_str(),
            Config::getShaderPath("pbrFS.glsl").c_str()
        );
        std::cout << "[RenderPipeline] PBR shader loaded" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[RenderPipeline] PBR shader not found, falling back to default: " << e.what() << std::endl;
        m_pbrShader = nullptr;
    }

    // ---- Shadow Shader (depth-only for CSM) --------------------------------
    try {
        m_shadowShader = std::make_unique<Shader>(
            Config::getShaderPath("shadow_depth.vert").c_str(),
            Config::getShaderPath("shadow_depth.frag").c_str()
        );
        std::cout << "[RenderPipeline] Shadow shader loaded" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[RenderPipeline] Shadow shader not found: " << e.what() << std::endl;
        m_shadowShader = nullptr;
    }

    // ---- SSAO Shaders ------------------------------------------------------
    try {
        m_gBufferShader = std::make_unique<Shader>(
            Config::getShaderPath("ssao_gbuffer.vert").c_str(),
            Config::getShaderPath("ssao_gbuffer.frag").c_str()
        );
        m_ssaoShader = std::make_unique<Shader>(
            Config::getShaderPath("post_quad.vert").c_str(),
            Config::getShaderPath("ssao_calc.frag").c_str()
        );
        m_ssaoBlurShader = std::make_unique<Shader>(
            Config::getShaderPath("post_quad.vert").c_str(),
            Config::getShaderPath("ssao_blur.frag").c_str()
        );
        std::cout << "[RenderPipeline] SSAO shaders loaded" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[RenderPipeline] SSAO shaders not found: " << e.what() << std::endl;
        m_gBufferShader = nullptr;
        m_ssaoShader = nullptr;
        m_ssaoBlurShader = nullptr;
        m_ssaoEnabled = false;
    }

    // ---- Post-Processing Shaders -------------------------------------------
    try {
        m_bloomExtractShader = std::make_unique<Shader>(
            Config::getShaderPath("post_quad.vert").c_str(),
            Config::getShaderPath("post_bloom_extract.frag").c_str()
        );
        m_blurShader = std::make_unique<Shader>(
            Config::getShaderPath("post_quad.vert").c_str(),
            Config::getShaderPath("post_blur.frag").c_str()
        );
        m_compositeShader = std::make_unique<Shader>(
            Config::getShaderPath("post_quad.vert").c_str(),
            Config::getShaderPath("post_composite.frag").c_str()
        );
        std::cout << "[RenderPipeline] Post-processing shaders loaded" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[RenderPipeline] Post-processing shaders not found: " << e.what() << std::endl;
        m_bloomExtractShader = nullptr;
        m_blurShader = nullptr;
        m_compositeShader = nullptr;
    }

    // ---- Auto-exposure luminance shader (log-average luminance) --------------
    try {
        m_luminanceShader = std::make_unique<Shader>(
            Config::getShaderPath("post_quad.vert").c_str(),
            Config::getShaderPath("post_luminance.frag").c_str()
        );
        std::cout << "[RenderPipeline] Luminance shader loaded (auto-exposure)" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[RenderPipeline] Luminance shader not found: " << e.what() << std::endl;
        m_luminanceShader = nullptr;
        m_autoExposure = false;
    }

    // ---- Shadow Mapper (CSM) -----------------------------------------------
    m_shadowMapper = std::make_unique<ShadowMapper>();
    if (!m_shadowMapper->Initialize()) {
        std::cerr << "[RenderPipeline] Shadow mapper init failed, shadows disabled" << std::endl;
        m_shadowEnabled = false;
    }

    // ---- Post-Process Pipeline (Bloom) -------------------------------------
    m_postProcess = std::make_unique<PostProcess>();
    if (!m_postProcess->Initialize(m_viewportWidth, m_viewportHeight)) {
        std::cerr << "[RenderPipeline] Post-process init failed, bloom disabled" << std::endl;
        m_bloomEnabled = false;
    }

    // ---- SSAO Pipeline -----------------------------------------------------
    if (m_ssaoEnabled) {
        m_ssao = std::make_unique<SSAO>();
        if (!m_ssao->Initialize(m_viewportWidth, m_viewportHeight)) {
            std::cerr << "[RenderPipeline] SSAO init failed, SSAO disabled" << std::endl;
            m_ssaoEnabled = false;
        }
    }

    // Create framebuffer for viewport rendering (HDR-capable)
    glGenFramebuffers(1, &m_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);

    // Create HDR framebuffer texture (RGBA16F for tone mapping)
    glGenTextures(1, &m_framebufferTexture);
    glBindTexture(GL_TEXTURE_2D, m_framebufferTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_viewportWidth, m_viewportHeight, 0,
                 GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_framebufferTexture, 0);

    // Create renderbuffer for depth/stencil
    glGenRenderbuffers(1, &m_renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, m_renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, m_viewportWidth, m_viewportHeight);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_renderbuffer);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[RenderPipeline] Framebuffer is not complete!" << std::endl;
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // ---- Forward PBR shared light SSBO (suggestions.txt #1) ----------------
    // Mirrors DeferredRenderer::m_lightSSBO: allocated once here; the per-frame
    // LightUBO (scene lights + directional-sun fallback) is pushed from
    // renderScene's forward path in a single glBufferSubData.
    glGenBuffers(1, &m_forwardLightSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_forwardLightSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(LightUBO), nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_forwardLightSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // Initialize skybox
    {
        std::vector<std::string> dayFaces = {
            "assets/skybox/Cubemap_04/right.jpg",
            "assets/skybox/Cubemap_04/left.jpg",
            "assets/skybox/Cubemap_04/top.jpg",
            "assets/skybox/Cubemap_04/bottom.jpg",
            "assets/skybox/Cubemap_04/front.jpg",
            "assets/skybox/Cubemap_04/back.jpg"
        };
        std::vector<std::string> nightFaces = dayFaces;
        m_skybox = std::make_unique<Skybox>(dayFaces, nightFaces);
        if (m_skybox && m_skybox->getShaderID() != 0) {
            std::cout << "[RenderPipeline] Skybox initialized" << std::endl;
        } else {
            std::cerr << "[RenderPipeline] Skybox initialization failed" << std::endl;
            m_skybox.reset();
        }
    }

    // ---- SSR Shaders --------------------------------------------------------
    std::cout << "[RenderPipeline] Initializing SSR..." << std::endl;
    try {
        m_ssrShader = std::make_unique<Shader>(
            Config::getShaderPath("post_quad.vert").c_str(),
            Config::getShaderPath("ssr_calc.frag").c_str()
        );
        m_ssrCompositeShader = std::make_unique<Shader>(
            Config::getShaderPath("post_quad.vert").c_str(),
            Config::getShaderPath("ssr_composite.frag").c_str()
        );
        m_ssr = std::make_unique<SSR>();
        if (m_ssr->Initialize(m_viewportWidth, m_viewportHeight)) {
            std::cout << "[RenderPipeline] SSR initialized" << std::endl;
        } else {
            std::cerr << "[RenderPipeline] SSR init failed" << std::endl;
            m_ssrEnabled = false;
        }
    } catch (const std::exception& e) {
        std::cerr << "[RenderPipeline] SSR shaders not found: " << e.what() << std::endl;
        m_ssrEnabled = false;
    } catch (...) {
        std::cerr << "[RenderPipeline] SSR unknown error" << std::endl;
        m_ssrEnabled = false;
    }
    std::cout << "[RenderPipeline] SSR block done" << std::endl;

    // ---- Default light (one directional) ------------------------------------
    // #3: resolve the env once here (initialize is single-threaded setup).
    addLight(LightingEnvironment::Instance().sunDirection, glm::vec3(1.0f), 1.0f, 0);

    // ---- Deferred Renderer (opt-in) ----------------------------------------
    try {
        m_deferredRenderer = std::make_unique<DeferredRenderer>();
        if (m_deferredRenderer->Initialize(m_viewportWidth, m_viewportHeight)) {
            m_deferredLightingShader = std::make_unique<Shader>(
                Config::getShaderPath("post_quad.vert").c_str(),
                Config::getShaderPath("deferred_lighting.frag").c_str()
            );
            std::cout << "[RenderPipeline] Deferred renderer ready" << std::endl;
        } else {
            std::cerr << "[RenderPipeline] Deferred renderer init failed" << std::endl;
            m_deferredRenderer.reset();
        }
    } catch (const std::exception& e) {
        std::cerr << "[RenderPipeline] Deferred shader not found: " << e.what() << std::endl;
        m_deferredRenderer.reset();
    }

    std::cout << "[RenderPipeline] Framebuffer created (" << m_viewportWidth << "x" << m_viewportHeight << ")" << std::endl;
    std::cout << "[RenderPipeline] Initialized successfully" << std::endl;
    std::cout << "  PBR:       " << (m_pbrShader ? "YES" : "no (fallback to Lambert)") << std::endl;
    std::cout << "  CSM:       " << (m_shadowEnabled ? "YES (3 cascades)" : "no") << std::endl;
    std::cout << "  SSAO:      " << (m_ssaoEnabled ? "YES" : "no") << std::endl;
    std::cout << "  Deferred:  " << (m_deferredRenderer ? "YES (opt-in)" : "no") << std::endl;
    std::cout << "  Bloom:     " << (m_bloomEnabled ? "YES" : "no") << std::endl;
    std::cout << "  SSR:       " << (m_ssr ? "YES" : "no") << std::endl;
    std::cout << "  Auto-exp:  " << ((m_autoExposure && m_luminanceShader) ? "YES (adaptive)" : "no (manual)") << std::endl;
    std::cout << "  Lights:    " << m_lights.size() << std::endl;

    return true;
}

void RenderPipeline::shutdown() {
    static bool alreadyShutdown = false;
    if (alreadyShutdown) return;
    alreadyShutdown = true;

    std::cout << "[RenderPipeline] Shutting down..." << std::endl;

    if (m_deferredRenderer) { m_deferredRenderer->Shutdown(); m_deferredRenderer.reset(); }
    if (m_ssr) { m_ssr->Shutdown(); m_ssr.reset(); }
    if (m_ssao) { m_ssao->Shutdown(); m_ssao.reset(); }
    if (m_postProcess) { m_postProcess->Shutdown(); m_postProcess.reset(); }
    if (m_shadowMapper) { m_shadowMapper->Shutdown(); m_shadowMapper.reset(); }

    if (m_framebuffer) {
        glDeleteFramebuffers(1, &m_framebuffer);
        m_framebuffer = 0;
    }
    if (m_framebufferTexture) {
        glDeleteTextures(1, &m_framebufferTexture);
        m_framebufferTexture = 0;
    }
    if (m_renderbuffer) {
        glDeleteRenderbuffers(1, &m_renderbuffer);
        m_renderbuffer = 0;
    }
    if (m_forwardLightSSBO) {
        glDeleteBuffers(1, &m_forwardLightSSBO);
        m_forwardLightSSBO = 0;
    }
}

void RenderPipeline::beginFrame(float dt) {
    m_stats.drawCalls = 0;
    m_stats.triangleCount = 0;

    // Sync culling settings from UI state to the Renderer. The engine binary
    // pushes editor.uiState.worldSettings.* into m_maxVisibleInstances /
    // m_cullingDebug before calling renderScene (see test.cpp).
    if (m_renderer) {
        m_renderer->SetMaxVisibleInstances(m_maxVisibleInstances);
        m_renderer->SetCullingDebug(m_cullingDebug);
    }

    // Advance the (optional) temporal lighting fade so an artist's cvar reload
    // transitions smoothly frame-to-frame instead of snapping.
    LightingEnvironment::Instance().update(dt);

    // Reset culling stats for this frame (updated during SubmitBatches)
    m_stats.visibleInstances = 0;
    m_stats.culledInstances = 0;
}

void RenderPipeline::renderScene(const glm::mat4& view, const glm::mat4& projection,
                                  const glm::vec3& cameraPosition, float fov,
                                  const LightingEnvironment& lighting) {
    if (!m_renderer) return;

    // Wireframe is applied by the caller (test.cpp) via glPolygonMode BEFORE
    // renderScene and restored AFTER — so worldManager.render() and
    // RenderPlayCharacter() also render in wireframe when enabled.
    // #3: the env was resolved ONCE by the frame owner and passed in; alias it
    // locally so the body reads `LEnv` (no Instance() lookup anywhere in the
    // render graph below).
    const LightingEnvironment& LEnv = lighting;

    // Sun direction drives BOTH the shadow cascades AND the deferred key light,
    // so the shadows and the lit side always agree with the terrain's sun.
    glm::vec3 lightDir = LEnv.sunDirection;
    float nearClip = 0.1f;
    float farClip = 500.0f;

    // ---- Pass 1: CSM Shadow Map (3 cascades) ------------------------------
    if (m_shadowEnabled && m_shadowMapper && m_shadowMapper->IsInitialized() && m_shadowShader) {
        glm::mat4 viewProj = projection * view;
        m_shadowMapper->Begin(viewProj, lightDir, nearClip, farClip);

        for (int c = 0; c < ShadowMapper::NUM_CASCADES; ++c) {
            m_shadowMapper->BeginCascade(c);

            m_shadowShader->use();
            m_shadowShader->setMat4("uLightSpaceMatrix", m_shadowMapper->GetLightSpaceMatrix(c));

            // Render scene geometry with shadow shader (depth-only)
            m_renderer->SetViewport(0, 0, ShadowMapper::SHADOW_SIZE, ShadowMapper::SHADOW_SIZE);
            m_renderer->SetCameraMatrices(
                glm::mat4(m_shadowMapper->GetLightView()),
                m_shadowMapper->GetLightSpaceMatrix(c));
            m_renderer->SubmitBatches();
            m_renderer->Render();
            m_renderer->ClearBatches();

            // Vegetation / terrain shadow: render additional geometry into
            // the shadow map so trees, rocks, and terrain cast shadows too.
            if (m_shadowRenderCallback) {
                glm::mat4 lightView = m_shadowMapper->GetLightView();
                glm::mat4 lightProj = m_shadowMapper->GetLightSpaceMatrix(c);
                m_shadowRenderCallback(lightView, lightProj);
            }
        }

        m_shadowMapper->End();
        m_stats.drawCalls += ShadowMapper::NUM_CASCADES;
    }

    // ---- DEFERRED PATH (if enabled) ----------------------------------------
    // When deferred is active, the entire scene goes through the G-Buffer:
    //   1. Geometry pass: render all opaque objects into MRT (position/normal/albedo/Material)
    //   2. Lighting pass: fullscreen PBR Cook-Torrance from G-Buffer textures
    //   3. Skybox drawn into the lighting FBO (forward pass on top of deferred)
    // This replaces the forward PBR path below.
    bool usedDeferred = false;
    if (m_deferredEnabled && m_deferredRenderer && m_deferredRenderer->IsInitialized() &&
        m_deferredLightingShader) {
        usedDeferred = true;

        // -- G-Buffer geometry pass --
        m_deferredRenderer->BeginGBuffer();

        // Use the G-Buffer mesh shader for the batched renderer
        Shader* gbufShader = m_gBufferShader ? m_gBufferShader.get() : m_pbrShader.get();
        if (gbufShader) {
            gbufShader->use();
            gbufShader->setMat4("view", view);
            gbufShader->setMat4("projection", projection);
        }

        m_renderer->SetViewport(0, 0, m_viewportWidth, m_viewportHeight);
        m_renderer->SetCameraMatrices(view, projection);
        // World geometry (terrain/vegetation) into the G-Buffer so it
        // receives deferred lighting + post-processing alongside everything
        // else. Runs before EndGBuffer() so the geometry is captured.
        if (m_sceneRenderCallback)
            m_sceneRenderCallback(view, projection, cameraPosition, LEnv);
        m_renderer->SubmitBatches();
        m_renderer->Render();
        m_renderer->ClearBatches();

        m_deferredRenderer->EndGBuffer();

        // -- Compute SSAO from the deferred G-Buffer --
        // The SSAO reads the G-Buffer's position + normal textures directly,
        // so we don't need a separate SSAO G-Buffer pass in deferred mode.
        if (m_ssaoEnabled && m_ssao && m_ssao->IsInitialized() && m_gBufferShader) {
            m_ssao->BeginGBuffer();
            // Copy deferred G-Buffer position + normal to SSAO G-Buffer
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_deferredRenderer->GetGBufferFBO());
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_ssao->GetGPositionFBO());
            glBlitFramebuffer(0, 0, m_viewportWidth, m_viewportHeight,
                              0, 0, m_viewportWidth, m_viewportHeight,
                              GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            m_ssao->EndGBuffer();

            if (m_ssaoShader) {
                m_ssao->RenderSSAO(m_ssaoShader->ID, projection, view,
                                   m_ssaoKernelSize, m_ssaoRadius, m_ssaoBias);
            }
            if (m_ssaoBlurShader) {
                m_ssao->RenderBlur(m_ssaoBlurShader->ID);
            }
        }

        // -- SSR pass (uses deferred G-Buffer) --
        if (m_ssrEnabled && m_ssr && m_ssr->IsInitialized() && m_ssrShader) {
            m_ssr->RenderSSR(
                m_ssrShader->ID, projection, view,
                m_deferredRenderer->GetGPosition(),
                m_deferredRenderer->GetGNormal(),
                m_deferredRenderer->GetGAlbedoMetal(),   // scene-color for SSR = G-Buffer albedo (lightingTex not rendered yet)
                m_deferredRenderer->GetDepthRBO()  // depth is in renderbuffer
            );
            if (m_ssrCompositeShader) {
                m_ssr->RenderBlur(m_blurShader ? m_blurShader->ID : 0);
            }
        }

        // -- Deferred lighting pass with light accumulation --
        GLuint shadowTex = 0;
        glm::mat4 lightSpaceMat(1.0f);
        if (m_shadowEnabled && m_shadowMapper && m_shadowMapper->IsInitialized()) {
            shadowTex = m_shadowMapper->GetCascadeArrayTex();
            lightSpaceMat = m_shadowMapper->GetLightSpaceMatrix(0);
        }
        GLuint ssaoTex = (m_ssaoEnabled && m_ssao && m_ssao->IsInitialized())
                         ? m_ssao->GetSSAOTexture() : 0;
        GLuint ssrTex = (m_ssrEnabled && m_ssr && m_ssr->IsInitialized())
                        ? m_ssr->GetSSRTexture() : 0;

        // Build light data vector for multi-light accumulation
        std::vector<DeferredRenderer::LightData> lightData;
        for (auto& l : m_lights) {
            DeferredRenderer::LightData ld;
            ld.position = l.position;
            ld.color = l.color;
            lightData.push_back(ld);
        }

        // CSM splits + matrices for the deferred lighting shader
        float casSplits[3] = {0, 0, 0};
        glm::mat4 casMats[3] = {glm::mat4(1), glm::mat4(1), glm::mat4(1)};
        if (m_shadowEnabled && m_shadowMapper && m_shadowMapper->IsInitialized()) {
            for (int i = 0; i < ShadowMapper::NUM_CASCADES; ++i) {
                casSplits[i] = m_shadowMapper->GetSplitDistance(i);
                casMats[i] = m_shadowMapper->GetLightSpaceMatrix(i);
            }
        }

        m_deferredRenderer->RenderLightingPass(
            m_deferredLightingShader->ID, projection, view, cameraPosition,
            LEnv.sunDirection, glm::vec3(1.0f), LEnv,   // #3: env threaded once, no Instance() in render graph
            shadowTex, lightSpaceMat, ssaoTex,
            lightData, ssrTex, m_reflectionStrength,
            m_shadowEnabled ? casSplits : nullptr,
            m_shadowEnabled ? casMats : nullptr);

        // -- Forward pass: skybox into the lighting FBO --
        if (m_skyboxEnabled && m_skybox) {
            glBindFramebuffer(GL_FRAMEBUFFER, m_deferredRenderer->GetLightingFBO());
            glViewport(0, 0, m_viewportWidth, m_viewportHeight);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            m_skybox->render(view, projection, LEnv);  // #3: thread env, no Instance() in Skybox::render
            glDepthFunc(GL_LESS);
        }

        // Copy the deferred lighting result into the PostProcess FBO or viewport FBO
        // so bloom + ACES can process it.
        if (m_bloomEnabled && m_postProcess && m_postProcess->IsInitialized()) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_deferredRenderer->GetLightingFBO());
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_postProcess->GetSceneFBO());
            glBlitFramebuffer(0, 0, m_viewportWidth, m_viewportHeight,
                              0, 0, m_viewportWidth, m_viewportHeight,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        } else {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_deferredRenderer->GetLightingFBO());
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_framebuffer);
            glBlitFramebuffer(0, 0, m_viewportWidth, m_viewportHeight,
                              0, 0, m_viewportWidth, m_viewportHeight,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        m_stats.drawCalls += 3;  // gbuffer + lighting + copy
    }

    // ---- SSAO (forward-path only — deferred path integrates SSAO internally)
    if (!usedDeferred && m_ssaoEnabled && m_ssao && m_ssao->IsInitialized() && m_gBufferShader) {
        m_ssao->BeginGBuffer();

        m_gBufferShader->use();
        m_gBufferShader->setMat4("view", view);
        m_gBufferShader->setMat4("projection", projection);

        m_renderer->SetViewport(0, 0, m_viewportWidth, m_viewportHeight);
        m_renderer->SetCameraMatrices(view, projection);
        m_renderer->SubmitBatches();
        m_renderer->Render();
        m_renderer->ClearBatches();

        m_ssao->EndGBuffer();

        if (m_ssaoShader) {
            m_ssao->RenderSSAO(m_ssaoShader->ID, projection, view,
                               m_ssaoKernelSize, m_ssaoRadius, m_ssaoBias);
        }
        if (m_ssaoBlurShader) {
            m_ssao->RenderBlur(m_ssaoBlurShader->ID);
        }

        m_stats.drawCalls += 3;
    }

    // ---- Pass 2: Scene into PostProcess FBO (HDR) — forward path only ------
    if (!usedDeferred) {
    if (m_bloomEnabled && m_postProcess && m_postProcess->IsInitialized()) {
        m_postProcess->BeginScene();
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
        glViewport(0, 0, m_viewportWidth, m_viewportHeight);
    }

    glClearColor(0.08f, 0.09f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Skybox renders first (at z=1.0)
    if (m_skyboxEnabled && m_skybox) {
        m_skybox->render(view, projection, LEnv);  // #3: env threaded, no Instance()
    }

    // Restore standard depth func
    glDepthFunc(GL_LESS);

    // Bind CSM shadow array to texture unit 4
    if (m_shadowEnabled && m_shadowMapper && m_shadowMapper->IsInitialized()) {
        m_shadowMapper->Bind(4);
    }

    // Bind SSAO texture to texture unit 8
    if (m_ssaoEnabled && m_ssao && m_ssao->IsInitialized()) {
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, m_ssao->GetSSAOTexture());
    }

    // Light position (used for both shader uniforms and Renderer)
    glm::vec3 lightPos(100.0f, 100.0f, 50.0f);

    // Select the best available shader (PBR > default)
    Shader* activeShader = m_pbrShader ? m_pbrShader.get() : m_defaultShader.get();
    if (activeShader) {
        activeShader->use();
        activeShader->setMat4("view", view);
        activeShader->setMat4("projection", projection);
        activeShader->setVec3("uCameraPos", cameraPosition);

        // Set light parameters
        // #1: the forward PBR path now consumes the SHARED GPULightData SSBO
        // (binding 0) — the same LightUBO the deferred path uploads — instead
        // of the old (unbound) camera.lightPositions / lightColors uniforms,
        // whose glGetUniformLocation lookups were silent no-ops and never
        // reached pbrFS. Upload the scene lights (with a directional-sun
        // fallback when the list is empty, matching DeferredRenderer) once.
        {
            LightUBO lightCache{};
            if (m_lights.empty()) {
                lightCache.lightCount = 1u;
                lightCache.lights[0] = makeDirectionalLight(
                    LEnv.sunDirection, glm::vec3(1.0f), 1.0f);
            } else {
                uint32_t n = (uint32_t)m_lights.size();
                if (n > kMaxGpuLights) n = kMaxGpuLights;
                lightCache.lightCount = n;
                for (uint32_t i = 0; i < n; ++i) {
                    const Light& l = m_lights[i];
                    lightCache.lights[i] = makeGpuLight(
                        (uint32_t)l.position.w, glm::vec3(l.position),
                        glm::vec3(0.0f), glm::vec3(l.color), l.color.a);
                }
            }
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_forwardLightSSBO);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(LightUBO),
                            &lightCache);
        }

        // NOTE: the previous per-draw glUniform4f("camera.lightPositions[i]")
        // / "camera.lightColors[i]" / "camera.lightCount" calls are removed —
        // they wrote to UBO members the renderer never bound, so they were
        // dead. Light data now flows through the SSBO above (#1).

        // Set CSM parameters
        if (m_shadowEnabled && m_shadowMapper && m_shadowMapper->IsInitialized()) {
            for (int i = 0; i < ShadowMapper::NUM_CASCADES; ++i) {
                activeShader->setMat4("uLightSpaceMatrix[" + std::to_string(i) + "]",
                                      m_shadowMapper->GetLightSpaceMatrix(i));
                activeShader->setFloat("uCascadeSplits[" + std::to_string(i) + "]",
                                       m_shadowMapper->GetSplitDistance(i));
            }
        }

        // Ambient / sky-light: reuse the single LEnv resolved at the top of
        // renderScene (#3). Forward is the legacy fallback path; these keep it
        // consistent with the deferred/terrain sun.
        activeShader->setVec3("uGroundBounce", LEnv.groundBounce);
        activeShader->setVec3("uSkyTint", LEnv.skyLightColor);
        activeShader->setFloat("uAmbientStrength", LEnv.ambientStrength);
        activeShader->setVec3("uSunDirectionWS", LEnv.sunDirection);
        activeShader->setFloat("uSkyLightStrength", LEnv.skyLightStrength);

        // Set SSAO texture uniform
        if (m_ssaoEnabled && m_ssao && m_ssao->IsInitialized()) {
            activeShader->setInt("uSSAOMap", 8);
        }
    }

    // Render scene geometry
    m_renderer->SetViewport(0, 0, m_viewportWidth, m_viewportHeight);
    m_renderer->SetCameraMatrices(view, projection);
    // World geometry (terrain/vegetation) renders into the SAME FBO as the
    // batched forward geometry BEFORE post-processing, so terrain receives
    // bloom / tone-mapping / SSAO like the rest of the scene.
    // Previously this was called from EditorApplication AFTER renderScene()
    // had already finished its post-process pass — world geometry was drawn
    // as a raw overlay on top of the composited image (no bloom/ACES on
    // terrain, depth testing inconsistent with the cleared framebuffer).
    if (m_sceneRenderCallback)
        m_sceneRenderCallback(view, projection, cameraPosition, LEnv);
    m_renderer->SubmitBatches();
    m_renderer->Render();
    m_renderer->ClearBatches();
    } // end forward path

    // ---- Pass 3: Post-Processing (Bloom + Tone Mapping + SSAO) ------------
    if (m_bloomEnabled && m_postProcess && m_postProcess->IsInitialized() &&
        m_bloomExtractShader && m_blurShader && m_compositeShader) {
        m_postProcess->SetExposure(m_exposure);
        m_postProcess->SetAutoExposure(m_autoExposure);
        m_postProcess->SetLuminanceRange(m_lumMin, m_lumMax);
        m_postProcess->SetBloomStrength(m_bloomStrength);
        m_postProcess->EndScene(
            m_bloomExtractShader->ID,
            m_blurShader->ID,
            m_compositeShader->ID,
            m_framebuffer,                                // composite to viewport FBO
            (m_luminanceShader && m_autoExposure) ? m_luminanceShader->ID : 0,
            m_lumMin, m_lumMax
        );
    } else {
        // Fallback: copy scene to viewport FBO
        if (m_bloomEnabled && m_postProcess && m_postProcess->IsInitialized()) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_postProcess->GetSceneFBO());
        } else {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_framebuffer);
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_framebuffer);
        glBlitFramebuffer(0, 0, m_viewportWidth, m_viewportHeight,
                          0, 0, m_viewportWidth, m_viewportHeight,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    }

    // Restore FBO binding + viewport
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glViewport(0, 0, m_viewportWidth, m_viewportHeight);

    m_stats.drawCalls += m_renderer->GetBatchCount();
    m_stats.visibleInstances += m_renderer->getVisibleInstances();
    m_stats.culledInstances += m_renderer->getCulledInstances();

    // Restore FBO binding + viewport
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glViewport(0, 0, m_viewportWidth, m_viewportHeight);
}

void RenderPipeline::renderUI() {
    // UI rendering is handled by the EngineUI system
}

void RenderPipeline::endFrame() {
    // Swap buffers if using default framebuffer
}

void RenderPipeline::resizeViewport(int width, int height) {
    if (width <= 0 || height <= 0 || (width == m_viewportWidth && height == m_viewportHeight)) {
        return;
    }

    m_viewportWidth = width;
    m_viewportHeight = height;

    // Resize viewport FBO
    if (m_framebuffer) {
        glBindTexture(GL_TEXTURE_2D, m_framebufferTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
                     GL_RGBA, GL_FLOAT, nullptr);
        glBindRenderbuffer(GL_RENDERBUFFER, m_renderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    }

    // Resize post-processing FBOs
    if (m_postProcess && m_postProcess->IsInitialized()) {
        m_postProcess->Resize(width, height);
    }

    // Resize SSAO
    if (m_ssao && m_ssao->IsInitialized()) {
        m_ssao->Resize(width, height);
    }

    // Resize deferred renderer
    if (m_deferredRenderer && m_deferredRenderer->IsInitialized()) {
        m_deferredRenderer->Resize(width, height);
    }

    // Resize SSR
    if (m_ssr && m_ssr->IsInitialized()) {
        m_ssr->Resize(width, height);
    }
}

unsigned int RenderPipeline::getFramebufferTexture() const {
    // The final composited result (post-process + terrain/world objects/character
    // rendered on top) lives in m_framebufferTexture. When bloom is enabled,
    // EndScene writes the post-processed output to m_framebuffer (whose color
    // attachment IS m_framebufferTexture). Returning the post-process scene
    // texture (pre-bloom, pre-terrain) caused the viewport to show only the
    // skybox/deferred lighting — terrain, world objects, and character that
    // render after renderScene() were invisible.
    return m_framebufferTexture;
}

} // namespace Render
