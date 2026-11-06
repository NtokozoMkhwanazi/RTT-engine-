#include "render_pipeline.h"
#include "camera_controls.h"
#include <glm/gtc/type_ptr.hpp>   // glm::value_ptr (OffscreenScene bridge)
#include "renderer/Renderer.h"
#include "rhi/RHI.h"                       // RHI::isSoftwareRasterizer() (software-GL auto-mitigation)
#include "renderer/GPUProfilerAdvanced.h"   // BEGIN_GPU_FRAME/END_GPU_FRAME/PROFILE_GPU_SCOPE
#include "renderer/ShadowMapper.h"
#include "lighting/LightingEnvironment.h"   // canonical sun/fog/ambient standard
#include "lighting/LightData.h"            // GPULightData / LightUBO (suggestions.txt #1)
#include "lighting/CVar.h"                  // live-tunable config (config/cvars.ini)
#include "profiler.h"                     // drive the (thread-safe) CPU profiler each frame
#include "shaderSystem/Shader.h"
#include <iostream>
#include <fstream>
#include <GLFW/glfw3.h>

#include "editor/editor_state.h"            // g_editor (Editor::world) for ECS primitive draw
#include "ecs/ECS.h"                        // ecs::World::forEach
#include "ecs/components/TransformComponent.h"
#include "ecs/components/MeshComponent.h"
#include "renderer/MeshRegistry.h"          // procedural primitive VAOs (Cube/Sphere/...)
#include "mesh_builder.h"                   // CPU procedural-vertex generators (bridge source-of-truth)
#include "ecs/components/ModelComponent.h"           // static arena models
#include "ecs/components/AnimatedCharacterComponent.h" // skinned NPCs / play-char borrow
#include "../modelSystem/Model.h"           // Model::GetMesh/Vertex (skinned bone data)
#include "world/Terrain.h"          // bridge: Terrain -> OffscreenScene (Phase B terrain port)
#include "texture_budget.h"         // Point 3: kMaxDimension ceiling for albedo assets
#include "terrain_bridge.h"        // pure-CPU bakeTerrainChunk (height-baked grid); tested headless
#include "shaderSystem/stb_image.h" // cached layer-albedo pixel load (GL-context-free -> RHI parity)
#include "rhi/RHI.h"                // OffscreenMesh/Scene types
#include <unordered_map>
#include <map>
#include <algorithm>
#include <cstring>                          // memcpy (mat4 -> float[16])

namespace Render {

namespace {
// forRenderer.txt Part 1 — route spawned ECS MeshComponent primitives into the
// deferred renderer. The .txt blueprints assumed invented APIs; the mappings
// below are verified against the real headers:
//   TransformComponent::getModelMatrix()              (not transform.matrix())
//   MeshComponent.meshType / .visible / .castShadow   (not mesh.type)
//   MeshRegistry::getMesh(MeshType) -> MeshEntry      (not MeshBuilder directly;
//     the registry is what RenderSystem already uses and is populated from
//     MeshBuilder in editor_state.cpp InitEditor)
//   ssao_gbuffer.vert:  uniform mat4 model            (not uModel)
//   shadow_depth.vert:  layout(location=6) in mat4 aModel  (not a uModel uniform)
//
// Note: the editor always renders spawned primitives again in a forward overlay
// (renderSceneOnly -> fwd.Render with the PBR shader), so their albedo /
// metallic / roughness stay correct via that path. This injection gives them
// deferred G-Buffer SSAO/shadow-depth participation and CSM shadow casting.
void renderEcsPrimitivesToGBuffer(Shader* gbufShader) {
    if (!gbufShader || !g_editor.isInitialized()) return;
    gbufShader->use();
    g_editor.world().forEach<ecs::TransformComponent, ecs::MeshComponent>(
        [gbufShader](auto, ecs::TransformComponent& t, ecs::MeshComponent& m) {
            if (!m.visible) return;
            gbufShader->setMat4("model", t.getModelMatrix());
            gbufShader->setVec3("uAlbedoColor", m.color);
            gbufShader->setFloat("uMetallic",   m.metallic);
            gbufShader->setFloat("uRoughness",  m.roughness);
            if (const MeshEntry* e = MeshRegistry::getInstance().getMesh(m.meshType)) {
                glBindVertexArray(e->VAO);
                glDrawElements(GL_TRIANGLES, e->indexCount, GL_UNSIGNED_INT, nullptr);
            }
        });
    glBindVertexArray(0);
}

void renderEcsPrimitivesToShadowMap(Shader* shadowShader) {
    if (!shadowShader || !g_editor.isInitialized()) return;
    shadowShader->use();
    // The shadow pass is depth-only; make sure depth test/write are on so the
    // injected primitives correctly occlude one another in the cascade.
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    g_editor.world().forEach<ecs::TransformComponent, ecs::MeshComponent>(
        [shadowShader](auto, ecs::TransformComponent& t, ecs::MeshComponent& m) {
            if (!m.visible || !m.castShadow) return;
            (void)shadowShader;
            if (const MeshEntry* e = MeshRegistry::getInstance().getMesh(m.meshType)) {
                glm::mat4 model = t.getModelMatrix();
                glBindVertexArray(e->VAO);
                // shadow_depth.vert: layout(location=6) in mat4 aModel — a mat4
                // input spans attribute locations 6/7/8/9. Feed it as a per-vertex
                // constant (divisor 0) per rigid primitive cube.
                for (int i = 0; i < 4; ++i) {
                    glVertexAttrib4fv(6 + i, &model[i][0]);
                    glEnableVertexAttribArray(6 + i);
                }
                glDrawElements(GL_TRIANGLES, e->indexCount, GL_UNSIGNED_INT, nullptr);
                for (int i = 0; i < 4; ++i) glDisableVertexAttribArray(6 + i);
            }
        });
    glBindVertexArray(0);
}
} // namespace

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

    // Software-rasterizer (llvmpipe / swrast) auto-mitigation. The full GL render
    // graph (3x CSM + SSAO + bloom + deferred G-buffer) renders at a few FPS
    // under llvmpipe; detect and drop those passes here so the editor stays
    // responsive on a no-GPU / software-GL host. No-ops on real hardware.
    if (RHI::isSoftwareRasterizer()) {
        setShadowEnabled(false);
        setSSAOEnabled(false);
        setBloomEnabled(false);
        std::cout << "[RenderPipeline] Software rasterizer - "
                  << "CSM/SSAO/Bloom/Deferred disabled for responsiveness." << std::endl;
    }

    // Drive the advanced GPU profiler (GL timestamp queries). No-op-safe:
    // initialize() returns false and issues no GL when GL_TIMESTAMP queries are
    // unsupported (e.g. headless test GL contexts).
    AdvancedGPUProfiler::getInstance().initialize();

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

    // 1x1 white texture (bound to texture unit 8 / uSSAOMap in the forward PBR
    // pass) so the ambient AO factor samples 1.0. The forward path has no SSAO
    // G-Buffer (ECS renders through the editor's own forward renderer, not
    // RenderPipeline::m_renderer, which stays empty in forward mode), so
    // sampling the real (black) AO buffer would clamp ambient to 0 and blacken
    // the scene. Mirrors test_pbr_forward_gl.cpp's unit-8 white bind. Freed in
    // shutdown().
    glGenTextures(1, &m_whiteTexture);
    glBindTexture(GL_TEXTURE_2D, m_whiteTexture);
    unsigned char whitePixel[4] = { 255, 255, 255, 255 };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 whitePixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

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

    // ---- Instanced ECS-primitive G-Buffer shader (forRenderer.txt Part 2) ---
    try {
        m_primitiveInstanceShader = std::make_unique<Shader>(
            Config::getShaderPath("primitive_gbuffer.vert").c_str(),
            Config::getShaderPath("primitive_gbuffer.frag").c_str()
        );
        if (m_primitiveInstanceShader && m_primitiveInstanceShader->ID) {
            // Streaming instance buffer, refilled once per frame from the world.
            glGenBuffers(1, &m_primitiveInstanceVBO);
            glBindBuffer(GL_ARRAY_BUFFER, m_primitiveInstanceVBO);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(m_maxPrimitiveInstances) *
                             sizeof(PrimitiveInstanceData),
                         nullptr, GL_STREAM_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            std::cout << "[RenderPipeline] Instanced primitive shader ready" << std::endl;
        } else {
            m_primitiveInstanceShader.reset();
            m_primitiveInstanceVBO = 0;
        }
    } catch (const std::exception& e) {
        std::cerr << "[RenderPipeline] Instanced primitive shader not found: "
                  << e.what() << std::endl;
        m_primitiveInstanceShader.reset();
        m_primitiveInstanceVBO = 0;
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
    if (m_shadowEnabled) {
        m_shadowMapper = std::make_unique<ShadowMapper>();
        if (!m_shadowMapper->Initialize()) {
            std::cerr << "[RenderPipeline] Shadow mapper init failed, shadows disabled" << std::endl;
            m_shadowEnabled = false;
        }
    }

    // ---- Post-Process Pipeline (Bloom) -------------------------------------
    if (m_bloomEnabled) {
        m_postProcess = std::make_unique<PostProcess>();
        if (!m_postProcess->Initialize(m_viewportWidth, m_viewportHeight)) {
            std::cerr << "[RenderPipeline] Post-process init failed, bloom disabled" << std::endl;
            m_bloomEnabled = false;
        }
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
    // Render targets must NEVER use GL_REPEAT (the default): during a panel
    // shrink/expand the ImGui Image UV-crop briefly exceeds 1.0 and would tile
    // the rendered region (garbage/stale texels) instead of edge-clamping —
    // this was the root cause of the viewport "going blank while resizing a
    // panel". CLAMP_TO_EDGE makes any overshoot sample the stable edge pixel.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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
            "assets/skybox/Cubemap/px.png",
            "assets/skybox/Cubemap/nx.png",
            "assets/skybox/Cubemap/py.png",
            "assets/skybox/Cubemap/ny.png",
            "assets/skybox/Cubemap/pz.png",
            "assets/skybox/Cubemap/nz.png"
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

    // forUI.txt Part 3: analytic procedural-sky shader (fullscreen gl_VertexID
    // quad). Loaded eagerly; the per-frame dispatch is a no-op unless the
    // authoring switch (editor.uiState.physicalSkyParams.useProceduralOverCubemap)
    // is engaged, so headless/gated rendering is unaffected.
    try {
        m_proceduralSkyShader = std::make_unique<Shader>(
            Config::getShaderPath("procedural_sky.vert").c_str(),
            Config::getShaderPath("procedural_sky.frag").c_str());
        if (m_proceduralSkyShader && m_proceduralSkyShader->ID != 0) {
            glGenVertexArrays(1, &m_procSkyQuadVAO);
            glBindVertexArray(m_procSkyQuadVAO);
            glBindVertexArray(0);
            std::cout << "[RenderPipeline] Procedural sky shader initialized" << std::endl;
        } else {
            m_proceduralSkyShader.reset();
        }
    } catch (const std::exception& e) {
        std::cerr << "[RenderPipeline] Procedural sky shader load failed: " << e.what() << "\n";
        m_proceduralSkyShader.reset();
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
    // On a software rasterizer the deferred G-buffer (4 fullscreen targets +
    // lighting pass) is the heaviest pass; skip it so the editor is usable.
    if (!RHI::isSoftwareRasterizer()) {
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

    // verify.txt Fix: only drop GL-backed resources while a context is current.
    // If shutdown() runs after the GLFW context is gone (static teardown,
    // std::exit / abort) these glDelete* calls segfault at exit.
    if (glfwGetCurrentContext() != nullptr) {
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
        if (m_whiteTexture) {
            glDeleteTextures(1, &m_whiteTexture);
            m_whiteTexture = 0;
        }
        if (m_primitiveInstanceVBO) {
            glDeleteBuffers(1, &m_primitiveInstanceVBO);
            m_primitiveInstanceVBO = 0;
        }
    }

    // Reset GL-heavy resources while the GL context is still alive (before
    // static destructors run). Without this, the Skybox/Shader/Renderer
    // destructors fire during static teardown when the GL context is gone,
    // causing a SIGSEGV (see gl_context_lifecycle.h).
    if (m_skybox) { m_skybox->cleanup(); m_skybox.reset(); }
    m_defaultShader.reset();
    m_modelShader.reset();
    m_pbrShader.reset();
    m_shadowShader.reset();
    m_bloomExtractShader.reset();
    m_blurShader.reset();
    m_compositeShader.reset();
    m_luminanceShader.reset();
    m_ssaoShader.reset();
    m_ssaoBlurShader.reset();
    m_gBufferShader.reset();
    m_primitiveInstanceShader.reset();
    m_deferredLightingShader.reset();
    m_ssrShader.reset();
    m_ssrCompositeShader.reset();
    if (m_renderer) { m_renderer->Shutdown(); m_renderer.reset(); }
}

// forUI.txt Pillar 1: routes a runtime cubemap hot-swap through to the Skybox,
// which deletes the old GL texture and re-uploads the 6 provided face paths.
void RenderPipeline::UpdateSkyboxTexture(const std::vector<std::string>& facePaths) {
    if (!m_skybox) return;
    m_skybox->UpdateSkyboxTexture(facePaths);
}

void RenderPipeline::beginFrame(float dt) {
    // Drive the thread-safe CPU profiler (start-of-frame timer + periodic
    // flush). Worker-thread PROFILE_CPU_SCOPE scopes (e.g. SystemExecutor)
    // populate cpuRegions during the update graph this frame.
    Profiler::Instance().beginFrame();
    BEGIN_GPU_FRAME();  // open AdvancedGPUProfiler frame scope (no-op if not initialized)

    m_stats.drawCalls = 0;
    m_stats.triangleCount = 0;

    // Advance the (optional) temporal lighting fade so an artist's cvar reload
    // transitions smoothly frame-to-frame instead of snapping.
    LightingEnvironment::Instance().update(dt);
}

bool RenderPipeline::renderProceduralSky(const glm::mat4& view, const glm::vec3& sunDir) {
    // forUI.txt Part 3: frame-uniform dispatch — analytic procedural sky
    // overrides the cubemap when the authoring switch is engaged. STRICTLY
    // gated: zero GL impact unless the editor state is live AND the proc-sky
    // shader compiled AND the routing flag is set (defaults false, so the
    // headless gate here is a no-op).
    if (!m_proceduralSkyShader || !g_editor.isInitialized()) return false;
    const Config::PhysicalSkyUiState& ps = g_editor.uiState.physicalSkyParams;
    if (!ps.useProceduralOverCubemap) return false;

    glViewport(0, 0, m_viewportWidth, m_viewportHeight);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(m_procSkyQuadVAO);
    m_proceduralSkyShader->use();
    m_proceduralSkyShader->setVec3("uSunDir", sunDir);
    // camera forward = -3rd column of the (column-major) view matrix
    glm::vec3 camForward = -glm::vec3(view[0][2], view[1][2], view[2][2]);
    m_proceduralSkyShader->setVec3("uViewDir", camForward);
    m_proceduralSkyShader->setFloat("uTurbidity", ps.turbidity);
    m_proceduralSkyShader->setFloat("uSunIntensity", ps.sunIntensity);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);   // procedural_sky.vert: gl_VertexID quad
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    return true;
}

// ============================================================================
// Phase B (Step A): build the backend-agnostic RHI::OffscreenScene from the
// live editor ECS + LightingEnvironment. This is the bridge that lets test.cpp
// render through rhi->renderFrameScene(scene) instead of renderPipeline.
// renderScene(). The scene carries exactly the subset with GL<->Vulkan pixel
// parity (forward + skinned + deferred): CSM/SSAO/SSR/skybox/bloom flags are
// threaded through but NO-OP'd by the backends until Phase B.
// ============================================================================
namespace {

// Procedural-primitive CPU verts (source-of-truth via MeshBuilder::Create*Verts).
// Cube is NOT returned here (Cube meshes become OffscreenScene::instances — the
// RHI's built-in cube — which the 60/60 parity tests exercise directly).
RHI::OffscreenMesh vertsForPrimitive(ecs::MeshType type, const glm::vec4& color) {
    RHI::OffscreenMesh mesh;
    MeshBuilder::Verts v;
    switch (type) {
        case ecs::MeshType::Sphere:    v = MeshBuilder::CreateSphereVerts(24, 32, 0.5f); break;
        case ecs::MeshType::Plane:     v = MeshBuilder::CreatePlaneVerts(); break;
        case ecs::MeshType::Cylinder:  v = MeshBuilder::CreateCylinderVerts(24, 0.5f, 1.0f); break;
        case ecs::MeshType::Cone:      v = MeshBuilder::CreateConeVerts(24, 0.5f, 1.0f); break;
        case ecs::MeshType::Torus:     v = MeshBuilder::CreateTorusVerts(32, 24, 0.35f, 0.15f); break;
        case ecs::MeshType::Cube:
            // Defensive: a Cube MeshComponent should take the instances path
            // (the caller routes Cube->instances). If it arrives here, fall
            // back to the cube verts so nothing silently disappears.
            v = MeshBuilder::CreateCubeVerts(); break;
        default: v = MeshBuilder::CreateCubeVerts(); break;
    }
    mesh.vertices = std::move(v.vertices);
    mesh.indices = std::move(v.indices);
    // 1x1 albedo of the MeshComponent color: the PBR mesh path samples the
    // albedo texture and multiplies by the per-instance color, so a 1x1 pixel
    // of m.color makes the offscreen mesh tint identically to the GL G-Buffer
    // path (renderEcsPrimitivesToGBuffer feeds uAlbedoColor = m.color).
    mesh.texturePixels = {
        (uint8_t)(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f),
        (uint8_t)(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f),
        (uint8_t)(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f),
        255
    };
    mesh.textureWidth = 1;
    mesh.textureHeight = 1;
    return mesh;
}

// Fill a per-instance transform block identically for cubes + meshes.
inline void fillInstance(RHI::OffscreenInstance& inst, const glm::mat4& m,
                         const glm::vec4& color) {
    std::memcpy(inst.model, glm::value_ptr(m), 16 * sizeof(float));
    inst.color[0] = color.r; inst.color[1] = color.g; inst.color[2] = color.b; inst.color[3] = color.a;
    std::memcpy(inst.prevModel, glm::value_ptr(m), 16 * sizeof(float)); // prev==curr => velocity 0 (forward)
}

} // namespace

RHI::OffscreenScene RenderPipeline::toOffscreenScene(const glm::mat4& view,
    const glm::mat4& projection, const glm::vec3& cameraPosition, float fov,
    const LightingEnvironment& lighting) const {
    (void)projection; // RHI reconstructs proj from camera.fovDeg + far + aspect = parity (see note)

    RHI::OffscreenScene scene;

    // ---- Camera: rebuild eye/target/up/fov so the RHI's viewProj matches the
    // GL camera the frame owner built (offscreen GL<->Vulkan parity is
    // verified with OffscreenCamera eye/target/up/fov, so mirroring them
    // here gives identical projection). forward/up are the view matrix's
    // 3rd/2nd columns (GLM lookAt convention: col2 = -forward, col1 = up).
    const glm::vec3 forward = -glm::vec3(view[2][0], view[2][1], view[2][2]);
    const glm::vec3 up      =  glm::vec3(view[1][0], view[1][1], view[1][2]);
    scene.camera.eye[0] = cameraPosition.x; scene.camera.eye[1] = cameraPosition.y; scene.camera.eye[2] = cameraPosition.z;
    const glm::vec3 target = cameraPosition + forward;
    scene.camera.target[0] = target.x; scene.camera.target[1] = target.y; scene.camera.target[2] = target.z;
    scene.camera.up[0] = up.x; scene.camera.up[1] = up.y; scene.camera.up[2] = up.z;
    scene.camera.fovDeg = fov;
    scene.camera.farPlane = Config::getRenderConfig().farPlane;

    // ---- LightingEnvironment -> scene lighting flags ----
    scene.fogDensity = lighting.fogEnabled ? 0.04f : 0.0f; // editor fog ~0.04 (matches GL fog math)
    scene.fogColor[0] = lighting.fogHorizon.x; scene.fogColor[1] = lighting.fogHorizon.y; scene.fogColor[2] = lighting.fogHorizon.z;
    scene.pbrEnabled = true;
    scene.skyEnabled  = m_skyboxEnabled;
    scene.sunDirection[0] = lighting.sunDirection.x;
    scene.sunDirection[1] = lighting.sunDirection.y;
    scene.sunDirection[2] = lighting.sunDirection.z;
    scene.shadows = m_shadowEnabled && m_shadowMapper && m_shadowMapper->IsInitialized();
    scene.deferred = false; // editor forward by default; RTT_DEFERRED_SCENE / A1 opt-in to deferred

    // CSM cascade config (best-effort: the shadow atlas is NO-OP until B1, but
    // threading the matrices means B1 drops in without touching this bridge).
    scene.shadowCascades = ShadowMapper::NUM_CASCADES;
    // log-linear split fractions (editor heuristic, lambda=0.5) — matches
    // ShadowMapper's Begin() split scheme so B1 reads the same cascade set.
    scene.cascadeSplits[0] = 0.05f; scene.cascadeSplits[1] = 0.2f; scene.cascadeSplits[2] = 0.6f;
    if (m_shadowMapper && m_shadowMapper->IsInitialized()) {
        for (int c = 0; c < ShadowMapper::NUM_CASCADES && c < 3; ++c) {
            const glm::mat4 ls = m_shadowMapper->GetLightSpaceMatrix(c);
            std::memcpy(scene.lightSpaceMatrices[c], glm::value_ptr(ls), 16 * sizeof(float));
        }
    }

    // ---- ECS primitives + models + skinned characters ----
    if (g_editor.isInitialized()) {
        // MeshComponent primitives (Cube -> instances; Sphere/Plane/Cyl/Cone/Torus -> meshes).
        g_editor.world().forEach<ecs::TransformComponent, ecs::MeshComponent>(
            [&](auto, ecs::TransformComponent& t, ecs::MeshComponent& m) {
                if (!m.visible) return;
                const glm::mat4 model = t.getModelMatrix();
                const glm::vec4 color(m.color.r, m.color.g, m.color.b, 1.0f);
                if (m.meshType == ecs::MeshType::Cube) {
                    RHI::OffscreenInstance inst{};
                    fillInstance(inst, model, color);
                    scene.instances.push_back(inst);
                } else {
                    RHI::OffscreenMesh om = vertsForPrimitive(m.meshType, color);
                    om.instances.emplace_back();
                    fillInstance(om.instances.back(), model, color);
                    om.version = 1;
                    scene.meshes.push_back(std::move(om));
                }
            });

        // Static ModelComponent (arena) meshes.
        g_editor.world().forEach<ecs::TransformComponent, ecs::ModelComponent>(
            [&](auto, ecs::TransformComponent& t, ecs::ModelComponent& mc) {
                if (!mc.visible) return;
                Model* model = ModelSystem::ModelRegistry::getInstance().get(mc.modelHandle);
                if (!model) return;
                const glm::mat4 modelMat = t.getModelMatrix();
                const glm::vec4 color(mc.albedoOverride.r, mc.albedoOverride.g, mc.albedoOverride.b, 1.0f);
                for (size_t i = 0; i < model->GetMeshCount(); ++i) {
                    const Mesh& m = model->GetMesh(i);
                    if (m.vertices.empty()) continue;
                    RHI::OffscreenMesh om;
                    om.vertices.resize(m.vertices.size() * 8);
                    for (size_t v = 0; v < m.vertices.size(); ++v) {
                        const Vertex& V = m.vertices[v];
                        const size_t o = v * 8;
                        om.vertices[o+0]=V.Position.x; om.vertices[o+1]=V.Position.y; om.vertices[o+2]=V.Position.z;
                        om.vertices[o+3]=V.Normal.x;   om.vertices[o+4]=V.Normal.y;   om.vertices[o+5]=V.Normal.z;
                        om.vertices[o+6]=V.TexCoords.x; om.vertices[o+7]=V.TexCoords.y;
                    }
                    om.indices.assign(m.indices.begin(), m.indices.end());
                    om.instances.emplace_back();
                    fillInstance(om.instances.back(), modelMat, color);
                    om.version = 1;
                    om.windSway = mc.windSway;
                    scene.meshes.push_back(std::move(om));
                }
            });

        // AnimatedCharacterComponent (skinned). Includes the externally-ticked
        // play char: under RTT_RHI_SCENE test.cpp skips renderAnimated(), so the
        // character MUST live in the OffscreenScene to remain visible. Uses
        // ch->modelMatrix() (matches renderAnimated) + the live bone palette.
        g_editor.world().forEach<ecs::TransformComponent, ecs::AnimatedCharacterComponent>(
            [&](auto, ecs::TransformComponent& t, ecs::AnimatedCharacterComponent& acc) {
                (void)t; // AnimatedCharacter drives its own modelMatrix()
                if (!acc.active || !acc.isValid()) return;
                Model* model = acc.getModel();
                AnimatedCharacter* ch = acc.get();
                if (!model || !ch) return;
                const std::vector<glm::mat4>& palette = ch->debugFinalBones();
                if (palette.empty()) return;
                const glm::mat4 modelMat = ch->modelMatrix();
                const glm::vec4 color(1.0f, 1.0f, 1.0f, 1.0f);
                for (size_t i = 0; i < model->GetMeshCount(); ++i) {
                    const Mesh& m = model->GetMesh(i);
                    if (m.vertices.empty()) continue;
                    RHI::OffscreenMesh om;
                    om.skinned = true;
                    om.jointCount = (int)palette.size();
                    om.vertices.resize(m.vertices.size() * 8);
                    om.boneIndices.resize(m.vertices.size() * 4);
                    om.boneWeights.resize(m.vertices.size() * 4);
                    for (size_t v = 0; v < m.vertices.size(); ++v) {
                        const Vertex& V = m.vertices[v];
                        const size_t o = v * 8, b = v * 4;
                        om.vertices[o+0]=V.Position.x; om.vertices[o+1]=V.Position.y; om.vertices[o+2]=V.Position.z;
                        om.vertices[o+3]=V.Normal.x;   om.vertices[o+4]=V.Normal.y;   om.vertices[o+5]=V.Normal.z;
                        om.vertices[o+6]=V.TexCoords.x; om.vertices[o+7]=V.TexCoords.y;
                        om.boneIndices[b+0]=(uint32_t)V.BoneIDs.x; om.boneIndices[b+1]=(uint32_t)V.BoneIDs.y;
                        om.boneIndices[b+2]=(uint32_t)V.BoneIDs.z; om.boneIndices[b+3]=(uint32_t)V.BoneIDs.w;
                        om.boneWeights[b+0]=V.Weights.x; om.boneWeights[b+1]=V.Weights.y;
                        om.boneWeights[b+2]=V.Weights.z; om.boneWeights[b+3]=V.Weights.w;
                    }
                    om.indices.assign(m.indices.begin(), m.indices.end());
                    if (om.jointCount && (size_t)(om.jointCount * 16) <= palette.size() * 16) {
                        om.jointPalette.resize((size_t)om.jointCount * 16);
                        for (int b = 0; b < om.jointCount; ++b)
                            std::memcpy(&om.jointPalette[(size_t)b * 16],
                                        glm::value_ptr(palette[(size_t)b]), 16 * sizeof(float));
                    }
                    om.instances.emplace_back();
                    fillInstance(om.instances.back(), modelMat, color);
                    om.version = 1;
                    scene.meshes.push_back(std::move(om));
                }
            });
    }

    // ---- Phase B terrain port: convert world/Terrain chunks into OffscreenMesh
    // splat meshes (height-baked grid + layer-0 albedo tiled). Vulkan renders
    // generic meshes via pbr_mesh; GL offscreen via pbrFsSrc -> bit-identical.
    // NO-OP when no Terrain is wired (headless tests) -> 60/60 safe.
    addTerrainToScene(scene, cameraPosition);
    addVegetationToScene(scene, cameraPosition);

    return scene;
}

// terrain_splat.txt Phase B: world/Terrain -> OffscreenScene bridge.
// Bakes each loaded chunk's (flat, y=0) TerrainVertex grid into a
// height-baked pos/normal/uv mesh + tiles the layer-0 albedo (stbi-loaded,
// GL-context-free) into texturePixels. The mesh is GENERIC (splatLayers
// empty): the existing generic offscreen PBR on BOTH backends — GL pbrFsSrc
// (renderOffsceneScene) and Vulkan pbr_mesh (renderFrameSceneScene) — then
// renders identical pixels without a dedicated GL terrain-splat shader.
// Height is baked into pos.y (pbr_mesh.vert has no heightmap sampler; only
// shaderSystem/terrain_splat.* displaces in-shader). Layer-0 albedo is stbi-
// loaded from the config path (not glGetTexImage) so the GL and Vulkan RHI
// backends receive identical bytes even when the GL context is absent.
void RenderPipeline::addTerrainToScene(RHI::OffscreenScene& scene,
                                       const glm::vec3& cameraPosition) const {
    if (!m_terrain || m_terrain->getActiveChunkCount() == 0) return;
    const auto& cfg = m_terrain->getConfig();
    const float heightScale = cfg.heightScale;
    const float chunkSize   = cfg.chunkSize;
    // Cap the bake tessellation: GL terrain_splat displaces via a heightmap
    // TEXTURE (full res), but the offscreen generic path bakes height into the
    // vertex buffer, so cap to ~256 to keep the per-frame upload sane.
    const int   bakeRes = std::min(cfg.chunkResolution, 256);
    const float maxDist = (cfg.viewDistance + 2) * chunkSize;

    // Cached layer-0 albedo pixels, keyed by Terrain. stbi is GL-context-free
    // (shaderSystem/stb_image.cpp), so the bytes are identical for the GL offscreen
    // and Vulkan backends -> parity. Flat grass fallback if the asset is missing.
    static std::unordered_map<const Terrain*, std::vector<uint8_t>> s_albedo;
    static std::unordered_map<const Terrain*, std::pair<int, int>>  s_albedoSz;
    if (s_albedo.find(m_terrain) == s_albedo.end()) {
        int w = 0, h = 0, ch = 0;
        std::vector<uint8_t> px;
        const std::string& ap = cfg.materialLayers[0].albedo;
        if (!ap.empty()) {
            // Point 3: never hand a 4K+ albedo to stbi_load (500x4K OOMs the
            // editor heap). Probe dims first via stbi_info (GL-free + cheap);
            // over the kMaxDimension cap falls through to the flat-grass fn below.
            int rw = 0, rh = 0, rch = 0;
            if (stbi_info(ap.c_str(), &rw, &rh, &rch) &&
                texture_budget::DimensionsWithinBudget(rw, rh)) {
                if (unsigned char* d = stbi_load(ap.c_str(), &w, &h, &ch, 4)) {
                    px.assign(d, d + (size_t)w * h * 4);
                    stbi_image_free(d);
                }
            }
        }
        if (px.empty()) { px = {90, 110, 60, 255}; w = h = 1; }
        s_albedo[m_terrain]   = std::move(px);
        s_albedoSz[m_terrain] = {w, h};
    }
    const std::vector<uint8_t>& albedoPx = s_albedo[m_terrain];
    const int albedow = s_albedoSz[m_terrain].first, albedoh = s_albedoSz[m_terrain].second;

    // Cached per-chunk baked mesh (terrain is static; rebuild only on edits).
    // Distance-cull to the near ring each frame; guard cache size for roaming.
    struct CacheKey { const Terrain* t; int cx, cy;
        bool operator<(const CacheKey& o) const {
            if (t != o.t) return t < o.t;
            if (cx != o.cx) return cx < o.cx;
            return cy < o.cy; } };
    static std::map<CacheKey, RHI::OffscreenMesh> s_chunkCache;
    if (s_chunkCache.size() > 256) s_chunkCache.clear();

    for (const auto& e : m_terrain->getChunks()) {
        const TerrainChunk* chunk = e.second.get();
        if (!chunk || !chunk->isLoaded()) continue;
        const int cx = chunk->getChunkX(), cy = chunk->getChunkY();
        // Distance cull (matches the editor viewDistance culler; bounds upload).
        const glm::vec3 center(cx * chunkSize + chunkSize / 2.0f, 0.0f,
                               cy * chunkSize + chunkSize / 2.0f);
        const float dist = glm::distance(glm::vec3(cameraPosition.x, 0.0f, cameraPosition.z), center);
        if (dist > maxDist) continue;

        const CacheKey key{m_terrain, cx, cy};
        auto it = s_chunkCache.find(key);
        if (it == s_chunkCache.end()) {
            // Height-baked grid + CCW indices + layer-0 albedo. Delegates to the
            // pure-CPU Render::bakeTerrainChunk (editor/terrain_bridge.{h,cpp})
            // which is unit-tested headless in tests/test_terrain_bridge.cpp —
            // the test verifies the EXACT bake the rhivulkan/GL offscreen bridge
            // ships. getHeightAt/getNormalAt are Terrain world-coord queries;
            // Terrain is initialized on this path so they are safe.
            RHI::OffscreenMesh om = bakeTerrainChunk(
                cx, cy, bakeRes, chunkSize, heightScale,
                [this](float worldX, float worldZ) -> float {
                    return m_terrain->getHeightAt(worldX, worldZ);
                },
                [this](float worldX, float worldZ) -> glm::vec3 {
                    return m_terrain->getNormalAt(worldX, worldZ);
                },
                albedoPx, albedow, albedoh);
            it = s_chunkCache.emplace(key, std::move(om)).first;
        }
        // Fresh per-instance copy (verts/texture identical on GL+Vulkan); verts
        // are world-space so the instance model is identity.
        RHI::OffscreenMesh mesh = it->second;
        mesh.instances.clear();
        mesh.instances.emplace_back();
        fillInstance(mesh.instances.back(), glm::mat4(1.0f), glm::vec4(1.0f));
        mesh.version = 1;
        scene.meshes.push_back(std::move(mesh));
    }
}

void RenderPipeline::addVegetationToScene(RHI::OffscreenScene& scene,
                                          const glm::vec3& cameraPosition) const {
    if (!m_vegetation) return;
    const auto& trees = m_vegetation->getTrees();
    if (trees.empty()) return;
    // Distance-cull to the near ring (matches the terrain culler order of mag).
    std::vector<VegetationSystem::Tree> nearTrees;
    nearTrees.reserve(trees.size());
    constexpr float maxDist = 256.0f;
    for (const auto& t : trees) {
        if (glm::distance(glm::vec3(cameraPosition.x, 0.0f, cameraPosition.z),
                          glm::vec3(t.position.x, 0.0f, t.position.z)) <= maxDist)
            nearTrees.push_back(t);
    }
    if (nearTrees.empty()) return;
    // Flat tree-green albedo (GL-context-free -> byte-identical on both backends).
    static const std::vector<uint8_t> green = {0, 128, 0, 255};
    RHI::OffscreenMesh foliage = Render::buildFoliageMesh(nearTrees, green, 1, 1);
    foliage.version = 1;
    foliage.instanceVersion = 1;
    scene.meshes.push_back(std::move(foliage));
}

void RenderPipeline::renderScene(const glm::mat4& view, const glm::mat4& projection,
                                  const glm::vec3& cameraPosition, float fov,
                                  const LightingEnvironment& lighting) {
    if (!m_renderer) return;

    // Clear stale lights from the previous scene/level so they don't leak
    // into the current render. The default directional sun is handled by the
    // empty-lights fallback below, so callers only need to add dynamic lights.
    clearLights();

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

            // forRenderer.txt Part 1: spawned ECS primitives must cast CSM shadows
            // too. The per-cascade uLightSpaceMatrix is bound above; feed each
            // primitive's model to the shadow shader's aModel attribute (loc 6).
            renderEcsPrimitivesToShadowMap(m_shadowShader.get());

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
        m_renderer->SubmitBatches();
        m_renderer->Render();
        m_renderer->ClearBatches();

        // forRenderer.txt Part 2: spawned primitives are not submitted to the
        // batch renderer, so inject them into the deferred G-Buffer here (after
        // BeginGBuffer, before EndGBuffer). When the instanced shader + instance
        // VBO are ready they are batched per MeshType (one
        // glDrawElementsInstanced per type); otherwise this falls back to the
        // non-instanced Part-1 draw. The instanced shader (primitive_gbuffer.*)
        // supplies its own view/projection uniforms; the fallback path reuses the
        // gbuf shader whose view/projection were set just above.
        renderEcsPrimitivesInstanced(gbufShader, view, projection);

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
        if (m_skyboxEnabled) {
            glBindFramebuffer(GL_FRAMEBUFFER, m_deferredRenderer->GetLightingFBO());
            glViewport(0, 0, m_viewportWidth, m_viewportHeight);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            // forUI.txt Part 3: proc-sky overrides cubemap when routed; else cubemap.
            if (!renderProceduralSky(view, LEnv.sunDirection) && m_skybox) {
                m_skybox->render(view, projection, LEnv);  // #3: thread env, no Instance() in Skybox::render
            }
            glDepthFunc(GL_LESS);
        }

        // Copy the deferred lighting result into the PostProcess FBO or viewport FBO
        // so bloom + ACES can process it.
        // FIX (forArena.txt Fix 1): The LightingFBO has no depth attachment, so
        // forward overlays (ArenaScene) render without any scene depth to test
        // against — causing violent Z-fighting. We blit color from the LightingFBO
        // (the lit result) AND depth from the G-Buffer FBO (which holds the
        // raw DepthRBO), giving forward overlays correct depth context.
        if (m_bloomEnabled && m_postProcess && m_postProcess->IsInitialized()) {
            // 1a. Lit color from LightingFBO
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_deferredRenderer->GetLightingFBO());
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_postProcess->GetSceneFBO());
            glBlitFramebuffer(0, 0, m_viewportWidth, m_viewportHeight,
                              0, 0, m_viewportWidth, m_viewportHeight,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            // 1b. Scene depth from G-Buffer FBO (LightingFBO has no depth)
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_deferredRenderer->GetGBufferFBO());
            glBlitFramebuffer(0, 0, m_viewportWidth, m_viewportHeight,
                              0, 0, m_viewportWidth, m_viewportHeight,
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        } else {
            // 2a. Lit color from LightingFBO
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_deferredRenderer->GetLightingFBO());
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_framebuffer);
            glBlitFramebuffer(0, 0, m_viewportWidth, m_viewportHeight,
                              0, 0, m_viewportWidth, m_viewportHeight,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            // 2b. Scene depth from G-Buffer FBO
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_deferredRenderer->GetGBufferFBO());
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_framebuffer);
            glBlitFramebuffer(0, 0, m_viewportWidth, m_viewportHeight,
                              0, 0, m_viewportWidth, m_viewportHeight,
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        m_stats.drawCalls += 3;  // gbuffer + lighting + copy
    }

    // ---- SSAO (forward-path only — deferred path integrates SSAO internally)
    // Forward ECS renders through the editor's own renderer (Editor::renderer_),
    // not RenderPipeline::m_renderer — so m_renderer is empty here and the SSAO
    // G-Buffer would be black. Only build forward SSAO when m_renderer actually
    // carries batches. (The forward PBR pass binds a white uSSAOMap below, so
    // ambient is unaffected when this is skipped.)
    if (!usedDeferred && m_ssaoEnabled && m_ssao && m_ssao->IsInitialized() && m_gBufferShader &&
        m_renderer->GetBatchCount() > 0) {
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
    if (m_skyboxEnabled) {
        // forUI.txt Part 3: proc-sky overrides cubemap when routed; else cubemap.
        if (!renderProceduralSky(view, LEnv.sunDirection) && m_skybox) {
            m_skybox->render(view, projection, LEnv);  // #3: env threaded, no Instance()
        }
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

        // SSAO: deferred samples the real AO texture; forward has no AO G-Buffer
        // (ECS renders via the editor forward renderer, not m_renderer), so
        // bind the 1x1 white texture to unit 8 — pbrFS.glsl multiplies ambient
        // by the sampled AO, white = 1.0 = full ambient (no darkening). Mirrors
        // the forward PBR test's unit-8 white bind.
        activeShader->setInt("uSSAOMap", 8);
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, m_whiteTexture);
    }

    // Render scene geometry
    m_renderer->SetViewport(0, 0, m_viewportWidth, m_viewportHeight);
    m_renderer->SetCameraMatrices(view, projection);
    m_renderer->SetLightParameters(lightPos, cameraPosition);
    m_renderer->SubmitBatches();
    m_renderer->Render();
    m_renderer->ClearBatches();
    } // end forward path

    // verify.txt Fix: unbind the CSM (unit 4) and SSAO / white-fallback (unit 8)
    // texture units the forward pass bound above. Stale descriptors left on those
    // slots leaked into downstream ImGui / debug-text draws, causing sampler
    // mismatches (and driver errors) on strict graphics drivers.
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE8);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);  // restore default active texture unit

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
}

void RenderPipeline::renderUI() {
    // UI rendering is handled by the EngineUI system
}

void RenderPipeline::endFrame() {
    // Drive the thread-safe CPU profiler: snapshot + periodic console flush +
    // numeric providers (e.g. /proc/self/status memory stats) run here.
    Profiler::Instance().endFrame();
    END_GPU_FRAME();    // close AdvancedGPUProfiler frame scope (no-op if not initialized)

    // Swap buffers if using default framebuffer
}

void RenderPipeline::resizeViewport(int width, int height) {
    if (width <= 0 || height <= 0 || (width == m_viewportWidth && height == m_viewportHeight)) {
        return;
    }

    // Clamp to a safe minimum: sub-32px FBOs churn the SSAO/SSR/deferred
    // sub-systems (compute workgroups / non-zero texture dimensions) and are
    // the "handled zero/negative" edge the splitter drag exposed at extreme
    // panel collapse. 32px is below any visible viewport anyway (the 10px
    // Image guard + panel clamps keep real sizes far larger on a usable display).
    if (width < 32)  width  = 32;
    if (height < 32) height = 32;

    m_viewportWidth = width;
    m_viewportHeight = height;

    // GROW-ONLY viewport colour/depth storage with power-of-two stepping to
    // avoid reallocations during panel drags (mirrors test.cpp/bin/engine).
    auto growAlloc = [](int needed, int current) -> int {
        if (needed <= current) return current;
        int alloc = current == 0 ? std::max(needed, 32) : current;
        while (alloc < needed) alloc *= 2;
        return alloc;
    };
    const int allocW = growAlloc(width, m_framebufferAllocWidth);
    const int allocH = growAlloc(height, m_framebufferAllocHeight);
    if (m_framebuffer &&
        (allocW != m_framebufferAllocWidth || allocH != m_framebufferAllocHeight)) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
        glBindTexture(GL_TEXTURE_2D, m_framebufferTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, allocW, allocH, 0,
                     GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // Match the creation-time wrap mode (see that call site for the
        // blank-on-resize rationale): render targets clamp to edge so the
        // ImGui viewport Image never samples a tiled/garbage overshoot.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               m_framebufferTexture, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, m_renderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, allocW, allocH);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, m_renderbuffer);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[RenderPipeline] Viewport framebuffer incomplete after resize!"
                      << std::endl;
        }
        m_framebufferAllocWidth  = allocW;
        m_framebufferAllocHeight = allocH;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // Sub-systems stay sized to the live REGION (width x height), not the
    // allocation: their passes composite into the framebuffer at
    // glViewport(0,0,m_viewportWidth,m_viewportHeight) == the region, which is
    // correct regardless of how much storage the colour attachment has.
    if (m_postProcess && m_postProcess->IsInitialized()) {
        m_postProcess->Resize(width, height);
    }
    if (m_ssao && m_ssao->IsInitialized()) {
        m_ssao->Resize(width, height);
    }
    if (m_deferredRenderer && m_deferredRenderer->IsInitialized()) {
        m_deferredRenderer->Resize(width, height);
    }
    if (m_ssr && m_ssr->IsInitialized()) {
        m_ssr->Resize(width, height);
    }
}

void RenderPipeline::setPendingViewportSize(int w, int h) {
    // Latched every UI frame by RenderViewport; applied next frame by
    // EditorApplication::render() before the 3D draw. Storing even un-changed
    // sizes is harmless (applyPendingViewportResize no-ops on no-change).
    m_pendingViewportWidth  = w;
    m_pendingViewportHeight = h;
}

void RenderPipeline::applyPendingViewportResize() {
    // Called at frame start (before renderSceneOnly) so the FBO is resized and
    // then immediately drawn into, before ImGui::Image reads it. Removes the
    // post-draw clear -> blank race.
    if (m_pendingViewportWidth > 0 && m_pendingViewportHeight > 0) {
        resizeViewport(m_pendingViewportWidth, m_pendingViewportHeight);
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

// --- forRenderer.txt Part 2: instanced draw for spawned ECS primitives -------
// Batches every spawned primitive of a given MeshType into one
// glDrawElementsInstanced call (vs. one glDrawElements per primitive in Part 1).
// Uses the dedicated primitive_gbuffer shader; if that shader / instance VBO are
// not available, falls back to the non-instanced Part-1 path
// (renderEcsPrimitivesToGBuffer) so the deferred G-Buffer still receives the
// primitives. Material data flows per-instance: albedo+metallic in
// aInstanceMaterial, roughness in aInstanceRoughness (see PrimitiveInstanceData).
void RenderPipeline::renderEcsPrimitivesInstanced(Shader* gbufShader,
                                                   const glm::mat4& view,
                                                   const glm::mat4& projection) {
    // Part-1 fallback: nothing instanced to draw, or the instanced path is not
    // ready (shader failed to compile at runtime, no GL context, etc.).
    if (!m_primitiveInstanceShader || !m_primitiveInstanceShader->ID ||
        m_primitiveInstanceVBO == 0 || !g_editor.isInitialized()) {
        renderEcsPrimitivesToGBuffer(gbufShader);
        return;
    }

    Shader* sh = m_primitiveInstanceShader.get();
    sh->use();
    sh->setMat4("view", view);
    sh->setMat4("projection", projection);

    // Procedural mesh types are registered as Cube..Torus (enum 0..5). Custom
    // meshes have no MeshRegistry VAO and are skipped (drawn via the model path).
    constexpr int kPrimitiveTypeCount = 6;

    glBindBuffer(GL_ARRAY_BUFFER, m_primitiveInstanceVBO);

    for (int mt = 0; mt < kPrimitiveTypeCount; ++mt) {
        int n = 0;
        g_editor.world().forEach<ecs::TransformComponent, ecs::MeshComponent>(
            [this, mt, &n](auto, ecs::TransformComponent& t, ecs::MeshComponent& m) {
                if (!m.visible) return;
                if (static_cast<int>(m.meshType) != mt) return;
                if (n >= m_maxPrimitiveInstances) return;  // cap per type
                PrimitiveInstanceData& d = m_instanceBuffer[n++];
                d.modelMatrix     = t.getModelMatrix();
                d.materialData    = glm::vec4(m.color, m.metallic);
                d.roughnessData   = glm::vec4(m.roughness, 0.0f, 0.0f, 0.0f);
            });

        if (n == 0) continue;

        const MeshEntry* e = MeshRegistry::getInstance().getMesh(
            static_cast<ecs::MeshType>(mt));
        if (!e || e->VAO == 0 || e->indexCount <= 0) continue;

        // Stream this type's instances (offset 0; one type uploaded at a time).
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        static_cast<GLsizeiptr>(n) * sizeof(PrimitiveInstanceData),
                        m_instanceBuffer);

        // Vertex attribs 0/1/2 come from the mesh VAO; append the per-instance
        // material attribs (layout matches primitive_gbuffer.vert: 3-6/7/8) on
        // top of it. divisor=1 => one model/material per instance.
        glBindVertexArray(e->VAO);
        for (int i = 0; i < 4; ++i) {
            glVertexAttribPointer(3 + i, 4, GL_FLOAT, GL_FALSE,
                                  sizeof(PrimitiveInstanceData),
                                  (void*)(sizeof(glm::vec4) * i));
            glVertexAttribDivisor(3 + i, 1);
            glEnableVertexAttribArray(3 + i);
        }
        glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, sizeof(PrimitiveInstanceData),
                              (void*)(sizeof(glm::vec4) * 4));   // materialData @64
        glVertexAttribDivisor(7, 1);
        glEnableVertexAttribArray(7);
        glVertexAttribPointer(8, 4, GL_FLOAT, GL_FALSE, sizeof(PrimitiveInstanceData),
                              (void*)(sizeof(glm::vec4) * 5));   // roughnessData @80
        glVertexAttribDivisor(8, 1);
        glEnableVertexAttribArray(8);

        glDrawElementsInstanced(GL_TRIANGLES, e->indexCount,
                                GL_UNSIGNED_INT, nullptr, n);

        // Restore the mesh VAO to its pristine state (attribs 0-2 only) so the
        // forward overlay (renderSceneOnly) and the shadow pass are unaffected:
        // loc 3-8 are re-disabled and reset to per-vertex (divisor 0). Shadow
        // depth runs BEFORE this deferred pass; the forward overlay draws
        // primitives non-instanced (uModel) so unused attribs 3-8 are ignored.
        // FIX (forPrimitives.txt Fix 2): Extend cleanup to loc 3-11 to exhaustively
        // wipe all possible instance-layout attributes (model rows 3-6, material 7-8,
        // instance color 9-11). The old bound stopped at 8 — attributes 9-11 set
        // by other shaders (e.g. shadow_depth.vert's aModel@6-9, or other mat4
        // layouts) could leak a stale glVertexAttribDivisor(i, 1), causing
        // strict drivers to expect instanced data on non-instanced forward draws
        // and silently dropping primitives.
        for (int i = 3; i <= 11; ++i) {
            glVertexAttribDivisor(i, 0);
            glDisableVertexAttribArray(i);
        }
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

} // namespace Render
