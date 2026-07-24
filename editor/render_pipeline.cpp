#include "render_pipeline.h"
#include "renderer/Renderer.h"
#include "shaderSystem/Shader.h"
#include <iostream>
#include <GLFW/glfw3.h>

namespace Render {

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

    // Create the renderer
    m_renderer = std::make_unique<Renderer>();
    m_renderer->Initialize();

    // Create default shader
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

    // Create model shader. The engine's canonical model shaders are VS.glsl/FS.glsl
    // (skinned, instanced, textured); modelVS.glsl/modelFS.glsl are not present.
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

    // Create framebuffer for viewport rendering
    glGenFramebuffers(1, &m_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);

    // Create framebuffer texture
    glGenTextures(1, &m_framebufferTexture);
    glBindTexture(GL_TEXTURE_2D, m_framebufferTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_viewportWidth, m_viewportHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
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

    // Initialize skybox (uses assets/skybox/day and assets/skybox/night cubemaps).
    {
        std::vector<std::string> dayFaces = {
            "assets/skybox/day/right.jpg",
            "assets/skybox/day/left.jpg",
            "assets/skybox/day/top.jpg",
            "assets/skybox/day/bottom.jpg",
            "assets/skybox/day/front.jpg",
            "assets/skybox/day/back.jpg"
        };
        std::vector<std::string> nightFaces = {
            "assets/skybox/night/right.jpg",
            "assets/skybox/night/left.jpg",
            "assets/skybox/night/top.jpg",
            "assets/skybox/night/bottom.jpg",
            "assets/skybox/night/front.jpg",
            "assets/skybox/night/back.jpg"
        };
        m_skybox = std::make_unique<Skybox>(dayFaces, nightFaces);
        if (m_skybox && m_skybox->getShaderID() != 0) {
            std::cout << "[RenderPipeline] Skybox initialized" << std::endl;
        } else {
            std::cerr << "[RenderPipeline] Skybox initialization failed" << std::endl;
            m_skybox.reset();
        }
    }

    std::cout << "[RenderPipeline] Framebuffer created (" << m_viewportWidth << "x" << m_viewportHeight << ")" << std::endl;
    std::cout << "[RenderPipeline] Initialized successfully" << std::endl;

    return true;
}

void RenderPipeline::shutdown() {
    static bool alreadyShutdown = false;
    if (alreadyShutdown) return;
    alreadyShutdown = true;

    std::cout << "[RenderPipeline] Shutting down..." << std::endl;

    // Clean up framebuffer
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

    // Clean up skybox
    if (m_skybox) {
        m_skybox->cleanup();
        m_skybox.reset();
    }

    // Clean up shaders
    m_defaultShader.reset();
    m_modelShader.reset();

    // Clean up renderer
    if (m_renderer) {
        m_renderer->Shutdown();
        m_renderer.reset();
    }

    std::cout << "[RenderPipeline] Shutdown complete" << std::endl;
}

void RenderPipeline::beginFrame() {
    // Update frame statistics
    static double lastTime = glfwGetTime();
    static int frameCount = 0;
    double now = glfwGetTime();
    frameCount++;
    if (now - lastTime >= 1.0) {
        m_stats.fps = (float)(frameCount / (now - lastTime));
        frameCount = 0;
        lastTime = now;
    }
}

void RenderPipeline::renderScene(const glm::mat4& view, const glm::mat4& projection,
                                  const glm::vec3& cameraPosition, float fov) {
    if (!m_renderer) return;

    // Bind the viewport FBO and set its viewport. RenderPipeline owns the FBO
    // binding from here on; the caller's binding is ignored.
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glViewport(0, 0, m_viewportWidth, m_viewportHeight);

    // Skybox renders FIRST, at z=1.0 (far plane) via the Skybox shader's
    // gl_Position = pos.xyww. Depth test = GL_LEQUAL so it draws where nothing
    // else has yet written to the depth buffer (which is cleared to 1.0).
    if (m_skyboxEnabled && m_skybox) {
        m_skybox->render(view, projection);
    }

    // Restore standard depth func for scene rendering. The Skybox render path
    // leaves glDepthFunc=GL_LESS and glCullFace enabled.
    glDepthFunc(GL_LESS);

    // Set up renderer with camera matrices
    m_renderer->SetCameraMatrices(view, projection);
    m_renderer->SetLightParameters(glm::vec3(100.0f, 100.0f, 50.0f), cameraPosition);

    // Render scene geometry. The renderer manages its own viewport via
    // glViewport(viewportX/Y, viewportW/H) but the FBO binding is preserved.
    m_renderer->SubmitBatches();
    m_renderer->Render();
    m_renderer->ClearBatches();

    // Restore FBO binding + viewport in case the renderer perturbed them.
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glViewport(0, 0, m_viewportWidth, m_viewportHeight);

    // Update stats
    m_stats.drawCalls += m_renderer->GetBatchCount();
}

void RenderPipeline::renderUI() {
    // UI rendering is handled by the EngineUI system
    // This method is a placeholder for integration
}

void RenderPipeline::endFrame() {
    // Swap buffers if using default framebuffer
    // (This is typically done by the main application)
}

void RenderPipeline::resizeViewport(int width, int height) {
    if (width <= 0 || height <= 0 || (width == m_viewportWidth && height == m_viewportHeight)) {
        return;
    }

    m_viewportWidth = width;
    m_viewportHeight = height;

    // Recreate framebuffer with new size
    if (m_framebuffer) {
        glBindTexture(GL_TEXTURE_2D, m_framebufferTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glBindRenderbuffer(GL_RENDERBUFFER, m_renderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    }
}

unsigned int RenderPipeline::getFramebufferTexture() const {
    return m_framebufferTexture;
}

} // namespace Render
