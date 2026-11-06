/**
 * OpenGL RHI backend.
 *
 * Owns the GLFW window + OpenGL context (via GLAD) and the presentation
 * loop. This is the "low / optimized" graphics mode: the engine's existing
 * GL renderer runs unchanged between beginFrame() and endFrame().
 */
#include "RHI.h"
#include "RHIMath.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "../editor/gl_context_lifecycle.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <unordered_map>
#include <string>
#include <cstring>

namespace RHI {

// ---------------------------------------------------------------------------
// Graphics API selection state (shared by both backends)
// ---------------------------------------------------------------------------
namespace {

GraphicsAPI g_activeApi = GraphicsAPI::OpenGL;   // default: low/optimized mode
constexpr const char* kApiConfigFile = "graphics_api.cfg";

// Per-program glGetUniformLocation cache (mirrors Renderer::GetCachedUniformLocation
// in renderer/Renderer.h). The forward path calls glGetUniformLocation on EVERY
// draw / per-frame uniform bind (drawMeshes uWindStrength/uTime/uShadowMap, and
// the uJitter block across m_scene/m_mesh/m_pbr/m_gbuffer programs); each call
// is a driver string-hash lookup. Caching the GLint removes that per-draw hash.
// Caching the -1 result is identical to the raw call (GL no-op), so all parity
// tests (windSway==0, jitter==0) stay pixel-identical. Stale entries for a
// recycled program handle are harmless: programs are stable for the whole frame
// that reads the cache (shader hot-swap rebuilds programs -> cache misses ->
// repopulates under the new handle).
static GLint gloc(GLuint program, const char* name) {
    static std::unordered_map<GLuint, std::unordered_map<std::string, GLint>> s_cache;
    auto& byName = s_cache[program];
    const auto it = byName.find(name);
    if (it != byName.end()) return it->second;
    const GLint loc = glGetUniformLocation(program, name);
    byName.emplace(name, loc);
    return loc;
}

} // namespace

GraphicsAPI activeGraphicsApi() { return g_activeApi; }

bool setGraphicsApi(GraphicsAPI api) {
    if (api == GraphicsAPI::OpenGL) {
        g_activeApi = api;
        std::ofstream f(kApiConfigFile);
        f << "opengl\n";
        return true;
    }
    // Vulkan: only persist when this machine can actually run it.
    auto probe = createRHI(GraphicsAPI::Vulkan);
    if (!probe || !probe->available()) {
        std::cerr << "[RHI] Vulkan unavailable on this machine - keeping OpenGL"
                  << std::endl;
        return false;
    }
    g_activeApi = api;
    std::ofstream f(kApiConfigFile);
    f << "vulkan\n";
    return true;
}

const char* graphicsApiConfigPath() { return kApiConfigFile; }

namespace {

class RHIGL final : public IRHI {
public:
    GraphicsAPI api() const override { return GraphicsAPI::OpenGL; }

    bool available() const override {
        // GLFW + the GL loader are effectively always present when the engine
        // builds; the authoritative probe is a real context creation, which
        // initialize() does. Report availability optimistically here.
        return true;
    }

    bool initialize(int w, int h, const char* title) override {
        if (m_window) return true;   // already initialized

        if (!glfwInit()) {
            std::cerr << "[RHI-GL] Failed to initialize GLFW" << std::endl;
            return false;
        }

        // Core 4.5 profile - matches what the engine's GL code expects.
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

        m_window = glfwCreateWindow(w, h, title, nullptr, nullptr);
        if (!m_window) {
            std::cerr << "[RHI-GL] Failed to create window" << std::endl;
            glfwTerminate();
            return false;
        }
        glfwMakeContextCurrent(m_window);
        // VSync: default ON (glfwSwapInterval(1)) — ties FPS to the display
        // refresh. On a context that fell back to software rasterization the
        // refresh may be ~3 Hz (hence the 3-FPS boot); --no-vsync / RTT_NO_VSYNC
        // disables it so the real frame rate is measurable and uncapped.
        glfwSwapInterval(RHI::skipVSync() ? 0 : 1);

        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            std::cerr << "[RHI-GL] Failed to initialize GLAD" << std::endl;
            glfwDestroyWindow(m_window);
            m_window = nullptr;
            glfwTerminate();
            return false;
        }

        // The whole engine relies on glctx::isAlive() (see
        // editor/gl_context_lifecycle.h) to decide whether GL calls are safe
        // from destructors - e.g. static-destruction-time teardown of global
        // model/shader caches. The RHI owns the context now, so it must be
        // the one to maintain that flag.
        glctx::setAlive(true);

        m_width = w;
        m_height = h;
        std::cout << "[RHI-GL] OpenGL " << glGetString(GL_VERSION)
                  << " (vendor: " << glGetString(GL_VENDOR) << ")" << std::endl;
        // Detect software rasterizers (llvmpipe / softpipe / sWRast). Under
        // software GL the full multi-pass scene renders at a few FPS, so the
        // RenderPipeline drops CSM/SSAO/Bloom/Deferred at startup. The vendor/
        // renderer string above is the definitive way to confirm host GL.
        const char* renderer = (const char*)glGetString(GL_RENDERER);
        const bool isSwrast = renderer &&
            (strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe") ||
             strstr(renderer, "sWRast") || strstr(renderer, "llvmpipe"));
        setSoftwareRasterizer(isSwrast);
        if (isSwrast) {
            std::cout << "[RHI-GL] Software rasterizer detected ("
                      << (renderer ? renderer : "unknown")
                      << ") - CSM/SSAO/Bloom/Deferred will be disabled for responsiveness." << std::endl;
        }
        return true;
    }

    void shutdown() override {
        // Free GL-owned resources while the context is still alive.
        destroyEasuResources();
        destroySceneResources();
        // Phase-B B4 skybox cubemaps: final teardown (persistent across renders).
        for (GLuint t : m_cubemaps) if (t) glDeleteTextures(1, &t);
        m_cubemaps.clear();
        // Flag the context dead BEFORE destroying the window/terminating:
        // anything destroyed after this point (global Model/Shader/Texture
        // caches at static destruction, for example) must not issue GL calls.
        glctx::setAlive(false);
        if (m_window) {
            glfwDestroyWindow(m_window);
            m_window = nullptr;
        }
        glfwTerminate();
    }

    GLFWwindow* window() const override { return m_window; }

    bool beginFrame() override {
        if (!m_window) return false;
        return true;
    }

    void endFrame() override {
        if (m_window) {
            glfwSwapBuffers(m_window);
            glfwPollEvents();
        }
    }

    int width() const override { return m_width; }
    int height() const override { return m_height; }

    bool renderOffscreenTriangle(int w, int h, unsigned char* outRGBA) override {
        if (!m_window) return false;
        ensureTriangleResources();
        if (!m_triProgram) return false;

        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(m_triProgram);
        glBindVertexArray(m_triVAO);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glUseProgram(0);

        // Read back the offscreen target (the default framebuffer here). GL
        // readback is bottom-up; the RHI contract says row 0 = TOP (matching
        // the Vulkan copy), so flip.
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, outRGBA);
        FlipRows(outRGBA, w, h);
        return true;
    }

    uint32_t createCubemapTexture(int w, int h, const uint8_t* faces[6]) override {
        if (!m_window || w <= 0 || h <= 0) return 0;
        for (int f = 0; f < 6; ++f) if (!faces[f]) return 0;
        GLuint tex; glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
        for (int f = 0; f < 6; ++f)
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8,
                         w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, faces[f]);
        glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        // Sharpen mipmap sampling to match the Vulkan backend, which applies a
        // -0.5 LOD bias on the skybox sampler. Without this the GL cubemap reads
        // one mip level softer than Vulkan.
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_LOD_BIAS, -0.5f);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        m_cubemaps.push_back(tex);
        return (uint32_t)m_cubemaps.size();   // 1-indexed (0 == null)
    }

    void destroyCubemapTexture(uint32_t handle) override {
        if (handle == 0 || handle > m_cubemaps.size()) return;
        GLuint tex = m_cubemaps[handle - 1];
        if (tex) glDeleteTextures(1, &tex);
        m_cubemaps[handle - 1] = 0;
    }

    bool renderFrameScene(const OffscreenScene& scene) override {
        // GL analog of the Vulkan swapchain render: draw the scene into the
        // window's DEFAULT framebuffer (which GLFW gives a depth buffer).
        // endFrame() then swaps it to screen. This is the GL path the
        // editor's windowed rendering can use; the engine renderer currently
        // draws via its own GL code between beginFrame/endFrame.
        if (!m_window || (scene.instances.empty() && scene.meshes.empty())) return false;
        // Phase B bridge: skybox / CSM / SSAO / SSR / bloom are threaded in
        // OffscreenScene but not yet implemented on the GL swapchain path. Log
        // ONCE (forward rendering stays pixel-identical to offline parity).
        static bool s_rhiPhaseBNotice = false;
        if (!s_rhiPhaseBNotice && (scene.skyEnabled || scene.shadows || scene.ssao || scene.ssr || scene.bloom)) {
            s_rhiPhaseBNotice = true;
            if (scene.skyEnabled) std::cerr << "[RHI-GL] skybox  : ported (B4 forward pass)\n";
            if (scene.shadows)    std::cerr << "[RHI-GL] CSM     : B1-forward ported (GL <-> Vulkan parity)\n";
            if (scene.ssao)       std::cerr << "[RHI-GL] SSAO    : not yet ported              [Phase B B2]\n";
            if (scene.ssr)        std::cerr << "[RHI-GL] SSR     : not yet ported              [Phase B B3]\n";
            if (scene.bloom)      std::cerr << "[RHI-GL] bloom   : not yet ported              [Phase B B5]\n";
        }
        if (!ensureSceneResources(m_width, m_height)) return false;

        uploadSceneUBO(scene, m_width, m_height);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_width, m_height);
        glClearColor(scene.clearColor[0], scene.clearColor[1],
                     scene.clearColor[2], scene.clearColor[3]);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);

        // Phase-B B4 skybox (twin of Vulkan drawSkyboxVk; only in renderOffsceneScene
        // is it verified — this swapchain hook is Vega-only compile-coverage).
        if (scene.skyEnabled && scene.skyboxCubemap != 0 &&
            scene.skyboxCubemap <= static_cast<int>(m_cubemaps.size())) {
            glUseProgram(m_skyboxProgram);
            glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_sceneUBO);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_CUBE_MAP, m_cubemaps[scene.skyboxCubemap - 1]);
            glUniform1i(glGetUniformLocation(m_skyboxProgram, "u_sky"), 0);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_FALSE);
            glBindVertexArray(m_sceneVAO);
            glDrawArrays(GL_TRIANGLES, 0, 36);
            glBindVertexArray(0);
            glUseProgram(0);
            glDepthFunc(GL_LESS);
            glDepthMask(GL_TRUE);
        }

        glUseProgram(m_sceneProgram);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_sceneUBO);

        glBindVertexArray(m_sceneVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_sceneInstanceVBO);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(scene.instances.size() * sizeof(OffscreenInstance)),
                     scene.instances.data(), GL_DYNAMIC_DRAW);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 36, static_cast<GLsizei>(scene.instances.size()));
        glBindVertexArray(0);
        glUseProgram(0);

        drawMeshes(scene);
        return true;
    }

    bool initializeImGui() override {
        // No-op on GL: the editor's own imgui_context owns the ImGui context
        // and its GLFW/OpenGL3 backends - the RHI does not manage ImGui here.
        return true;
    }

    bool renderFrameImGui(ImDrawData* /*drawData*/) override {
        // GL renders ImGui through the editor's imgui_context (imgui_impl_
        // opengl3), not through the RHI. Report "not handled" so callers can
        // fall back to the editor's own render path.
        return false;
    }

    bool renderOffscreenScene(int w, int h, const OffscreenScene& scene,
                              unsigned char* outRGBA,
                              float* outVelocityRG = nullptr,
                              bool deferred = false,
                              unsigned char* outGBufferAlbedo = nullptr) override {
        if (!m_window || (scene.instances.empty() && scene.meshes.empty())) return false;
        if (!ensureSceneResources(w, h)) return false;

        // Camera -> viewProj, GL convention (no clip correction needed).
        uploadSceneUBO(scene, w, h);

        if (deferred) {
            // GL twin of Vulkan's renderOffscreenScene(deferred=true):
            // geometry pass -> 2-attachment G-Buffer (albedo RGBA8 +
            // normal/R16f), fullscreen lighting pass -> m_sceneFBO color0,
            // readback final lit color (+ optional G-Buffer albedo). The
            // lighting math is ported verbatim from rhi/shaders/lighting.frag
            // (Lambert + ambient), so GL and Vulkan deferred pixels match.
            if (!m_gbufferProgram || !m_lightingProgram) return false;

            // --- Geometry pass: albedo + normal/roughness to G-Buffer ---
            glBindFramebuffer(GL_FRAMEBUFFER, m_gbufferFBO);
            glViewport(0, 0, w, h);
            const GLenum gbDraws[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
            glDrawBuffers(2, gbDraws);
            const GLfloat clearAl[4] = {0.0f, 0.0f, 0.0f, 1.0f};     // albedo  (matches gAlAtt 0,0,0,1)
            const GLfloat clearNr[4] = {0.0f, 0.0f, 1.0f, 1.0f};     // normal  (matches gNrAtt 0,0,1,1 = +Z)
            glClearBufferfv(GL_COLOR, 0, clearAl);
            glClearBufferfv(GL_COLOR, 1, clearNr);
            glClearDepth(1.0f);
            glClear(GL_DEPTH_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
            glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CCW);
            glUseProgram(m_gbufferProgram);
            glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_sceneUBO);
            glActiveTexture(GL_TEXTURE0);
            drawMeshes(scene, /*gbufferPass=*/true);   // meshes only (no cubes; matches Vulkan)
            glBindVertexArray(0);
            glUseProgram(0);

            // --- B2 SSAO: geometry pass done -> AO factor -> bilateral blur ---
            // Samples the G-Buffer depth (samplerable depth texture) + world
            // normal; writes AO to m_ssaoAoBlur which the lighting pass below
            // multiplies into the ambient term. NO-OP (white AO = 1.0) when
            // scene.ssao is false -> 60/60 deferred parity stays byte-identical.
            if (scene.ssao) {
                ensureSsaoResources(w, h);
                uploadSsaoUBO(scene, w, h);
                if (m_ssaoProgram && m_ssaoBlurProgram) {
                    glUseProgram(m_ssaoProgram);
                    glBindTextureUnit(0, m_gDepthTex);
                    glBindTextureUnit(1, m_gNormalTex);
                    glBindTextureUnit(2, m_ssaoNoiseTex);
                    glBindBufferBase(GL_UNIFORM_BUFFER, 3, m_ssaoKernelUbo);
                    glBindImageTexture(4, m_ssaoAoRaw, 0, GL_FALSE, 0,
                                       GL_WRITE_ONLY, GL_R8);
                    glDispatchCompute((w + 15) / 16, (h + 15) / 16, 1);
                    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                    glBindImageTexture(4, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);

                    glUseProgram(m_ssaoBlurProgram);
                    glBindTextureUnit(0, m_ssaoAoRaw);
                    glBindImageTexture(1, m_ssaoAoBlur, 0, GL_FALSE, 0,
                                       GL_WRITE_ONLY, GL_R8);
                    glDispatchCompute((w + 15) / 16, (h + 15) / 16, 1);
                    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                    glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
                    glUseProgram(0);
                    glBindTexture(GL_TEXTURE_2D, 0);
                }
            }

            // --- Lighting pass: fullscreen triangle -> scene color0 ---
            glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFBO);
            const GLenum sceneDraw = GL_COLOR_ATTACHMENT0;
            glDrawBuffer(sceneDraw);
            glViewport(0, 0, w, h);
            glClearColor(scene.clearColor[0], scene.clearColor[1],
                         scene.clearColor[2], scene.clearColor[3]);
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(m_lightingProgram);
            glUniform1i(glGetUniformLocation(m_lightingProgram, "uAlbedo"), 0);
            glUniform1i(glGetUniformLocation(m_lightingProgram, "uNormalRough"), 1);
            glUniform1i(glGetUniformLocation(m_lightingProgram, "uAo"), 2);   // B2 SSAO (white default = no-op)
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_gAlbedoTex);
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_gNormalTex);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, scene.ssao ? m_ssaoAoBlur : m_white1x1Ao);
            glDisable(GL_DEPTH_TEST);   // lighting pass has no depth test (matches Vulkan lRI.pDepthAttachment=nullptr)
            glBindVertexArray(m_lightingVAO);
            glDrawArrays(GL_TRIANGLES, 0, 3);   // fullscreen triangle
            glBindVertexArray(0);
            glUseProgram(0);

            // --- B5 Bloom: brightpass -> gauss blur -> additive composite ---
            // Reads the just-rendered scene color (m_sceneColorTex) into
            // m_bloomFinal; the readback below then samples m_bloomFinal so the
            // bloomed image is what the host sees. NO-OP (readback unchanged)
            // when scene.bloom is false -> preserves the 60/60 deferred parity.
            if (scene.bloom) {
                ensureBloomResources(w, h);
                dispatchBloom(w, h);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, m_bloomFinal, 0);
            }

            // --- readback: final lit color + G-Buffer albedo ---
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            if (outRGBA) glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, outRGBA);
            if (outGBufferAlbedo) {
                glBindTexture(GL_TEXTURE_2D, m_gAlbedoTex);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, outGBufferAlbedo);
                FlipRows(outGBufferAlbedo, w, h);
            }
            // Parity fix (Task 3): reset all active texture units so the G-Buffer
            // descriptors bound during the lighting pass don't leak to the caller.
            for (int i = 0; i < 16; ++i) {
                glBindTextureUnit(i, 0);
            }
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (outRGBA) FlipRows(outRGBA, w, h);
            return true;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFBO);
        glViewport(0, 0, w, h);
        glClearColor(scene.clearColor[0], scene.clearColor[1],
                     scene.clearColor[2], scene.clearColor[3]);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        // glClear above stamps scene.clearColor into ALL draw buffers (incl.
        // the RG32f velocity attachment, which would read 0.04 gray). Clear
        // the velocity attachment (draw buffer 1) to 0 so pixels with no mesh
        // show "no motion" -- matches Vulkan's velAtt.clearValue = (0,0,0,0).
        const GLfloat kVelClearZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        glClearBufferfv(GL_COLOR, 1, kVelClearZero);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        // Match Vulkan's VK_CULL_MODE_BACK_BIT + VK_FRONT_FACE_COUNTER_CLOCKWISE.
        // Without this the GL backend renders back faces too, producing more
        // pixels and breaking GL-vs-Vulkan pixel parity.
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);

        // Phase-B B4 skybox — twin of Vulkan drawSkyboxVk: LEQUAL, depthWrite off,
        // z=w clamps the cube to the far plane so opaque cubes/occluders win.
        if (scene.skyEnabled && scene.skyboxCubemap != 0 &&
            scene.skyboxCubemap <= (int)m_cubemaps.size()) {
            glUseProgram(m_skyboxProgram);
            glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_sceneUBO);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_CUBE_MAP, m_cubemaps[scene.skyboxCubemap - 1]);
            glUniform1i(glGetUniformLocation(m_skyboxProgram, "u_sky"), 0);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_FALSE);
            glBindVertexArray(m_sceneVAO);   // cube VA (aPos @ loc0, 36 verts)
            glDrawArrays(GL_TRIANGLES, 0, 36);
            glBindVertexArray(0);
            glUseProgram(0);
            glDepthFunc(GL_LESS);
            glDepthMask(GL_TRUE);
        }

        glUseProgram(m_sceneProgram);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_sceneUBO);

        glBindVertexArray(m_sceneVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_sceneInstanceVBO);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(scene.instances.size() * sizeof(OffscreenInstance)),
                     scene.instances.data(), GL_DYNAMIC_DRAW);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 36, static_cast<GLsizei>(scene.instances.size()));
        glBindVertexArray(0);
        glUseProgram(0);

        drawMeshes(scene);

        // B5 Bloom (forward path): composite brights into m_bloomFinal and
        // redirect readback to it when scene.bloom is true.
        if (scene.bloom) {
            ensureBloomResources(w, h);
            dispatchBloom(w, h);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, m_bloomFinal, 0);
        }

        // Readback while the FBO is still bound, then flip to row 0 = top.
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        if (outRGBA) glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, outRGBA);
        if (outVelocityRG) {
            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glReadPixels(0, 0, w, h, GL_RG, GL_FLOAT, outVelocityRG);
            RHI::FlipRows(outVelocityRG, w, h);
        }
        // Parity fix (Task 3): reset all active texture units (skybox cubemap on
        // unit 0, scene textures) before yielding so ImGui/forward passes don't
        // inherit stale descriptors.
        for (int i = 0; i < 16; ++i) {
            glBindTextureUnit(i, 0);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (outRGBA) FlipRows(outRGBA, w, h);
        return true;
    }

    // FSR3 EASU (Phase 4 + Phase 5b GL fallback). GL 3.3 has no compute
    // shaders, so the upsampler is a fullscreen-fragment pass that samples the
    // half-res scene color texture with AMD's canonical ffxFsrEasuFloat
    // 12-tap edge-directed filter (ported verbatim from
    // FidelityFX-SDK-FSR3/sdk/include/FidelityFX/gpu/fsr1/ffx_fsr1.h — the
    // float path). This gives the GL backend real FSR3 EASU parity with the
    // Vulkan compute shader, instead of a 1:1 passthrough. No AMD headers are
    // #included (they use Vulkan layout(set,binding) syntax) — the math is
    // inlined here as a standalone GLSL 3.30 program.
    bool renderOffscreenSceneEasu(int outW, int outH, const OffscreenScene& scene,
                                  unsigned char* outRGBA) override {
        if (!m_window || (scene.instances.empty() && scene.meshes.empty())) return false;
        if (outW < 2 || outH < 2) return false;
        const int lowW = outW / 2, lowH = outH / 2;   // half-res G-buffer

        // 1. Render the scene at HALF the output resolution into m_sceneColorTex
        //    (no readback — the texture persists as the EASU input). Reuses the
        //    exact draw path (camera UBO, depth, culling, meshes) that
        //    renderOffscreenScene uses, so the low-res G-buffer matches both the
        //    Vulkan EASU low-res render and the GL parity render.
        if (!ensureSceneResources(lowW, lowH)) return false;
        uploadSceneUBO(scene, lowW, lowH);
        renderSceneToLowRes(lowW, lowH, scene);

        // 2. Fullscreen EASU fragment pass: sample m_sceneColorTex with the
        //    canonical 12-tap edge-directed filter, writing (outW x outH) into a
        //    dedicated output FBO/color texture. texelFetch point-samples
        //    (matching AMD's gather4 ordering for the flat-field case); the
        //    filter state on m_sceneColorTex is irrelevant to it.
        if (!ensureEasuResources(outW, outH)) return false;
        glBindFramebuffer(GL_FRAMEBUFFER, m_easuOutFBO);
        glViewport(0, 0, outW, outH);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(m_easuProgram);
        // EASU constants — mirror VulkanRHI::makeEasuCon (ffxFsrPopulateEasuConstants)
        // bit-for-bit, as plain floats (the VK path bit-casts float->uint32 for a
        // UBO; here uniforms carry the float values directly).
        float con[16];
        easuCon((uint32_t)lowW, (uint32_t)lowH, (uint32_t)outW, (uint32_t)outH, con);
        glUniform4fv(glGetUniformLocation(m_easuProgram, "u_const"), 4, con);
        glUniform2f(glGetUniformLocation(m_easuProgram, "u_outputSize"),
                    (float)outW, (float)outH);
        glUniform1i(glGetUniformLocation(m_easuProgram, "u_inputColor"), 0);
        // Sample the low-res scene color on tex unit 0. textureGather (used by
        // the EASU shader) respects wrap mode, so force CLAMP_TO_EDGE to match
        // AMD's s_LinearClamp and keep border gathers from wrapping.
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_sceneColorTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindVertexArray(m_easuVAO);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);

        // 3. Read back the upscaled (outW x outH) image, bottom-up -> flip to
        //    row 0 = TOP (RHI contract, matching the Vulkan path).
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        if (outRGBA) {
            glReadPixels(0, 0, outW, outH, GL_RGBA, GL_UNSIGNED_BYTE, outRGBA);
            FlipRows(outRGBA, outW, outH);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return true;
    }

    // Draw the scene into the currently-bound (already-sized) scene FBO
    // (m_sceneFBO @ m_sceneW x m_sceneH) WITHOUT readback. Leaves m_sceneFBO
    // bound + m_sceneColorTex populated so callers can either read back
    // (renderOffscreenScene) or sample the texture directly (the EASU pass).
    // Shared between renderOffscreenScene and renderOffscreenSceneEasu so the
    // low-res G-buffer matches the 1:1 GL parity render bit-for-bit.
    void renderSceneToLowRes(int w, int h, const OffscreenScene& scene) {
        // Bind the (already-sized) scene FBO so the clear/draw land in
        // m_sceneColorTex, which the EASU pass samples. renderOffscreenScene
        // binds this same FBO before reading back; here we leave it bound.
        glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFBO);
        glViewport(0, 0, w, h);
        glClearColor(scene.clearColor[0], scene.clearColor[1],
                     scene.clearColor[2], scene.clearColor[3]);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        // Clear the velocity attachment (draw buffer 1) to 0 so empty pixels are
        // "no motion" — matches Vulkan's velAtt.clearValue = (0,0,0,0).
        const GLfloat kVelClearZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        glClearBufferfv(GL_COLOR, 1, kVelClearZero);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        // Match Vulkan's VK_CULL_MODE_BACK_BIT + VK_FRONT_FACE_CCW so GL renders
        // only front faces (parity with the swapchain/VK scene).
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);

        glUseProgram(m_sceneProgram);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_sceneUBO);
        glBindVertexArray(m_sceneVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_sceneInstanceVBO);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(scene.instances.size() * sizeof(OffscreenInstance)),
                     scene.instances.data(), GL_DYNAMIC_DRAW);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 36,
                              static_cast<GLsizei>(scene.instances.size()));
        glBindVertexArray(0);
        glUseProgram(0);

        drawMeshes(scene);
    }

    // Build the EASU constants as plain floats (con0..con3, 16 floats) — the GL
    // equivalent of VulkanRHI::makeEasuCon, mirroring ffxFsrPopulateEasuConstants
    // (ffx_fsr1.h:42). jitterX/jitterY are frame-0 {0,0} here; the GL EASU pass
    // is a single-shot spatial upscale (no temporal reproject), so NDC jitter is
    // not cancelled through a low-res render the way VK's offscreen jitter path
    // is yet — that's a Phase-5 temporal refinement; frame-0 jitter {0,0} is
    // exact and matches the headless Vulkan EASU test.
    static void easuCon(uint32_t renderW, uint32_t renderH,
                        uint32_t outW, uint32_t outH, float* c) {
        auto rcp = [](float x) { return 1.0f / x; };
        const float rW = (float)renderW, rH = (float)renderH;
        c[0]  = rW * rcp((float)outW);          c[1]  = rH * rcp((float)outH);
        c[2]  = 0.5f * rW * rcp((float)outW) - 0.5f;   c[3]  = 0.5f * rH * rcp((float)outH) - 0.5f;
        c[4]  = rcp(rW);                        c[5]  = rcp(rH);         // con1.xy
        c[6]  = rcp(rW);                        c[7]  = -rcp(rH);        // con1.zw
        c[8]  = -rcp(rW);                       c[9]  = 2.0f * rcp(rH);  // con2.xy
        c[10] = rcp(rW);                        c[11] = 2.0f * rcp(rH);  // con2.zw
        c[12] = 0.0f;                           c[13] = 4.0f * rcp(rH);  // con3.xy
        c[14] = 0.0f;                           c[15] = 0.0f;            // con3.zw + sample0(=0: debug sup-off)
    }

    bool ensureEasuResources(int outW, int outH) {
        // The EASU program + fullscreen-quad VAO are size-independent: build once.
        if (!m_easuProgram) {
            static const char* vsSrc =
                "#version 330 core\n"
                "layout(location=0) in vec2 aPos;\n"
                "void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }\n";
            static const char* fsSrc =
                // AMD FSR3 EASU (float path, ffx_fsr1.h) ported to GLSL 3.30.
                // Self-contained: no #include of the Vulkan-layout AMD callbacks
                // (layout(set,binding)); texelFetch gathers replace gather4.
                "#version 330 core\n"
                "precision highp float;\n"
                "uniform sampler2D u_inputColor;\n"
                "uniform vec4 u_const[4];\n"
                "uniform vec2 u_outputSize;\n"
                "out vec4 out_color;\n"
                // AMD's GatherEasuRed/Green/Blue: 4 adjacent texels around a
                // NORMALIZED-uv position (p0..p3 from the EASU constants are
                // normalized UV, NOT texel coords). textureGather is a GL 4.0
                // feature (Mesa doesn't expose it at #version 330), so emulate
                // the gather4 with 4 texelFetch calls at the 2x2 footprint.
                // Returns (s00,s10,s01,s11) = lower-left, lower-right,
                // upper-left, upper-right — matching AMD's bczz/b/c/z/z swizzle.
                "vec4 gatherR(vec2 p){ ivec2 sz=textureSize(u_inputColor,0); ivec2 pp=ivec2(p*vec2(sz));"
                " return vec4(texelFetch(u_inputColor, pp,          0).r,"
                "              texelFetch(u_inputColor, pp+ivec2(1,0), 0).r,"
                "              texelFetch(u_inputColor, pp+ivec2(0,1), 0).r,"
                "              texelFetch(u_inputColor, pp+ivec2(1,1), 0).r); }\n"
                "vec4 gatherG(vec2 p){ ivec2 sz=textureSize(u_inputColor,0); ivec2 pp=ivec2(p*vec2(sz));"
                " return vec4(texelFetch(u_inputColor, pp,          0).g,"
                "              texelFetch(u_inputColor, pp+ivec2(1,0), 0).g,"
                "              texelFetch(u_inputColor, pp+ivec2(0,1), 0).g,"
                "              texelFetch(u_inputColor, pp+ivec2(1,1), 0).g); }\n"
                "vec4 gatherB(vec2 p){ ivec2 sz=textureSize(u_inputColor,0); ivec2 pp=ivec2(p*vec2(sz));"
                " return vec4(texelFetch(u_inputColor, pp,          0).b,"
                "              texelFetch(u_inputColor, pp+ivec2(1,0), 0).b,"
                "              texelFetch(u_inputColor, pp+ivec2(0,1), 0).b,"
                "              texelFetch(u_inputColor, pp+ivec2(1,1), 0).b); }\n"
                "float ffxRecip(float x){ return 1.0/x; }\n"
                "float ffxSaturate(float x){ return clamp(x, 0.0, 1.0); }\n"
                "void fsrEasuTapFloat(inout vec3 aC, inout float aW, vec2 off,"
                " vec2 dir, vec2 len2, float lob, float clp, vec3 color){"
                " vec2 rx; rx.x = off.x*dir.x + off.y*dir.y;"
                " rx.y = off.x*(-dir.y) + off.y*dir.x; rx *= len2;"
                " float d2 = min(dot(rx,rx), clp);"
                " float wB = (2.0/5.0)*d2 - 1.0; float wA = lob*d2 - 1.0;"
                " wB *= wB; wA *= wA; wB = (25.0/16.0)*wB - (25.0/16.0 - 1.0);"
                " float w = wB*wA; aC += color*w; aW += w; }\n"
                "void fsrEasuSetFloat(inout vec2 dir, inout float len, vec2 pp,"
                " bool biS, bool biT, bool biU, bool biV,"
                " float lA, float lB, float lC, float lD, float lE){"
                " float w = 0.0;"
                " if (biS) w = (1.0-pp.x)*(1.0-pp.y);\n"
                " if (biT) w = pp.x*(1.0-pp.y);\n"
                " if (biU) w = (1.0-pp.x)*pp.y;\n"
                " if (biV) w = pp.x*pp.y;\n"
                " float dc=lD-lC, cb=lC-lB, lenX=max(abs(dc),abs(cb)); lenX=ffxRecip(lenX);"
                " float dirX=lD-lB; dir.x += dirX*w; lenX=ffxSaturate(abs(dirX)*lenX); lenX*=lenX; len += lenX*w;"
                " float ec=lE-lC, ca=lC-lA, lenY=max(abs(ec),abs(ca)); lenY=ffxRecip(lenY);"
                " float dirY=lE-lA; dir.y += dirY*w; lenY=ffxSaturate(abs(dirY)*lenY); lenY*=lenY; len += lenY*w; }\n"
                "void main(){"
                " vec2 ip = vec2(floor(gl_FragCoord.x), u_outputSize.y - floor(gl_FragCoord.y));\n"
                " vec2 pp = ip*u_const[0].xy + u_const[0].zw; vec2 fp = floor(pp); pp -= fp;\n"
                " vec2 p0 = fp*u_const[1].xy + u_const[1].zw;\n"
                " vec2 p1 = p0 + u_const[2].xy; vec2 p2 = p0 + u_const[2].zw; vec2 p3 = p0 + u_const[3].xy;\n"
                " vec4 bczzR=gatherR(p0), bczzG=gatherG(p0), bczzB=gatherB(p0);\n"
                " vec4 ijfeR=gatherR(p1), ijfeG=gatherG(p1), ijfeB=gatherB(p1);\n"
                " vec4 klhgR=gatherR(p2), klhgG=gatherG(p2), klhgB=gatherB(p2);\n"
                " vec4 zzonR=gatherR(p3), zzonG=gatherG(p3), zzonB=gatherB(p3);\n"
                " vec4 bczzL=bczzB*0.5+(bczzR*0.5+bczzG); vec4 ijfeL=ijfeB*0.5+(ijfeR*0.5+ijfeG);\n"
                " vec4 klhgL=klhgB*0.5+(klhgR*0.5+klhgG); vec4 zzonL=zzonB*0.5+(zzonR*0.5+zzonG);\n"
                " float bL=bczzL.x,cL=bczzL.y, iL=ijfeL.x,jL=ijfeL.y,fL=ijfeL.z,eL=ijfeL.w,\n"
                " kL=klhgL.x,lL=klhgL.y,hL=klhgL.z,gL=klhgL.w, oL=zzonL.z,nL=zzonL.w;\n"
                " vec2 dir=vec2(0.0); float len=0.0;\n"
                " fsrEasuSetFloat(dir, len, pp, true,  false, false, false, bL,eL,fL,gL,jL);\n"
                " fsrEasuSetFloat(dir, len, pp, false, true,  false, false, cL,fL,gL,hL,kL);\n"
                " fsrEasuSetFloat(dir, len, pp, false, false, true,  false, fL,iL,jL,kL,nL);\n"
                " fsrEasuSetFloat(dir, len, pp, false, false, false, true,  gL,jL,kL,lL,oL);\n"
                " vec2 dir2=dir*dir; float dirR=dir2.x+dir2.y; bool zro=dirR<(1.0/32768.0);\n"
                " dirR=inversesqrt(dirR); dirR=zro?1.0:dirR; if (zro) dir.x=1.0; dir*=dirR;\n"
                " len=len*0.5; len*=len;\n"
                " float stretch=(dir.x*dir.x+dir.y*dir.y)*ffxRecip(max(abs(dir.x),abs(dir.y)));\n"
                " vec2 len2=vec2(1.0+(stretch-1.0)*len, 1.0-0.5*len);\n"
                " float lob=0.5+((1.0/4.0-0.04)-0.5)*len; float clp=ffxRecip(lob);\n"
                " vec3 min4=min(min(vec3(ijfeR.z,ijfeG.z,ijfeB.z), vec3(klhgR.w,klhgG.w,klhgB.w)),\n"
                "               min(vec3(ijfeR.y,ijfeG.y,ijfeB.y), vec3(klhgR.x,klhgG.x,klhgB.x)));\n"
                " vec3 max4=max(max(vec3(ijfeR.z,ijfeG.z,ijfeB.z), vec3(klhgR.w,klhgG.w,klhgB.w)),\n"
                "               max(vec3(ijfeR.y,ijfeG.y,ijfeB.y), vec3(klhgR.x,klhgG.x,klhgB.x)));\n"
                " vec3 aC=vec3(0.0); float aW=0.0;\n"
                " fsrEasuTapFloat(aC,aW, vec2(0.0,-1.0)-pp, dir,len2,lob,clp, vec3(bczzR.x,bczzG.x,bczzB.x));\n"
                " fsrEasuTapFloat(aC,aW, vec2(1.0,-1.0)-pp, dir,len2,lob,clp, vec3(bczzR.y,bczzG.y,bczzB.y));\n"
                " fsrEasuTapFloat(aC,aW, vec2(-1.0,1.0)-pp, dir,len2,lob,clp, vec3(ijfeR.x,ijfeG.x,ijfeB.x));\n"
                " fsrEasuTapFloat(aC,aW, vec2(0.0,1.0)-pp,  dir,len2,lob,clp, vec3(ijfeR.y,ijfeG.y,ijfeB.y));\n"
                " fsrEasuTapFloat(aC,aW, vec2(0.0,0.0)-pp,  dir,len2,lob,clp, vec3(ijfeR.z,ijfeG.z,ijfeB.z));\n"
                " fsrEasuTapFloat(aC,aW, vec2(-1.0,0.0)-pp, dir,len2,lob,clp, vec3(ijfeR.w,ijfeG.w,ijfeB.w));\n"
                " fsrEasuTapFloat(aC,aW, vec2(1.0,1.0)-pp,  dir,len2,lob,clp, vec3(klhgR.x,klhgG.x,klhgB.x));\n"
                " fsrEasuTapFloat(aC,aW, vec2(2.0,1.0)-pp,  dir,len2,lob,clp, vec3(klhgR.y,klhgG.y,klhgB.y));\n"
                " fsrEasuTapFloat(aC,aW, vec2(2.0,0.0)-pp,  dir,len2,lob,clp, vec3(klhgR.z,klhgG.z,klhgB.z));\n"
                " fsrEasuTapFloat(aC,aW, vec2(1.0,0.0)-pp,  dir,len2,lob,clp, vec3(klhgR.w,klhgG.w,klhgB.w));\n"
                " fsrEasuTapFloat(aC,aW, vec2(1.0,2.0)-pp,  dir,len2,lob,clp, vec3(zzonR.z,zzonG.z,zzonB.z));\n"
                " fsrEasuTapFloat(aC,aW, vec2(0.0,2.0)-pp,  dir,len2,lob,clp, vec3(zzonR.w,zzonG.w,zzonB.w));\n"
                " out_color = vec4(min(max4, max(min4, aC*ffxRecip(aW))), 1.0);\n"
                "}\n";
            m_easuProgram = compileProgram(vsSrc, fsSrc);
            if (!m_easuProgram) return false;
            glUseProgram(0);

            glGenVertexArrays(1, &m_easuVAO);
            glBindVertexArray(m_easuVAO);
            glGenBuffers(1, &m_easuVBO);
            // Fullscreen triangle strip (2 triangles, CCW).
            const float q[8] = {-1,-1,  1,-1,  -1,1,  1,1};
            glBindBuffer(GL_ARRAY_BUFFER, m_easuVBO);
            glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
            glBindVertexArray(0);
        }
        // Resize the full-res output texture/FBO per call (cheap; only realloc
        // on size change). Recreate when the requested size differs.
        if (m_easuOutW != outW || m_easuOutH != outH) {
            destroyEasuOut();
            m_easuOutW = outW; m_easuOutH = outH;
            glGenFramebuffers(1, &m_easuOutFBO);
            glGenTextures(1, &m_easuOutTex);
            glBindTexture(GL_TEXTURE_2D, m_easuOutTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, outW, outH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, m_easuOutFBO);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_easuOutTex, 0);
            const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (st != GL_FRAMEBUFFER_COMPLETE) {
                std::cerr << "[RHI-GL] EASU output FBO incomplete ("
                          << std::hex << st << std::dec << ")" << std::endl;
                return false;
            }
        }
        return true;
    }

    void destroyEasuOut() {
        if (m_easuOutFBO) { glDeleteFramebuffers(1, &m_easuOutFBO); m_easuOutFBO = 0; }
        if (m_easuOutTex) { glDeleteTextures(1, &m_easuOutTex); m_easuOutTex = 0; }
        m_easuOutW = m_easuOutH = 0;
    }
    void destroyEasuResources() {
        if (m_easuProgram) { glDeleteProgram(m_easuProgram); m_easuProgram = 0; }
        if (m_easuVAO) { glDeleteVertexArrays(1, &m_easuVAO); m_easuVAO = 0; }
        if (m_easuVBO) { glDeleteBuffers(1, &m_easuVBO); m_easuVBO = 0; }
        destroyEasuOut();
    }

    // Fill the shared camera UBO (96 bytes: viewProj + camPos + fog) - the
    // IDENTICAL bytes the Vulkan backend uploads, so both backends feed the
    // mesh shaders the same values. Uses scene.camera.farPlane so the GL
    // scene doesn't hard-clip the world at 100 units.
    void uploadSceneUBO(const OffscreenScene& scene, int w, int h) {
        CameraUBOData d{};
        const Mat4 proj = mat4Perspective(scene.camera.fovDeg, (float)w / (float)h,
                                          0.1f, scene.camera.farPlane);
        const Mat4 view = mat4LookAt(scene.camera.eye, scene.camera.target, scene.camera.up);
        const Mat4 viewProj = mat4Multiply(proj, view);
        d.viewProj = viewProj;
        // FSR3 history: prevViewProj + jitterOffset staged here (Phase 1) and
        // CONSUMED in Phase 2 -- the Vulkan shaders read jitterOffset@160 and
        // apply it to gl_Position; GL reads the SAME value via the uJitter
        // uniform (GL #version 330 lacks layout(offset=N), so it cannot read
        // the UBO's jitterOffset directly, hence the plain uniform). Frame 0
        // -> {0,0} keeps offscreen renders byte-identical to the reference.
        d.prevViewProj = m_prevViewProj;
        const Vec2 j = ndcJitter((int)m_frame, w, h);
        d.jitterOffset[0] = j.x; d.jitterOffset[1] = j.y;
        d.camPos[0] = scene.camera.eye[0];
        d.camPos[1] = scene.camera.eye[1];
        d.camPos[2] = scene.camera.eye[2];
        d.fogDensity = scene.fogDensity;
        d.fogColor[0] = scene.fogColor[0];
        d.fogColor[1] = scene.fogColor[1];
        d.fogColor[2] = scene.fogColor[2];
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_sceneUBO);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(d), &d, GL_DYNAMIC_DRAW);
        // FSR3 Phase 2: push the per-frame Halton jitter to the GL shaders'
        // uJitter uniform. GL #version 330 has no layout(offset=N) qualifier
        // (that is GLSL 4.20+/ARB_shading_language_420pack, which the offscreen
        // GL 3.3 shaders target), so GL reads the jitter from a plain uniform
        // instead of CameraUBOData.jitterOffset@160 (which the Vulkan shaders
        // read directly from the shared UBO). The injected VALUE is identical
        // on both backends (same Halton sample) -> GL<->Vulkan parity preserved.
        // Frame 0 maps to {0,0}, so the first offscreen render is unshifted.
        // Parity fix: previously this block was gated on m_pbrMeshProgram and
        // omitted m_gbufferProgram, so the deferred G-Buffer geometry pass was
        // never jittered and its temporal history desynced from Vulkan's
        // jitterOffset@160. Push uJitter to every rasterizing vertex stage; the
        // per-program guards make this a no-op on any stage not compiled this
        // run (frame 0 jitter is {0,0}, so offscreen parity tests are unchanged).
        if (m_sceneProgram) {
            glUseProgram(m_sceneProgram);
            glUniform2f(gloc(m_sceneProgram, "uJitter"), j.x, j.y);
        }
        if (m_meshProgram) {
            glUseProgram(m_meshProgram);
            glUniform2f(gloc(m_meshProgram, "uJitter"), j.x, j.y);
        }
        if (m_pbrMeshProgram) {
            glUseProgram(m_pbrMeshProgram);
            glUniform2f(gloc(m_pbrMeshProgram, "uJitter"), j.x, j.y);
        }
        if (m_gbufferProgram) {
            glUseProgram(m_gbufferProgram);
            glUniform2f(gloc(m_gbufferProgram, "uJitter"), j.x, j.y);
        }
        glUseProgram(0);
        // Advance per-frame history: next frame reprojects from this viewProj.
        m_prevViewProj = viewProj;
        ++m_frame;
    }

    // ----------------------------------------------------------------------
    // B2 SSAO (GL twin of the Vulkan ssao.comp / ssao_blur.comp). The SSAO
    // compute shader is a GLSL 4.30 compute program (GL 4.5 context supports
    // compute). It samples the deferred G-Buffer (depth texture + world normal)
    // and writes a screen-space AO factor to an R8 texture that the lighting
    // pass multiplies into the ambient term. The math is the pixel-identical
    // twin of rhi/shaders/ssao.comp (Vulkan NDC z in [0,1]; GL NDC z in [-1,1]
    // -> glSsaoCompSrc uses z*2-1), so GL and Vulkan produce byte-identical AO.
    // ----------------------------------------------------------------------
    void uploadSsaoUBO(const OffscreenScene& scene, int w, int h) {
        RHI::Mat4 proj = RHI::mat4Perspective(scene.camera.fovDeg,
                                              (float)w / (float)h, 0.1f,
                                              scene.camera.farPlane);
        RHI::Mat4 view = RHI::mat4LookAt(scene.camera.eye, scene.camera.target,
                                         scene.camera.up);
        RHI::Mat4 viewProj = RHI::mat4Multiply(proj, view);
        RHI::Mat4 invViewProj = RHI::mat4Inverse(viewProj);
        RHI::SsaoUBOData d{};
        d.viewProj = viewProj;
        d.invViewProj = invViewProj;
        const auto kernel = RHI::generateSsaoSampleKernel(16, /*radius*/0.5f);
        for (int i = 0; i < 16; ++i) {
            d.samples[i][0] = kernel[i * 3 + 0];
            d.samples[i][1] = kernel[i * 3 + 1];
            d.samples[i][2] = kernel[i * 3 + 2];
            d.samples[i][3] = 0.0f;   // std140 vec3 pad
        }
        glBindBufferBase(GL_UNIFORM_BUFFER, 3, m_ssaoKernelUbo);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(d), &d, GL_DYNAMIC_DRAW);
    }

    void ensureSsaoResources(int w, int h) {
        if (!m_ssaoKernelUbo) {
            glGenBuffers(1, &m_ssaoKernelUbo);
            // 1x1 white fallback so uAo samples 1.0 (ambient untouched) on the
            // offline/60-60 path -> preserves GL<->Vulkan deferred parity.
            glGenTextures(1, &m_white1x1Ao);
            glBindTexture(GL_TEXTURE_2D, m_white1x1Ao);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE,
                         (void*)nullptr);
            uint8_t w1[1] = { 255 };
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, w1);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            // 4x4 RG8 noise (shared generator -> parity with Vulkan).
            const auto noise = RHI::generateSsaoNoiseR8G8();
            glGenTextures(1, &m_ssaoNoiseTex);
            glBindTexture(GL_TEXTURE_2D, m_ssaoNoiseTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, 4, 4, 0, GL_RG, GL_UNSIGNED_BYTE, noise.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            // SSAO compute programs (GLSL 4.30). Twin math to rhi/shaders/ssao.comp
            // (Vulkan); GL NDC z remapped to [-1,1] via z*2-1.
            static const char* ssaoCsSrc =
                "#version 430\n"
                "layout(local_size_x=16, local_size_y=16) in;\n"
                "layout(binding=0) uniform sampler2D uDepth;\n"
                "layout(binding=1) uniform sampler2D uNormal;\n"
                "layout(binding=2) uniform sampler2D uNoise;\n"
                "layout(std140, binding=3) uniform SsaoUBO { mat4 viewProj;"
                " mat4 invViewProj; vec3 samples[16]; };\n"
                "layout(binding=4, r8) uniform writeonly image2D outAO;\n"
                "const float kRadius=0.5, kBias=0.001;\n"
                "vec3 worldPosFromDepth(vec2 uv, float z){\n"
                "  vec4 clip=vec4(uv*2.0-1.0, z*2.0-1.0, 1.0);\n"   // GL NDC z in [-1,1]
                "  vec4 wp=invViewProj*clip; return wp.xyz/wp.w; }\n"
                "void main(){\n"
                "  ivec2 p=ivec2(gl_GlobalInvocationID.xy);\n"
                "  ivec2 sz=imageSize(outAO);\n"
                "  if(p.x>=sz.x||p.y>=sz.y) return;\n"
                "  vec2 uv=(vec2(p)+0.5)/vec2(sz);\n"
                "  float d=texture(uDepth,uv).r;\n"
                "  if(d>=1.0){ imageStore(outAO,p,vec4(1.0)); return; }\n"
                "  vec3 pos=worldPosFromDepth(uv,d);\n"
                "  vec3 N=normalize(texture(uNormal,uv).rgb);\n"
                "  ivec2 nsz=textureSize(uNoise,0);\n"
                "  vec2 rnd=texture(uNoise,uv*vec2(sz)/vec2(nsz)).rg;\n"
                "  vec3 tang=abs(N.z)<0.99?cross(N,vec3(0,0,1)):cross(N,vec3(0,1,0));\n"
                "  tang=normalize(tang); vec3 bitang=normalize(cross(N,tang));\n"
                "  float c=rnd.r, s=rnd.g;\n"
                "  vec3 T=tang*c-bitang*s; vec3 B=tang*s+bitang*c;\n"
                "  mat3 basis=mat3(T,B,N);\n"
                "  float ao=0.0;\n"
                "  for(int i=0;i<16;++i){ vec3 spt=pos+basis*samples[i];\n"
                "    vec4 sp=viewProj*vec4(spt,1.0); sp/=sp.w;\n"
                "    vec2 suv=sp.xy*0.5+0.5;\n"
                "    if(suv.x<0.0||suv.x>1.0||suv.y<0.0||suv.y>1.0) continue;\n"
                "    float sd=texture(uDepth,suv).r;\n"
                "    if(sd>=1.0) continue;\n"
                "    float sZ=worldPosFromDepth(suv,sd).z;\n"
                "    float rc=smoothstep(0.0,1.0,kRadius/(abs(pos.z-sZ)+1e-3));\n"
                "    ao += ((sZ>spt.z+kBias)?1.0:0.0)*rc; }\n"
                "  ao=1.0-clamp(ao/16.0,0.0,1.0);\n"
                "  imageStore(outAO,p,vec4(ao)); }\n";
            static const char* ssaoBlurCsSrc =
                "#version 430\n"
                "layout(local_size_x=16, local_size_y=16) in;\n"
                "layout(binding=0) uniform sampler2D uAO;\n"
                "layout(binding=1, r8) uniform writeonly image2D outBlur;\n"
                "void main(){\n"
                "  ivec2 p=ivec2(gl_GlobalInvocationID.xy);\n"
                "  ivec2 sz=imageSize(outBlur);\n"
                "  if(p.x>=sz.x||p.y>=sz.y) return;\n"
                "  float sum=0.0,w=0.0;\n"
                "  for(int dy=-2;dy<=2;++dy) for(int dx=-2;dx<=2;++dx){ ivec2 q=p+ivec2(dx,dy);\n"
                "    float a=texture(uAO,(q+0.5)/vec2(sz)).r;\n"
                "    float wg=1.0/(1.0+float(dx*dx+dy*dy)); sum+=a*wg; w+=wg; }\n"
                "  imageStore(outBlur,p,vec4(sum/w)); }\n";
            auto link = [&](const char* name, const char* src) -> GLuint {
                GLuint cs = glCreateShader(GL_COMPUTE_SHADER);
                glShaderSource(cs, 1, &src, nullptr);
                glCompileShader(cs);
                GLint ok = 0, logLen = 0;
                glGetShaderiv(cs, GL_COMPILE_STATUS, &ok);
                if (ok == GL_FALSE) {
                    glGetShaderiv(cs, GL_INFO_LOG_LENGTH, &logLen);
                    std::vector<char> log(logLen);
                    glGetShaderInfoLog(cs, logLen, nullptr, log.data());
                    std::cerr << "[RHI-GL] SSAO compute '" << name << "' compile failed (len="
                              << logLen << "): " << (logLen ? log.data() : "(empty)") << "\n";
                    glDeleteShader(cs); return 0;
                }
                GLuint pg = glCreateProgram();
                glAttachShader(pg, cs);
                glLinkProgram(pg);
                glGetProgramiv(pg, GL_LINK_STATUS, &ok);
                if (ok == GL_FALSE) {
                    std::cerr << "[RHI-GL] SSAO compute '" << name << "' link failed\n";
                    glDeleteProgram(pg); pg = 0;
                }
                glDeleteShader(cs);
                return pg;
            };
            m_ssaoProgram = link("ssao", ssaoCsSrc);
            m_ssaoBlurProgram = link("ssaoBlur", ssaoBlurCsSrc);
        }
        // AO ping-pong targets resize with the scene.
        if (m_ssaoAoRaw) glDeleteTextures(1, &m_ssaoAoRaw);
        if (m_ssaoAoBlur) glDeleteTextures(1, &m_ssaoAoBlur);
        glGenTextures(1, &m_ssaoAoRaw);
        glBindTexture(GL_TEXTURE_2D, m_ssaoAoRaw);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenTextures(1, &m_ssaoAoBlur);
        glBindTexture(GL_TEXTURE_2D, m_ssaoAoBlur);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // B5 Bloom (GL twin of rhi/shaders/bloom_extract|blur|composite.comp).
    // Creates the 3 compute programs + 3 rgba16f ping-pong/final images + the
    // BloomUBO once; resizes are a full teardown+rebuild (cheap, bloom runs at
    // full scene resolution only when scene.bloom is true).
    void ensureBloomResources(int w, int h) {
        auto link = [&](const char* name, const char* src) -> GLuint {
            GLuint cs = glCreateShader(GL_COMPUTE_SHADER);
            glShaderSource(cs, 1, &src, nullptr);
            glCompileShader(cs);
            GLint ok = 0, logLen = 0;
            glGetShaderiv(cs, GL_COMPILE_STATUS, &ok);
            if (ok == GL_FALSE) {
                glGetShaderiv(cs, GL_INFO_LOG_LENGTH, &logLen);
                std::vector<char> log(logLen);
                glGetShaderInfoLog(cs, logLen, nullptr, log.data());
                std::cerr << "[RHI-GL] bloom compute '" << name << "' compile failed (len="
                          << logLen << "): " << (logLen ? log.data() : "(empty)") << "\n";
                glDeleteShader(cs); return 0;
            }
            GLuint pg = glCreateProgram();
            glAttachShader(pg, cs);
            glLinkProgram(pg);
            glGetProgramiv(pg, GL_LINK_STATUS, &ok);
            if (ok == GL_FALSE) {
                glGetProgramInfoLog(pg, 1024, nullptr, nullptr);
                std::cerr << "[RHI-GL] bloom compute '" << name << "' link failed\n";
                glDeleteProgram(pg); pg = 0;
            }
            glDeleteShader(cs);
            return pg;
        };
        if (!m_bloomCreated) {
            static const char* bloomExtractCsSrc =
                "#version 430\n"
                "layout(local_size_x=16, local_size_y=16) in;\n"
                "layout(binding=0) uniform sampler2D uSceneHdr;\n"
                "layout(std140, binding=1) uniform BloomUBO { float uThreshold; float uStrength; };\n"
                "layout(binding=2, rgba16f) uniform writeonly image2D outExtract;\n"
                "const vec3 kLum = vec3(0.2126, 0.7152, 0.0722);\n"
                "void main(){ ivec2 p=ivec2(gl_GlobalInvocationID.xy); ivec2 sz=imageSize(outExtract);\n"
                "  if(p.x>=sz.x||p.y>=sz.y) return;\n"
                "  vec2 uv=(vec2(p)+0.5)/vec2(sz); vec3 c=texture(uSceneHdr,uv).rgb;\n"
                "  float l=dot(c,kLum); float gain=max(0.0,l-uThreshold);\n"
                "  imageStore(outExtract,p,vec4(c*(gain/max(l,1e-4))*uStrength,1.0)); }\n";
            static const char* bloomBlurCsSrc =
                "#version 430\n"
                "layout(local_size_x=16, local_size_y=16) in;\n"
                "layout(binding=0) uniform sampler2D uExtract;\n"
                "layout(std140, binding=1) uniform BloomBlurUBO { vec2 uDirection; };\n"
                "layout(binding=2, rgba16f) uniform writeonly image2D outBlur;\n"
                "const float w[3]=float[](0.380517,0.241287,0.060652);\n"
                "void main(){ ivec2 p=ivec2(gl_GlobalInvocationID.xy); ivec2 sz=imageSize(outBlur);\n"
                "  if(p.x>=sz.x||p.y>=sz.y) return;\n"
                "  vec2 uv=(vec2(p)+0.5)/vec2(sz); vec3 s=texture(uExtract,uv).rgb*w[0];\n"
                "  s+=texture(uExtract,uv+uDirection*vec2( 1.0)).rgb*w[1];\n"
                "  s+=texture(uExtract,uv+uDirection*vec2(-1.0)).rgb*w[1];\n"
                "  s+=texture(uExtract,uv+uDirection*vec2( 2.0)).rgb*w[2];\n"
                "  s+=texture(uExtract,uv+uDirection*vec2(-2.0)).rgb*w[2];\n"
                "  imageStore(outBlur,p,vec4(s,1.0)); }\n";
            static const char* bloomCompositeCsSrc =
                "#version 430\n"
                "layout(local_size_x=16, local_size_y=16) in;\n"
                "layout(binding=0) uniform sampler2D uSceneHdr;\n"
                "layout(binding=1) uniform sampler2D uBloom;\n"
                "layout(binding=2, rgba16f) uniform writeonly image2D outFinal;\n"
                "void main(){ ivec2 p=ivec2(gl_GlobalInvocationID.xy); ivec2 sz=imageSize(outFinal);\n"
                "  if(p.x>=sz.x||p.y>=sz.y) return;\n"
                "  vec2 uv=(vec2(p)+0.5)/vec2(sz);\n"
                "  imageStore(outFinal,p,vec4(texture(uSceneHdr,uv).rgb+texture(uBloom,uv).rgb,1.0)); }\n";
            m_bloomExtractProgram   = link("bloomExtract",   bloomExtractCsSrc);
            m_bloomBlurProgram      = link("bloomBlur",      bloomBlurCsSrc);
            m_bloomCompositeProgram = link("bloomComposite", bloomCompositeCsSrc);
            glGenBuffers(1, &m_bloomUBO);
            glBindBuffer(GL_UNIFORM_BUFFER, m_bloomUBO);
            glBufferData(GL_UNIFORM_BUFFER, 8, nullptr, GL_DYNAMIC_DRAW);  // 2 floats (std140)
            glBindBuffer(GL_UNIFORM_BUFFER, 0);
            m_bloomCreated = true;
        }
        if (m_bloomRaw)   glDeleteTextures(1, &m_bloomRaw);
        if (m_bloomBlur)  glDeleteTextures(1, &m_bloomBlur);
        if (m_bloomFinal) glDeleteTextures(1, &m_bloomFinal);
        auto makeRgba16f = [&](GLuint& out) {
            glGenTextures(1, &out);
            glBindTexture(GL_TEXTURE_2D, out);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        };
        makeRgba16f(m_bloomRaw);
        makeRgba16f(m_bloomBlur);
        makeRgba16f(m_bloomFinal);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // B5 Bloom: extract brights -> gauss blur (H+V ping-pong) -> composite into
    // the HDR scene. Reads the current scene color (m_sceneColorTex) and writes
    // the composited result to m_bloomFinal (the host read-backs that instead of
    // m_sceneColorTex when scene.bloom is true). NO-OP if no bloom programs.
    void dispatchBloom(int w, int h) {
        if (!m_bloomExtractProgram || !m_bloomBlurProgram || !m_bloomCompositeProgram) return;
        const float bloomThresh = 0.0f, bloomStrength = 1.0f;   // brightpass everything; blur provides the falloff
        struct BloomUBO { float a, b; };                         // std140: 2 floats, 8 bytes
        auto up = [&](float a, float b) {
            BloomUBO u{a, b};
            glBindBuffer(GL_UNIFORM_BUFFER, m_bloomUBO);
            glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(u), &u);
            glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_bloomUBO);   // binding 1 (matches both bloom UBOs)
        };
        const GLuint gx = (w + 15) / 16, gy = (h + 15) / 16;

        // 1) brightpass: scene color -> m_bloomRaw
        glUseProgram(m_bloomExtractProgram);
        glBindTextureUnit(0, m_sceneColorTex);
        up(bloomThresh, bloomStrength);
        glBindImageTexture(2, m_bloomRaw, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        glBindImageTexture(2, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);

        // 2) horizontal blur: m_bloomRaw -> m_bloomBlur
        glUseProgram(m_bloomBlurProgram);
        glBindTextureUnit(0, m_bloomRaw);
        up(1.0f, 0.0f);   // uDirection = (1,0) horizontal
        glBindImageTexture(2, m_bloomBlur, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        glBindImageTexture(2, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);

        // 3) vertical blur: m_bloomBlur -> m_bloomRaw (ping-pong back)
        glUseProgram(m_bloomBlurProgram);
        glBindTextureUnit(0, m_bloomBlur);
        up(0.0f, 1.0f);   // uDirection = (0,1) vertical
        glBindImageTexture(2, m_bloomRaw, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        glBindImageTexture(2, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);

        // 4) additive composite: scene color + blurred bloom -> m_bloomFinal
        glUseProgram(m_bloomCompositeProgram);
        glBindTextureUnit(0, m_sceneColorTex);
        glBindTextureUnit(1, m_bloomRaw);
        glBindImageTexture(2, m_bloomFinal, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        glBindImageTexture(2, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        glUseProgram(0);
    }

    void cleanupBloomResources() {
        if (m_bloomCreated) {
            if (m_bloomExtractProgram)   { glDeleteProgram(m_bloomExtractProgram); m_bloomExtractProgram = 0; }
            if (m_bloomBlurProgram)      { glDeleteProgram(m_bloomBlurProgram); m_bloomBlurProgram = 0; }
            if (m_bloomCompositeProgram) { glDeleteProgram(m_bloomCompositeProgram); m_bloomCompositeProgram = 0; }
            if (m_bloomUBO)  { glDeleteBuffers(1, &m_bloomUBO); m_bloomUBO = 0; }
            m_bloomCreated = false;
        }
        if (m_bloomRaw)   { glDeleteTextures(1, &m_bloomRaw); m_bloomRaw = 0; }
        if (m_bloomBlur)  { glDeleteTextures(1, &m_bloomBlur); m_bloomBlur = 0; }
        if (m_bloomFinal) { glDeleteTextures(1, &m_bloomFinal); m_bloomFinal = 0; }
    }

    // B1-forward CSM (GL): upload the ShadowUBO (lightSpaceMatrices[3] +
    // cascadeSplits + sunDirection) into binding 2. Mirrors Vulkan's
    // uploadShadowUBO; the GL forward shaders read lightSpaceMatrices[0]
    // (cascade 0), matching the Vulkan forward cascade-0 sampling. Skipped
    // when shadows=false (so the 60/60 parity tests are byte-identical).
    void uploadShadowUBO(const OffscreenScene& scene) {
        struct GlShadowUBO {
            float lightSpaceMatrices[3][16];
            float cascadeSplits[4];
            float sunDirection[4];
        };
        GlShadowUBO u{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 16; ++j)
                u.lightSpaceMatrices[i][j] = scene.lightSpaceMatrices[i][j];
        u.cascadeSplits[0] = scene.cascadeSplits[0];
        u.cascadeSplits[1] = scene.cascadeSplits[1];
        u.cascadeSplits[2] = scene.cascadeSplits[2];
        u.sunDirection[0] = scene.sunDirection[0];
        u.sunDirection[1] = scene.sunDirection[1];
        u.sunDirection[2] = scene.sunDirection[2];
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_glShadowUbo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(u), &u);
    }

    // B1-forward CSM (GL): render the scene into the 2048x2048 depth atlas
    // from lightSpaceMatrices[0] (cascade 0). Saves/restores the FBO so it
    // can be called inside drawMeshes (forward only; gbufferPass skips).
    // Uses m_meshVAO (aPos@0, aModel@4); skinned meshes cast bind-pose
    // shadows (shadow vert ignores bone attribs), matching Vulkan.
    void drawShadowAtlas(const OffscreenScene& scene) {
        if (!m_glShadowDepthTex || !m_shadowDepthProgram) return;
        static constexpr int kShadowSize = 2048;
        GLint prevFbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_glShadowFBO);
        glViewport(0, 0, kShadowSize, kShadowSize);
        glClearDepth(1.0);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDisable(GL_CULL_FACE);          // full coverage in the depth map
        glUseProgram(m_shadowDepthProgram);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_sceneUBO);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_glShadowUbo);
        glBindVertexArray(m_meshVAO);
        // m_meshVAO may enable bone attribs (loc12/13) for the skinned path;
        // bind m_boneVBO so they don't dangle (shadow vert ignores them).
        glBindBuffer(GL_ARRAY_BUFFER, m_boneVBO);
        if (m_boneVBO) glBufferData(GL_ARRAY_BUFFER, 1, nullptr, GL_DYNAMIC_DRAW);
        for (const auto& mesh : scene.meshes) {
            if (mesh.vertices.empty() || mesh.indices.empty() || mesh.instances.empty()) continue;
            if (!mesh.clusters.empty()) continue;  // Nanite: not shadowed
            glBindBuffer(GL_ARRAY_BUFFER, m_meshVBO);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(float)),
                         nullptr, GL_DYNAMIC_DRAW);
            glBufferSubData(GL_ARRAY_BUFFER, 0,
                            static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(float)),
                            mesh.vertices.data());
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_meshEBO);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(uint32_t)),
                         nullptr, GL_DYNAMIC_DRAW);
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0,
                            static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(uint32_t)),
                            mesh.indices.data());
            glBindBuffer(GL_ARRAY_BUFFER, m_meshInstanceVBO);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(mesh.instances.size() * sizeof(OffscreenInstance)),
                         nullptr, GL_DYNAMIC_DRAW);
            glBufferSubData(GL_ARRAY_BUFFER, 0,
                            static_cast<GLsizeiptr>(mesh.instances.size() * sizeof(OffscreenInstance)),
                            mesh.instances.data());
            glDrawElementsInstanced(GL_TRIANGLES,
                                    static_cast<GLsizei>(mesh.indices.size()),
                                    GL_UNSIGNED_INT, nullptr,
                                    static_cast<GLsizei>(mesh.instances.size()));
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindVertexArray(0);
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    }

    // 1x1 white texture shared by untextured meshes (sampling white leaves
    // the lambert/fog math identical to textured meshes - parity by
    // construction).
    void ensureWhiteTexture() {
        if (m_whiteTex) return;
        glGenTextures(1, &m_whiteTex);
        glBindTexture(GL_TEXTURE_2D, m_whiteTex);
        const unsigned char px[4] = {255, 255, 255, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    // Cached per-mesh albedo texture (RGBA8, row 0 = TOP, same bytes the
    // Vulkan backend uploads). Keyed on the mesh pointer + dimensions: a
    // CPU-skinned character bumps version every frame but its albedo never
    // changes, so the texture uploads ONCE (matching the Vulkan texture
    // cache, which is explicitly NOT keyed on mesh.version).
    GLuint meshTexture(const OffscreenMesh& mesh) {
        ensureWhiteTexture();
        auto it = m_meshTexCache.find(&mesh);
        if (it != m_meshTexCache.end() && it->second.w == mesh.textureWidth &&
            it->second.h == mesh.textureHeight) {
            return it->second.tex ? it->second.tex : m_whiteTex;
        }
        if (it != m_meshTexCache.end() && it->second.tex) glDeleteTextures(1, &it->second.tex);
        GLuint tex = 0;
        if (mesh.textureWidth > 0 && mesh.textureHeight > 0 && !mesh.texturePixels.empty()) {
            // GL and Vulkan sample the SAME texel at the same UV: row 0 of
            // texturePixels lands at v=0 on both backends (GL stores the first
            // uploaded row at v=0; the Vulkan image copy puts buffer row 0 in
            // image row 0, which v=0 samples). No flip - the parity tests
            // assert identical pixels for the same bytes.
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mesh.textureWidth, mesh.textureHeight,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, mesh.texturePixels.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            // CLAMP_TO_EDGE matches the Vulkan sampler exactly: at uv=0/1 a
            // REPEAT wrap would blend the edge texel with the opposite edge's
            // texel under linear filtering, shifting the quad's 1px border
            // and breaking pixel parity on the same bytes.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        m_meshTexCache[&mesh] = {tex, mesh.textureWidth, mesh.textureHeight};
        return tex ? tex : m_whiteTex;
    }

    // Draw the scene's GENERIC meshes (interleaved pos/normal/uv + indices,
    // drawn instanced) after the cube path. Uploads each mesh's vertex/index/
    // instance data per draw - the same immediate-mode pattern as the cube
    // path; the world-object renderer will cache static buffers later. Each
    // mesh binds its albedo texture (white fallback) on unit 0.
    void drawMeshes(const OffscreenScene& scene, bool gbufferPass = false) {
        if (scene.meshes.empty()) return;
        // PBR path: Cook-Torrance + ACES + gamma, bit-identical math to the
        // Vulkan pbr_mesh.* shaders (parity). Falls back to the lambert mesh
        // program when PBR is disabled or not compiled.
        // Skinnable meshes use pbrSkinnedVsSrc (GL twin of pbr_skinned.vert):
        // a BoneUBO (mat4 bones[]) + per-vertex boneIdx/Weight attrs drive a
        // 4-bone LBS, reusing the SAME pbrFsSrc fragment shader so GL skinned
        // pixels match Vulkan bit-for-bit -> scene parity for the play char.
        // In the deferred geometry pass (gbufferPass) the geometry is rasterized
        // into the G-Buffer via m_gbufferProgram (pbrVsSrc + gbuffer.fs); skinned
        // meshes are drawn unskinned through that path (skinned+deferred is a
        // documented follow-on; the parity tests drive non-skinned deferred).
        const bool usePbr = scene.pbrEnabled;
        if (!usePbr && (!m_meshProgram && !m_pbrMeshProgram)) return;
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_sceneUBO);
        glBindVertexArray(m_meshVAO);
        glActiveTexture(GL_TEXTURE0);
        // B1-forward CSM (GL): render the light's depth atlas, then bind the
        // shadow UBO (binding 2) + depth texture (unit 1) so the shadow-PBR
        // programs can sample it. GL<->Vulkan forward-shadow parity. OFF when
        // shadows=false (tests) -> original prog path is unchanged.
        const bool forwardShadows = !gbufferPass && scene.shadows &&
                                    m_shadowPbrProgram && m_glShadowDepthTex;
        if (forwardShadows) {
            uploadShadowUBO(scene);
            drawShadowAtlas(scene);
            glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_glShadowUbo);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, m_glShadowDepthTex);
            glActiveTexture(GL_TEXTURE0);
        }
        for (const auto& mesh : scene.meshes) {
            if (mesh.vertices.empty() || mesh.indices.empty() || mesh.instances.empty()) continue;
            const bool skinned = !gbufferPass && mesh.skinned && m_skinnedPbrProgram &&
                                 !mesh.boneIndices.empty() && !mesh.jointPalette.empty();
            GLuint prog = gbufferPass ? m_gbufferProgram
                        : (skinned ? (forwardShadows ? m_shadowSkinnedPbrProgram
                                                     : m_skinnedPbrProgram)
                                   : (usePbr ? (forwardShadows ? m_shadowPbrProgram
                                                               : m_pbrMeshProgram)
                                             : m_meshProgram));
            if (!prog) continue;
            glUseProgram(prog);
            // Wind sway: per-mesh amplitude carried on OffscreenMesh.windSway
            // (captured from ModelComponent.windSway). Bound only when >0 (the
            // common static-prop case is skipped); programs lacking the uniform
            // resolve glGetUniformLocation to -1 -> GL no-op, so skinned / non-PBR
            // progs and all parity tests (windSway == 0) are pixel-identical.
            if (mesh.windSway > 0.0f) {
                glUniform1f(gloc(prog, "uWindStrength"), mesh.windSway);
                glUniform1f(gloc(prog, "uTime"), static_cast<float>(m_frame));
            }
            if (forwardShadows) {
                glUniform1i(gloc(prog, "uShadowMap"), 1);
            }
            if (skinned) {
                // Joint palette -> BoneUBO (binding 1). jointPalette holds
                // jointCount column-major mat4s; upload this mesh's local slice.
                const int bc = mesh.jointCount > 0 ? mesh.jointCount : 0;
                if (bc > 0) {
                    glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_boneUBO);
                    glBufferSubData(GL_UNIFORM_BUFFER, 0,
                                    (size_t)bc * 16 * sizeof(float),
                                    &mesh.jointPalette[0]);
                }
                // Per-vertex boneIdx/Weight -> shared bone VBO (loc12/13 on
                // m_meshVAO). boneIndices = 4 uints/vert, boneWeights = 4/vert;
                // pack as 32 bytes/vert {uvec4,vec4}.
                const size_t nv = mesh.boneIndices.size() / 4;
                const size_t nwi = mesh.boneWeights.size() / 4;
                const size_t nbc = nv < nwi ? nv : nwi;
                std::vector<uint32_t> boneData(nbc * 8);
                for (size_t i = 0; i < nbc; ++i) {
                    std::memcpy(&boneData[i * 8],      &mesh.boneIndices[i * 4], 16);
                    std::memcpy(&boneData[i * 8 + 4],  &mesh.boneWeights[i * 4], 16);
                }
                glBindBuffer(GL_ARRAY_BUFFER, m_boneVBO);
                glBufferData(GL_ARRAY_BUFFER, nbc * sizeof(uint32_t) * 8,
                             nbc ? boneData.data() : nullptr, GL_DYNAMIC_DRAW);
                // Rebind the active ARRAY buffer to m_meshVBO for the vert
                // upload below (VAO keeps its own per-attribute buffer refs).
            }
            glBindTexture(GL_TEXTURE_2D, meshTexture(mesh));
            glBindBuffer(GL_ARRAY_BUFFER, m_meshVBO);
            // Orphan first: realloc a fresh storage so the GPU never reads verts
            // we are mid-overwriting (no glBufferSubData into a live buffer).
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(float)),
                         nullptr, GL_DYNAMIC_DRAW);
            glBufferSubData(GL_ARRAY_BUFFER, 0,
                            static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(float)),
                            mesh.vertices.data());
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_meshEBO);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(uint32_t)),
                         nullptr, GL_DYNAMIC_DRAW);
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0,
                            static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(uint32_t)),
                            mesh.indices.data());
            glBindBuffer(GL_ARRAY_BUFFER, m_meshInstanceVBO);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(mesh.instances.size() * sizeof(OffscreenInstance)),
                         nullptr, GL_DYNAMIC_DRAW);
            glBufferSubData(GL_ARRAY_BUFFER, 0,
                            static_cast<GLsizeiptr>(mesh.instances.size() * sizeof(OffscreenInstance)),
                             mesh.instances.data());
            glDrawElementsInstanced(GL_TRIANGLES,
                                    static_cast<GLsizei>(mesh.indices.size()),
                                    GL_UNSIGNED_INT, nullptr,
                                    static_cast<GLsizei>(mesh.instances.size()));
        }
        // Parity fix (Task 3): reset ALL 16 texture units (shadow atlas on unit 1,
        // mesh textures on unit 0) so stale descriptors don't leak into the
        // downstream ImGui/forward passes. Vulkan's descriptor bindings are
        // command-buffer scoped; GL texture units are global state.
        for (int i = 0; i < 16; ++i) {
            glBindTextureUnit(i, 0);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        glBindVertexArray(0);
        glUseProgram(0);
    }

private:
    // Offscreen 3D scene state (camera UBO + depth + instancing). Mirrors the
    // Vulkan scene path so both backends render the same scene identically.
    bool ensureSceneResources(int w, int h) {
        if (m_sceneProgram && m_sceneW == w && m_sceneH == h) return true;
        destroySceneResources();
        m_sceneW = w;
        m_sceneH = h;

        // Instanced cube shader. Vertex layout matches the Vulkan SPIR-V:
        // binding 0 = positions, binding 1 = { mat4 model, vec4 color } with
        // a per-instance divisor on the model+color attributes.
        static const char* vsSrc =
            "#version 330 core\n"
            "uniform vec2 uJitter;\n"
            "layout(location=0) in vec3 aPos;\n"
            "layout(location=1) in vec4 aColor;\n"
            "layout(location=2) in mat4 aModel;\n"
            "layout(std140) uniform CameraUBO { mat4 viewProj; };\n"
            "out vec4 vColor;\n"
            "void main(){ vColor = aColor; gl_Position = viewProj * aModel * vec4(aPos, 1.0);"
            " gl_Position.xy += uJitter * gl_Position.w; }\n";
        static const char* fsSrc =
            "#version 330 core\n"
            "in vec4 vColor;\n"
            "out vec4 c;\n"
            "void main(){ c = vColor; }\n";

        m_sceneProgram = compileProgram(vsSrc, fsSrc);
        if (!m_sceneProgram) return false;
        const GLuint blockIdx = glGetUniformBlockIndex(m_sceneProgram, "CameraUBO");
        glUniformBlockBinding(m_sceneProgram, blockIdx, 0);

        // Cube positions (static).
        glGenVertexArrays(1, &m_sceneVAO);
        glBindVertexArray(m_sceneVAO);
        glGenBuffers(1, &m_sceneVBO);
        glBindBuffer(GL_ARRAY_BUFFER, m_sceneVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kCubeVerts), kCubeVerts, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);

        // Per-instance data: { mat4 model (bytes 0-63), vec4 color (bytes
        // 64-79), mat4 prevModel (bytes 80-143) }. Offsets/stride are BYTES:
        // the mat4 rows sit 16 bytes apart and each instance is 144 bytes.
        // prevModel (Phase 3) is uploaded (raw memcpy of the 144-byte struct)
        // but inert on GL in Phase 3A: there is no aPrevModel vertex attrib
        // yet, so the color math reads only model/color -> parity preserved.
        const GLsizei instStride = static_cast<GLsizei>(sizeof(OffscreenInstance));
        glGenBuffers(1, &m_sceneInstanceVBO);
        glBindBuffer(GL_ARRAY_BUFFER, m_sceneInstanceVBO);
        glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, instStride,
                              (void*)(16 * sizeof(float)));
        glVertexAttribDivisor(1, 1);
        for (int i = 0; i < 4; ++i) {
            glEnableVertexAttribArray(2 + i);
            glVertexAttribPointer(2 + i, 4, GL_FLOAT, GL_FALSE, instStride,
                                  (void*)(i * 4 * sizeof(float)));
            glVertexAttribDivisor(2 + i, 1);
        }
        glBindVertexArray(0);

        // Camera UBO.
        glGenBuffers(1, &m_sceneUBO);
        glBindBuffer(GL_UNIFORM_BUFFER, m_sceneUBO);
        glBufferData(GL_UNIFORM_BUFFER, 64, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

        // ---- Generic mesh path (interleaved pos/normal/uv + indices). The
        // vertex layout MUST match the Vulkan SPIR-V (mesh.vert/frag): binding
        // 0 = { vec3 pos, vec3 normal, vec2 uv } (32-byte stride), binding 1 =
        // per-instance { mat4 model, vec4 color, mat4 prevModel } (144-byte
        // stride; prevModel@80 inert on GL in Phase 3A -> parity preserved).
        // lighting math is identical to mesh.frag so parity holds.
        // The vertex shader passes the world position + UV through and the
        // fragment shader runs the IDENTICAL math to the Vulkan SPIR-V
        // (mesh.vert/.frag): base albedo = texture * vColor, flat lambert with
        // the same light direction, exponential-squared distance fog toward
        // cam.fogColor. fogDensity = 0 keeps the parity tests pixel-identical.
        static const char* meshVsSrc =
            "#version 330 core\n"
            "layout(location=0) in vec3 aPos;\n"
            "layout(location=1) in vec3 aNormal;\n"
            "layout(location=2) in vec2 aUV;\n"
            "layout(location=3) in vec4 aColor;\n"
            "layout(location=4) in mat4 aModel;\n"
            "layout(location=8) in mat4 aPrevModel;\n"
            "uniform vec2 uJitter;\n"
            "layout(std140) uniform CameraUBO { mat4 viewProj; vec3 camPos;"
            " float fogDensity; vec3 fogColor; float pad; };\n"
            "out vec4 vColor;\n"
            "out vec3 vNormal;\n"
            "out vec2 vUV;\n"
            "out vec3 vWorldPos;\n"
            "out vec2 vCurrNdc;\n"
            "out vec4 vPrevPos;\n"
            "void main(){ vColor = aColor; vNormal = mat3(aModel) * aNormal;"
            " vUV = aUV; vWorldPos = vec3(aModel * vec4(aPos, 1.0));"
            " gl_Position = viewProj * aModel * vec4(aPos, 1.0);"
            " gl_Position.xy += uJitter * gl_Position.w;"
            " vPrevPos = viewProj * aPrevModel * vec4(aPos, 1.0);"
            " vCurrNdc = gl_Position.xy / gl_Position.w; }\n";
        static const char* meshFsSrc =
            "#version 330 core\n"
            "in vec4 vColor;\n"
            "in vec3 vNormal;\n"
            "in vec2 vUV;\n"
            "in vec3 vWorldPos;\n"
            "in vec2 vCurrNdc;\n"
            "in vec4 vPrevPos;\n"
            "layout(std140) uniform CameraUBO { mat4 viewProj; vec3 camPos;"
            " float fogDensity; vec3 fogColor; float pad; };\n"
            "uniform sampler2D uTex;\n"
            "out vec4 c;\n"
            "layout(location = 1) out vec2 outVelocity;\n"
            "const vec3 kLightDir = normalize(vec3(0.4, 0.8, 0.3));\n"
            "void main(){\n"
            "  vec3 n = normalize(vNormal);\n"
            "  float diff = max(dot(n, kLightDir), 0.0);\n"
            "  vec3 base = texture(uTex, vUV).rgb;\n"
            "  vec3 shaded = base * vColor.rgb * (0.5 + 0.5 * diff);\n"
            "  float dist = distance(vWorldPos, camPos);\n"
            "  float fogF = clamp(1.0 - exp(-fogDensity * fogDensity * dist * dist), 0.0, 1.0);\n"
            "  c = vec4(mix(shaded, fogColor, fogF), vColor.a);\n"
            "  outVelocity = vCurrNdc - (vPrevPos.xy / vPrevPos.w);\n"
            "}\n";
        m_meshProgram = compileProgram(meshVsSrc, meshFsSrc);
        if (!m_meshProgram) return false;
        const GLuint meshBlockIdx = glGetUniformBlockIndex(m_meshProgram, "CameraUBO");
        glUniformBlockBinding(m_meshProgram, meshBlockIdx, 0);

        // ---- PBR mesh path (Cook-Torrance + ACES + gamma), ported VERBATIM
        // from rhi/shaders/pbr_mesh.vert/.frag (the Vulkan GLSL 4.6 shaders).
        // The PBR vertex shader uses the CORRECT normal transform
        // mat3(transpose(inverse(model))) to match the Vulkan pbr_mesh.vert
        // (the lambert path above uses the cheaper mat3(model), which is only
        // safe for uniform scale). SAME vertex attribute layout as the mesh
        // path, so m_meshVAO is reused. The fragment shader is the identical
        // BRDF math (kLightDir=(0.5,1,0.3), kLightColor, GGX, Smith,
        // Fresnel-Schlick, ACESFilm, pow(1/2.2), exponential-squared fog) —
        // only the texture fetch differs (GL bound sampler `uTex` on unit 0
        // instead of the Vulkan bindless globalTextures[textureId]). Same
        // CameraUBO (camPos drives V), so GL and Vulkan PBR pixels match.
        static const char* pbrVsSrc =
            "#version 330 core\n"
            "layout(location=0) in vec3 aPos;\n"
            "layout(location=1) in vec3 aNormal;\n"
            "layout(location=2) in vec2 aUV;\n"
            "layout(location=3) in vec4 aColor;\n"
            "layout(location=4) in mat4 aModel;\n"
            "layout(location=8) in mat4 aPrevModel;\n"
            "uniform vec2 uJitter;\n"
            "layout(std140) uniform CameraUBO { mat4 viewProj; vec3 camPos;"
            " float fogDensity; vec3 fogColor; float pad; };\n"
            "out vec4 vColor;\n"
            "out vec3 vNormal;\n"
            "out vec2 vUV;\n"
            "out vec3 vWorldPos;\n"
            "out vec2 vCurrNdc;\n"
            "out vec4 vPrevPos;\n"
            "uniform float uWindStrength = 0.0;\n"
            "uniform float uTime = 0.0;\n"
            "uniform vec3 uWindDir = vec3(1.0, 0.0, 0.2);\n"
            "void main(){ vColor = aColor; vUV = aUV;"
            " vWorldPos = vec3(aModel * vec4(aPos, 1.0));"
            " if (uWindStrength > 0.0) {"
            "   float w_h = max(aPos.y, 0.0);"
            "   float w_phase = uTime * 2.0 + vWorldPos.x * 0.12 + vWorldPos.z * 0.12;"
            "   float w_sway = sin(w_phase) * w_h * uWindStrength;"
            "   vWorldPos.x += uWindDir.x * w_sway;"
            "   vWorldPos.z += uWindDir.z * w_sway;"
            " }"
            " vNormal = mat3(transpose(inverse(aModel))) * aNormal;"
            " gl_Position = viewProj * vec4(vWorldPos, 1.0);"
            " gl_Position.xy += uJitter * gl_Position.w;"
            " vPrevPos = viewProj * aPrevModel * vec4(aPos, 1.0);"
            " vCurrNdc = gl_Position.xy / gl_Position.w; }\n";
        static const char* pbrFsSrc =
            "#version 330 core\n"
            "in vec4 vColor;\n"
            "in vec3 vNormal;\n"
            "in vec2 vUV;\n"
            "in vec3 vWorldPos;\n"
            "in vec2 vCurrNdc;\n"
            "in vec4 vPrevPos;\n"
            "layout(std140) uniform CameraUBO { mat4 viewProj; vec3 camPos;"
            " float fogDensity; vec3 fogColor; float pad; };\n"
            "uniform sampler2D uTex;\n"
            "out vec4 c;\n"
            "layout(location = 1) out vec2 outVelocity;\n"
            "const float PI = 3.14159265359;\n"
            "const vec3 kLightDir = normalize(vec3(0.5, 1.0, 0.3));\n"
            "const vec3 kLightColor = vec3(1.0, 0.98, 0.92);\n"
            "float DistributionGGX(vec3 N, vec3 H, float r){ float a=r*r;"
            " float a2=a*a; float d=max(dot(N,H),0.0); d=d*d;"
            " return a2/(PI*(d*(a2-1.0)+1.0)*(d*(a2-1.0)+1.0)); }\n"
            "float GeometrySchlickGGX(float NdotV, float r){ float k=(r+1.0);"
            " k=k*k/8.0; return NdotV/(NdotV*(1.0-k)+k); }\n"
            "float GeometrySmith(vec3 N, vec3 V, vec3 L, float r){"
            " return GeometrySchlickGGX(max(dot(N,V),0.0),r)*"
            " GeometrySchlickGGX(max(dot(N,L),0.0),r); }\n"
            "vec3 fresnelSchlick(float t, vec3 F0){ return F0 + (1.0-F0)*"
            " pow(clamp(1.0-t,0.0,1.0),5.0); }\n"
            "vec3 ACESFilm(vec3 x){ float a=2.51,b=0.03,c=2.43,d=0.59,e=0.14;"
            " return clamp((x*(a*x+b))/(x*(c*x+d)+e),0.0,1.0); }\n"
            "// GL twin of pbr_mesh.frag::CalculatePOM. Vulkan reads height from the\n"
            "// albedo texture's ALPHA channel via the bindless globalTextures[texId];\n"
            "// GL samples the per-primitive bound sampler `uTex` instead. The ray-\n"
            "// march arithmetic is bit-identical (same displacementScale/layer count/\n"
            "// derivatives), so GL and Vulkan POM pixels match. For alpha=1.0 (no\n"
            "// height) the loop never runs and weight==0 -> displacedUV==vUV, leaving\n"
            "// every existing non-height render unchanged.\n"
            "vec2 CalculatePOM(vec2 initialUV, vec3 viewDirTS, vec2 dx, vec2 dy) {\n"
            "  const float minLayers = 4.0, maxLayers = 16.0;\n"
            "  float numLayers = mix(maxLayers, minLayers, abs(dot(vec3(0.0,0.0,1.0), viewDirTS)));\n"
            "  float layerDepth = 1.0/numLayers; float currentLayerDepth = 0.0;\n"
            "  const float displacementScale = 0.04;\n"
            "  vec2 p = viewDirTS.xy / viewDirTS.z * displacementScale;\n"
            "  vec2 deltaUV = p / numLayers;\n"
            "  vec2 currentUV = initialUV;\n"
            "  float currentDepthMapValue = 1.0 - textureGrad(uTex, currentUV, dx, dy).a;\n"
            "  while(currentLayerDepth < currentDepthMapValue) {\n"
            "    currentUV -= deltaUV;\n"
            "    currentDepthMapValue = 1.0 - textureGrad(uTex, currentUV, dx, dy).a;\n"
            "    currentLayerDepth += layerDepth; }\n"
            "  vec2 prevUV = currentUV + deltaUV;\n"
            "  float nextDepth = currentDepthMapValue - currentLayerDepth;\n"
            "  float prevDepth = (1.0 - textureGrad(uTex, prevUV, dx, dy).a) - (currentLayerDepth - layerDepth);\n"
            "  float weight = nextDepth / (nextDepth - prevDepth);\n"
            "  return mix(currentUV, prevUV, weight); }\n"
            "void main(){\n"
            "  vec3 dX = dFdx(vWorldPos); vec3 dY = dFdy(vWorldPos);\n"
            "  vec2 dT = dFdx(vUV); vec2 dB = dFdy(vUV);\n"
            "  vec3 N_geom = normalize(vNormal);\n"
            "  vec3 T = normalize(dX * dB.t - dY * dT.t);\n"
            "  vec3 B = -normalize(cross(N_geom, T));\n"
            "  mat3 TBN = mat3(T, B, N_geom);\n"
            "  vec3 viewDirWorld = normalize(camPos - vWorldPos);\n"
            "  vec3 viewDirTS = normalize(transpose(TBN) * viewDirWorld);\n"
            "  vec2 displacedUV = CalculatePOM(vUV, viewDirTS, dT, dB);\n"
            "  vec3 albedo = texture(uTex, displacedUV).rgb * vColor.rgb;\n"
            "  vec3 N = N_geom;\n"
            "  vec3 V = viewDirWorld;\n"
            "  float lum = dot(albedo, vec3(0.2126, 0.7152, 0.0722));\n"
            "  float metallic = clamp(lum*0.3, 0.0, 0.8);\n"
            "  float roughness = clamp(0.4+(1.0-lum)*0.4, 0.04, 1.0);\n"
            "  vec3 F0 = mix(vec3(0.04), albedo, metallic);\n"
            "  vec3 L = kLightDir, H = normalize(V+L);\n"
            "  float D = DistributionGGX(N,H,roughness);\n"
            "  float G = GeometrySmith(N,V,L,roughness);\n"
            "  vec3 F = fresnelSchlick(max(dot(H,V),0.0),F0);\n"
            "  vec3 spec = (D*G*F)/(4.0*max(dot(N,V),0.0)*max(dot(N,L),0.0)+0.0001);\n"
            "  vec3 kD = (vec3(1.0)-F)*(1.0-metallic);\n"
            "  vec3 Lo = (kD*albedo/PI + spec)*kLightColor*max(dot(N,L),0.0);\n"
            "  vec3 skyCol = vec3(0.4,0.5,0.7), groundCol = vec3(0.3,0.2,0.1);\n"
            "  vec3 ambient = fresnelSchlick(max(dot(N,V),0.0),F0)*"
            " mix(groundCol,skyCol,N.y*0.5+0.5)*0.3;\n"
            "  vec3 color = ambient + Lo;\n"
            "  float dist = distance(vWorldPos, camPos);\n"
            "  float fogF = clamp(1.0-exp(-fogDensity*fogDensity*dist*dist),0.0,1.0);\n"
            "  color = mix(color, fogColor, fogF);\n"
            "  color = ACESFilm(color);\n"
            "  color = pow(color, vec3(1.0/2.2));\n"
            "  c = vec4(color, vColor.a);\n"
            "  outVelocity = vCurrNdc - (vPrevPos.xy / vPrevPos.w);\n"
            "}\n";
        m_pbrMeshProgram = compileProgram(pbrVsSrc, pbrFsSrc);
        if (!m_pbrMeshProgram) return false;
        const GLuint pbrBlockIdx = glGetUniformBlockIndex(m_pbrMeshProgram, "CameraUBO");
        glUniformBlockBinding(m_pbrMeshProgram, pbrBlockIdx, 0);

        // ---- B1-forward CSM parity (GL twin of Vulkan pbr_mesh_shadow.frag).
        // pbrFsShadowSrc is pbrFsSrc byte-for-byte (same POM + Cook-Torraton +
        // ACES + gamma) PLUS a cascade-0 shadow factor (multiply Lo), sampled
        // from a 2048x2048 depth texture via uShadowMap (GL sampler2DShadow).
        // vWorldPos comes from pbrVsSrc (aModel* aPos), so no depth-reconstruct.
        // ShadowUBO (binding 1) mirrors Vulkan's ShadowUBO std140 layout; only
        // lightSpaceMatrices[0] is sampled (cascade 0), matching the Vulkan
        // forward path. OFF by default (scene.shadows=false) -> pbrFsSrc is used
        // instead, so the 60/60 GL<->Vulkan parity tests are pixel-identical.
        static const char* pbrFsShadowSrc =
            "#version 330 core\n"
            "in vec4 vColor;\n"
            "in vec3 vNormal;\n"
            "in vec2 vUV;\n"
            "in vec3 vWorldPos;\n"
            "in vec2 vCurrNdc;\n"
            "in vec4 vPrevPos;\n"
            "layout(std140, binding=0) uniform CameraUBO { mat4 viewProj; vec3 camPos;"
            " float fogDensity; vec3 fogColor; float pad; };\n"
            "layout(std140, binding=2) uniform ShadowUBO { mat4 lightSpaceMatrices[3];"
            " vec4 cascadeSplits; vec4 sunDirection; };\n"
            "uniform sampler2D uTex;\n"
            "uniform sampler2DShadow uShadowMap;\n"
            "out vec4 c;\n"
            "layout(location = 1) out vec2 outVelocity;\n"
            "const float PI = 3.14159265359;\n"
            "const vec3 kLightDir = normalize(vec3(0.5, 1.0, 0.3));\n"
            "const vec3 kLightColor = vec3(1.0, 0.98, 0.92);\n"
            "float DistributionGGX(vec3 N, vec3 H, float r){ float a=r*r;"
            " float a2=a*a; float d=max(dot(N,H),0.0); d=d*d;"
            " return a2/(PI*(d*(a2-1.0)+1.0)*(d*(a2-1.0)+1.0)); }\n"
            "float GeometrySchlickGGX(float NdotV, float r){ float k=(r+1.0);"
            " k=k*k/8.0; return NdotV/(NdotV*(1.0-k)+k); }\n"
            "float GeometrySmith(vec3 N, vec3 V, vec3 L, float r){"
            " return GeometrySchlickGGX(max(dot(N,V),0.0),r)*"
            " GeometrySchlickGGX(max(dot(N,L),0.0),r); }\n"
            "vec3 fresnelSchlick(float t, vec3 F0){ return F0 + (1.0-F0)*"
            " pow(clamp(1.0-t,0.0,1.0),5.0); }\n"
            "vec3 ACESFilm(vec3 x){ float a=2.51,b=0.03,c=2.43,d=0.59,e=0.14;"
            " return clamp((x*(a*x+b))/(x*(c*x+d)+e),0.0,1.0); }\n"
            "vec2 CalculatePOM(vec2 initialUV, vec3 viewDirTS, vec2 dx, vec2 dy) {\n"
            "  const float minLayers = 4.0, maxLayers = 16.0;\n"
            "  float numLayers = mix(maxLayers, minLayers, abs(dot(vec3(0.0,0.0,1.0), viewDirTS)));\n"
            "  float layerDepth = 1.0/numLayers; float currentLayerDepth = 0.0;\n"
            "  const float displacementScale = 0.04;\n"
            "  vec2 p = viewDirTS.xy / viewDirTS.z * displacementScale;\n"
            "  vec2 deltaUV = p / numLayers;\n"
            "  vec2 currentUV = initialUV;\n"
            "  float currentDepthMapValue = 1.0 - textureGrad(uTex, currentUV, dx, dy).a;\n"
            "  while(currentLayerDepth < currentDepthMapValue) {\n"
            "    currentUV -= deltaUV;\n"
            "    currentDepthMapValue = 1.0 - textureGrad(uTex, currentUV, dx, dy).a;\n"
            "    currentLayerDepth += layerDepth; }\n"
            "  vec2 prevUV = currentUV + deltaUV;\n"
            "  float nextDepth = currentDepthMapValue - currentLayerDepth;\n"
            "  float prevDepth = (1.0 - textureGrad(uTex, prevUV, dx, dy).a) - (currentLayerDepth - layerDepth);\n"
            "  float weight = nextDepth / (nextDepth - prevDepth);\n"
            "  return mix(currentUV, prevUV, weight); }\n"
            "void main(){\n"
            "  vec3 dX = dFdx(vWorldPos); vec3 dY = dFdy(vWorldPos);\n"
            "  vec2 dT = dFdx(vUV); vec2 dB = dFdy(vUV);\n"
            "  vec3 N_geom = normalize(vNormal);\n"
            "  vec3 T = normalize(dX * dB.t - dY * dT.t);\n"
            "  vec3 B = -normalize(cross(N_geom, T));\n"
            "  mat3 TBN = mat3(T, B, N_geom);\n"
            "  vec3 viewDirWorld = normalize(camPos - vWorldPos);\n"
            "  vec3 viewDirTS = normalize(transpose(TBN) * viewDirWorld);\n"
            "  vec2 displacedUV = CalculatePOM(vUV, viewDirTS, dT, dB);\n"
            "  vec3 albedo = texture(uTex, displacedUV).rgb * vColor.rgb;\n"
            "  vec3 N = N_geom;\n"
            "  vec3 V = viewDirWorld;\n"
            "  float lum = dot(albedo, vec3(0.2126, 0.7152, 0.0722));\n"
            "  float metallic = clamp(lum*0.3, 0.0, 0.8);\n"
            "  float roughness = clamp(0.4+(1.0-lum)*0.4, 0.04, 1.0);\n"
            "  vec3 F0 = mix(vec3(0.04), albedo, metallic);\n"
            "  vec3 L = kLightDir, H = normalize(V+L);\n"
            "  float D = DistributionGGX(N,H,roughness);\n"
            "  float G = GeometrySmith(N,V,L,roughness);\n"
            "  vec3 F = fresnelSchlick(max(dot(H,V),0.0),F0);\n"
            "  vec3 spec = (D*G*F)/(4.0*max(dot(N,V),0.0)*max(dot(N,L),0.0)+0.0001);\n"
            "  vec3 kD = (vec3(1.0)-F)*(1.0-metallic);\n"
            "  vec3 Lo = (kD*albedo/PI + spec)*kLightColor*max(dot(N,L),0.0);\n"
            "  // B1-forward CSM (cascade 0) — depth-compare against the sun atlas.\n"
            "  vec4 sc = lightSpaceMatrices[0] * vec4(vWorldPos,1.0); sc/=sc.w;\n"
            "  float shadowFactor = texture(uShadowMap, vec3(sc.xy*0.5+0.5, sc.z*0.5+0.5));\n"
            "  Lo *= shadowFactor;\n"
            "  vec3 skyCol = vec3(0.4,0.5,0.7), groundCol = vec3(0.3,0.2,0.1);\n"
            "  vec3 ambient = fresnelSchlick(max(dot(N,V),0.0),F0)*"
            " mix(groundCol,skyCol,N.y*0.5+0.5)*0.3;\n"
            "  vec3 color = ambient + Lo;\n"
            "  float dist = distance(vWorldPos, camPos);\n"
            "  float fogF = clamp(1.0-exp(-fogDensity*fogDensity*dist*dist),0.0,1.0);\n"
            "  color = mix(color, fogColor, fogF);\n"
            "  color = ACESFilm(color);\n"
            "  color = pow(color, vec3(1.0/2.2));\n"
            "  c = vec4(color, vColor.a);\n"
            "  outVelocity = vCurrNdc - (vPrevPos.xy / vPrevPos.w);\n"
            "}\n";
        m_shadowPbrProgram = compileProgram(pbrVsSrc, pbrFsShadowSrc);
        if (m_shadowPbrProgram) {
            const GLuint sbIdx = glGetUniformBlockIndex(m_shadowPbrProgram, "ShadowUBO");
            if (sbIdx != GL_INVALID_INDEX) glUniformBlockBinding(m_shadowPbrProgram, sbIdx, 2);
        }

        // Shadow-map depth pass: render scene from the light's view into the
        // 2048x2048 depth atlas. aPos@0 + aModel@4 (same m_meshVAO layout as
        // pbrVsSrc; cubes reuse m_sceneVAO's aModel@2..5 but the GL parity path
        // drives offscreen scene meshes, so per-mesh VBO/EBO is bound).
        static const char* glShadowVsSrc =
            "#version 330 core\n"
            "layout(location=0) in vec3 aPos;\n"
            "layout(location=4) in mat4 aModel;\n"
            "layout(std140, binding=2) uniform ShadowUBO { mat4 lightSpaceMatrices[3];"
            " vec4 cascadeSplits; vec4 sunDirection; };\n"
            "void main(){ gl_Position = lightSpaceMatrices[0] * aModel * vec4(aPos,1.0); }\n";
        static const char* glShadowFsSrc =
            "#version 330 core\nvoid main(){}\n";
        m_shadowDepthProgram = compileProgram(glShadowVsSrc, glShadowFsSrc);

        // Phase-B B4 skybox — inline GLSL 3.30 twin of rhi/shaders/skybox.{vert,frag}
        static const char* skyboxVsSrc =
            "#version 330 core\n"
            "layout(location=0) in vec3 aPos;\n"
            "layout(std140) uniform CameraUBO { mat4 viewProj; vec3 camPos;"
            " float fogDensity; vec3 fogColor; float pad; };\n"
            "out vec3 vDir;\n"
            "void main(){ vec3 p = aPos * 2.0; vDir = p;"
            " gl_Position = viewProj * vec4(p, 1.0);"
            " gl_Position.z = gl_Position.w; }\n";
        static const char* skyboxFsSrc =
            "#version 330 core\n"
            "in vec3 vDir;\n"
            "uniform samplerCube u_sky;\n"
            "out vec4 fragColor;\n"
            "void main(){ fragColor = texture(u_sky, vDir); }\n";
        m_skyboxProgram = compileProgram(skyboxVsSrc, skyboxFsSrc);

        // ---- Skinned PBR vertex stage (GL twin of rhi/shaders/pbr_skinned.vert).
        // Fork of pbrVsSrc that adds a 4-bone LBS: per-vertex boneIdx (loc12,
        // uvec4) + boneWeight (loc13, vec4) and a BoneUBO (mat4 bones[],
        // binding 1) holding the joint palette. The SAME pbrFsSrc is reused, so
        // the Cook-Torrance/A CES/gamma BRDF is bit-identical to the unskinned
        // (and to Vulkan pbr_skinned) -> skinned pixels are GL<->Vulkan parity.
        // Mirrors pbr_skinned.vert's convention: localV = gl_VertexID (the GL
        // offscreen re-uploads a per-mesh vertex buffer starting at 0, so there
        // is no global firstVertex offset here); boneIdx are LOCAL indices into
        // the mesh's own palette slice (palette[boneOffset+idx] collapses to
        // palette[idx] when the slice is uploaded starting at UBO offset 0).
        static const char* pbrSkinnedVsSrc =
            "#version 330 core\n"
            "layout(location=0) in vec3 aPos;\n"
            "layout(location=1) in vec3 aNormal;\n"
            "layout(location=2) in vec2 aUV;\n"
            "layout(location=3) in vec4 aColor;\n"
            "layout(location=4) in mat4 aModel;\n"
            "layout(location=8) in mat4 aPrevModel;\n"
            "layout(location=12) in uvec4 aBoneIdx;\n"
            "layout(location=13) in vec4 aBoneWeight;\n"
            "uniform vec2 uJitter;\n"
            "layout(std140) uniform CameraUBO { mat4 viewProj; vec3 camPos;"
            " float fogDensity; vec3 fogColor; float pad; };\n"
            "layout(std140) uniform BoneUBO { mat4 bones[32]; };\n"
            "out vec4 vColor;\n"
            "out vec3 vNormal;\n"
            "out vec2 vUV;\n"
            "out vec3 vWorldPos;\n"
            "out vec2 vCurrNdc;\n"
            "out vec4 vPrevPos;\n"
            "void main(){ vColor = aColor; vUV = aUV;\n"
            "  mat4 skinM = mat4(0.0);\n"
            "  skinM += bones[int(aBoneIdx.x)] * aBoneWeight.x;\n"
            "  skinM += bones[int(aBoneIdx.y)] * aBoneWeight.y;\n"
            "  skinM += bones[int(aBoneIdx.z)] * aBoneWeight.z;\n"
            "  skinM += bones[int(aBoneIdx.w)] * aBoneWeight.w;\n"
            "  vec4 skinnedPos = skinM * vec4(aPos, 1.0);\n"
            "  vec3 skinnedN = mat3(skinM) * aNormal;\n"
            "  vWorldPos = vec3(aModel * skinnedPos);\n"
            "  vNormal = mat3(transpose(inverse(aModel))) * skinnedN;\n"
            "  gl_Position = viewProj * vec4(vWorldPos, 1.0);\n"
            "  gl_Position.xy += uJitter * gl_Position.w;\n"
            "  vPrevPos = viewProj * aPrevModel * skinnedPos;\n"
            "  vCurrNdc = gl_Position.xy / gl_Position.w; }\n";
        m_skinnedPbrProgram = compileProgram(pbrSkinnedVsSrc, pbrFsSrc);
        if (m_skinnedPbrProgram) {
            const GLuint skBlockIdx = glGetUniformBlockIndex(m_skinnedPbrProgram, "CameraUBO");
            glUniformBlockBinding(m_skinnedPbrProgram, skBlockIdx, 0);
            const GLuint boneBlockIdx = glGetUniformBlockIndex(m_skinnedPbrProgram, "BoneUBO");
            glUniformBlockBinding(m_skinnedPbrProgram, boneBlockIdx, 1);
        }

        // Skinned-shadow program: pbrSkinnedVsSrc (4-bone LBS) + pbrFsShadowSrc.
        // Skinned meshes cast unskinned bounds shadows (atlas verts use aPos@0 +
        // aModel@4 only), matching the Vulkan forward skinned-shadow convention.
        m_shadowSkinnedPbrProgram = compileProgram(pbrSkinnedVsSrc, pbrFsShadowSrc);
        if (m_shadowSkinnedPbrProgram) {
            const GLuint ssIdx = glGetUniformBlockIndex(m_shadowSkinnedPbrProgram, "ShadowUBO");
            if (ssIdx != GL_INVALID_INDEX) glUniformBlockBinding(m_shadowSkinnedPbrProgram, ssIdx, 2);
            const GLuint sbIdx = glGetUniformBlockIndex(m_shadowSkinnedPbrProgram, "BoneUBO");
            if (sbIdx != GL_INVALID_INDEX) glUniformBlockBinding(m_shadowSkinnedPbrProgram, sbIdx, 1);
        }

        // ---- GL forward-CSM shadow atlas (2048x2048 depth, cascade 0). Mirrors
        // Vulkan's white-fallback atlas when shadows=false (caller never binds it).
        static constexpr int kGlShadowSize = 2048;
        glGenTextures(1, &m_glShadowDepthTex);
        glBindTexture(GL_TEXTURE_2D, m_glShadowDepthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, kGlShadowSize, kGlShadowSize,
                     0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        { float b = 1.0f; glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, &b); }
        glGenFramebuffers(1, &m_glShadowFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_glShadowFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                               m_glShadowDepthTex, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // ShadowUBO (std140, binding 1): matches Vulkan ShadowUBO layout.
        static constexpr int kShadowUBOSize = 224;
        glGenBuffers(1, &m_glShadowUbo);
        glBindBuffer(GL_UNIFORM_BUFFER, m_glShadowUbo);
        glBufferData(GL_UNIFORM_BUFFER, kShadowUBOSize, nullptr, GL_DYNAMIC_DRAW);

        glGenVertexArrays(1, &m_meshVAO);
        glBindVertexArray(m_meshVAO);
        glGenBuffers(1, &m_meshVBO);
        glBindBuffer(GL_ARRAY_BUFFER, m_meshVBO);
        glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
        const GLsizei meshStride = static_cast<GLsizei>(8 * sizeof(float));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, meshStride, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, meshStride, (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, meshStride, (void*)(6 * sizeof(float)));

        glGenBuffers(1, &m_meshEBO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_meshEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);

        glGenBuffers(1, &m_meshInstanceVBO);
        glBindBuffer(GL_ARRAY_BUFFER, m_meshInstanceVBO);
        glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
        const GLsizei meshInstStride = static_cast<GLsizei>(sizeof(OffscreenInstance));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, meshInstStride,
                              (void*)(16 * sizeof(float)));
        glVertexAttribDivisor(3, 1);
        for (int i = 0; i < 4; ++i) {
            glEnableVertexAttribArray(4 + i);
            glVertexAttribPointer(4 + i, 4, GL_FLOAT, GL_FALSE, meshInstStride,
                                  (void*)(i * 4 * sizeof(float)));
            glVertexAttribDivisor(4 + i, 1);
        }
        // Phase 3b: prevModel (offset 80, 144-byte stride) -> location 8..11.
        for (int i = 0; i < 4; ++i) {
            glEnableVertexAttribArray(8 + i);
            glVertexAttribPointer(8 + i, 4, GL_FLOAT, GL_FALSE, meshInstStride,
                                  (void*)(80 + i * 4 * sizeof(float)));
            glVertexAttribDivisor(8 + i, 1);
        }
        // Phase 3c: per-vertex bone data for skinned meshes.
        // loc12 = uvec4 boneIdx, loc13 = vec4 boneWeight (32-byte stride:
        // 4 uint + 4 float). Set up here on m_meshVAO; inert for lambert/PBR
        // shaders that don't declare aBoneIdx/aBoneWeight (GL won't fetch a
        // vertex attribute the shader doesn't consume). The shared m_boneVBO
        // is (re)orphaned/uploaded per skinned mesh in drawMeshes; BoneUBO
        // (binding 1) holds the joint palette and is likewise per-mesh.
        glGenBuffers(1, &m_boneVBO);
        glBindBuffer(GL_ARRAY_BUFFER, m_boneVBO);
        glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(12);
        glVertexAttribIPointer(12, 4, GL_UNSIGNED_INT, 32, (void*)0);
        glEnableVertexAttribArray(13);
        glVertexAttribPointer(13, 4, GL_FLOAT, GL_FALSE, 32, (void*)(16));
        glGenBuffers(1, &m_boneUBO);
        glBindBuffer(GL_UNIFORM_BUFFER, m_boneUBO);
        glBufferData(GL_UNIFORM_BUFFER, 32 * 16 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        glBindVertexArray(0);

        // Color texture + depth renderbuffer attached to an FBO.
        glGenFramebuffers(1, &m_sceneFBO);
        glGenTextures(1, &m_sceneColorTex);
        glBindTexture(GL_TEXTURE_2D, m_sceneColorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenRenderbuffers(1, &m_sceneDepthRB);
        glBindRenderbuffer(GL_RENDERBUFFER, m_sceneDepthRB);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               m_sceneColorTex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                  m_sceneDepthRB);

        // Phase 3b: second color attachment (RG32f) for screen-space motion
        // vectors. Color (loc0) is unchanged -> GL<->Vulkan color parity holds;
        // velocity (loc1) is written only by the mesh/pbr fragment shaders.
        glGenTextures(1, &m_sceneVelocityTex);
        glBindTexture(GL_TEXTURE_2D, m_sceneVelocityTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, w, h, 0, GL_RG, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                               m_sceneVelocityTex, 0);
        const GLenum drawBufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, drawBufs);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[RHI-GL] Scene FBO incomplete (" << std::hex << status
                      << std::dec << ")" << std::endl;
            return false;
        }

        // Phase 3d: deferred G-Buffer + lighting pass (mirrors rhi/shaders/
        // gbuffer.vert/frag + lighting.vert/frag so GL deferred pixels match
        // Vulkan's renderOffscreenScene(deferred=true)). Non-fatal if shaders
        // fail to compile/link: the deferred path bails in renderOffscreenScene
        // instead of corrupting the forward path.
        static const char* gbufferFsSrc =
            "#version 330 core\n"
            "in vec4 vColor;\n"
            "in vec3 vNormal;\n"
            "in vec2 vUV;\n"
            "layout(std140) uniform CameraUBO { mat4 viewProj; vec3 camPos;"
            " float fogDensity; vec3 fogColor; float pad; };\n"
            "uniform sampler2D uTex;\n"
            "layout(location = 0) out vec4 gAlbedo;\n"
            "layout(location = 1) out vec4 gNormalRough;\n"
            "void main(){\n"
            "  vec3 albedo = texture(uTex, vUV).rgb * vColor.rgb;\n"
            "  vec3 N = normalize(vNormal);\n"
            "  float lum = dot(albedo, vec3(0.2126, 0.7152, 0.0722));\n"
            "  float roughness = clamp(0.4 + (1.0 - lum) * 0.4, 0.04, 1.0);\n"
            "  gAlbedo = vec4(albedo, 1.0);\n"
            "  gNormalRough = vec4(N, roughness); }\n";
        static const char* lightingVsSrc =
            "#version 330 core\n"
            "out vec2 fUV;\n"
            "void main(){ vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);"
            " gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); fUV = p; }\n";
        static const char* lightingFsSrc =
            "#version 330 core\n"
            "in vec2 fUV;\n"
            "out vec4 outColor;\n"
            "uniform sampler2D uAlbedo;\n"
            "uniform sampler2D uNormalRough;\n"
            "uniform sampler2D uAo;\n"     // B2 SSAO (white 1.0 default -> parity preserved)
            "const vec3 kLightDir = normalize(vec3(0.5, 1.0, 0.3));\n"
            "const vec3 kLightColor = vec3(1.0, 0.98, 0.92);\n"
            "void main(){\n"
            "  vec4 albedo = texture(uAlbedo, fUV);\n"
            "  vec4 nr = texture(uNormalRough, fUV);\n"
            "  vec3 N = normalize(nr.xyz);\n"
            "  float NdotL = max(0.0, dot(N, kLightDir));\n"
            "  vec3 skyCol = vec3(0.4, 0.5, 0.7);\n"
            "  vec3 groundCol = vec3(0.3, 0.2, 0.1);\n"
            "  vec3 ambient = mix(groundCol, skyCol, N.y * 0.5 + 0.5) * 0.3;\n"
            "  ambient *= texture(uAo, fUV).r;\n"   // B2 SSAO (uAo=1.0 default -> no-op, parity preserved)
            "  vec3 lit = ambient + albedo.rgb * kLightColor * NdotL * 0.94;\n"
            "  outColor = vec4(lit, albedo.a); }\n";
        m_lightingProgram = compileProgram(lightingVsSrc, lightingFsSrc);
        m_gbufferProgram = compileProgram(pbrVsSrc, gbufferFsSrc);
        if (m_gbufferProgram) {
            const GLuint gb = glGetUniformBlockIndex(m_gbufferProgram, "CameraUBO");
            glUniformBlockBinding(m_gbufferProgram, gb, 0);
        }
        // 1x1 white R8 fallback for the lighting uAo sampler. Created here (in
        // ensureSceneResources, not lazily inside ensureSsaoResources) so it is
        // ALWAYS bound to uAo when ssao is off — otherwise the sampler sees the
        // null texture (r=0) and `ambient *= 0` would make GL lighting darker than
        // the Vulkan white fallback, breaking the 60/60 deferred parity test.
        glGenTextures(1, &m_white1x1Ao);
        glBindTexture(GL_TEXTURE_2D, m_white1x1Ao);
        uint8_t w1[1] = { 255 };
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, w1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glGenFramebuffers(1, &m_gbufferFBO);
        glGenTextures(1, &m_gAlbedoTex);
        glBindTexture(GL_TEXTURE_2D, m_gAlbedoTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenTextures(1, &m_gNormalTex);
        glBindTexture(GL_TEXTURE_2D, m_gNormalTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        // G-Buffer depth: a samplerable DEPTH texture (not a renderbuffer) so the
        // B2 SSAO compute shader can read scene depth. R8G8B8A8 albedo + R16G16B16A16F
        // normal are already textures (sampled by SSAO for world-normal); the
        // depth buffer now matches that convention. The lighting pass (binding 4)
        // still binds the WHITE depth fallback in the parity path, so 60/60 tests
        // stay byte-identical (lighting uses its own white depth, not this one).
        glGenTextures(1, &m_gDepthTex);
        glBindTexture(GL_TEXTURE_2D, m_gDepthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, w, h, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Depth comparison OFF so the SSAO compute reads the raw depth value.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        glBindFramebuffer(GL_FRAMEBUFFER, m_gbufferFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gAlbedoTex, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_gNormalTex, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_gDepthTex, 0);
        const GLenum gbDraws[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, gbDraws);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[RHI-GL] G-Buffer FBO incomplete" << std::endl;
        }
        // Fullscreen-triangle VAO for the lighting pass (no vertex buffers;
        // the triangle is built from gl_VertexID).
        glGenVertexArrays(1, &m_lightingVAO);
        glBindVertexArray(m_lightingVAO);
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return true;
    }

    void destroySceneResources() {
        if (m_sceneProgram) { glDeleteProgram(m_sceneProgram); m_sceneProgram = 0; }
        if (m_sceneVAO) { glDeleteVertexArrays(1, &m_sceneVAO); m_sceneVAO = 0; }
        if (m_sceneVBO) { glDeleteBuffers(1, &m_sceneVBO); m_sceneVBO = 0; }
        if (m_sceneInstanceVBO) { glDeleteBuffers(1, &m_sceneInstanceVBO); m_sceneInstanceVBO = 0; }
        if (m_sceneUBO) { glDeleteBuffers(1, &m_sceneUBO); m_sceneUBO = 0; }
        if (m_skyboxProgram) { glDeleteProgram(m_skyboxProgram); m_skyboxProgram = 0; }
        // Phase-B B4 skybox cubemaps: persistent user resources, torn down in
        // shutdown() only — NOT here, because destroySceneResources runs on
        // every render via ensureSceneResources and would kill cubemaps mid-frame.

        if (m_sceneFBO) { glDeleteFramebuffers(1, &m_sceneFBO); m_sceneFBO = 0; }
        if (m_sceneColorTex) { glDeleteTextures(1, &m_sceneColorTex); m_sceneColorTex = 0; }
        if (m_sceneDepthRB) { glDeleteRenderbuffers(1, &m_sceneDepthRB); m_sceneDepthRB = 0; }
        if (m_sceneVelocityTex) { glDeleteTextures(1, &m_sceneVelocityTex); m_sceneVelocityTex = 0; }
        if (m_meshProgram) { glDeleteProgram(m_meshProgram); m_meshProgram = 0; }
        if (m_pbrMeshProgram) { glDeleteProgram(m_pbrMeshProgram); m_pbrMeshProgram = 0; }
        if (m_skinnedPbrProgram) { glDeleteProgram(m_skinnedPbrProgram); m_skinnedPbrProgram = 0; }
        if (m_shadowPbrProgram) { glDeleteProgram(m_shadowPbrProgram); m_shadowPbrProgram = 0; }
        if (m_shadowSkinnedPbrProgram) { glDeleteProgram(m_shadowSkinnedPbrProgram); m_shadowSkinnedPbrProgram = 0; }
        if (m_shadowDepthProgram) { glDeleteProgram(m_shadowDepthProgram); m_shadowDepthProgram = 0; }
        if (m_glShadowDepthTex) { glDeleteTextures(1, &m_glShadowDepthTex); m_glShadowDepthTex = 0; }
        if (m_glShadowFBO) { glDeleteFramebuffers(1, &m_glShadowFBO); m_glShadowFBO = 0; }
        if (m_glShadowUbo) { glDeleteBuffers(1, &m_glShadowUbo); m_glShadowUbo = 0; }
        if (m_boneVBO) { glDeleteBuffers(1, &m_boneVBO); m_boneVBO = 0; }
        if (m_boneUBO) { glDeleteBuffers(1, &m_boneUBO); m_boneUBO = 0; }
        if (m_gbufferProgram) { glDeleteProgram(m_gbufferProgram); m_gbufferProgram = 0; }
        if (m_lightingProgram) { glDeleteProgram(m_lightingProgram); m_lightingProgram = 0; }
        if (m_gbufferFBO) { glDeleteFramebuffers(1, &m_gbufferFBO); m_gbufferFBO = 0; }
        if (m_gAlbedoTex) { glDeleteTextures(1, &m_gAlbedoTex); m_gAlbedoTex = 0; }
        if (m_gNormalTex) { glDeleteTextures(1, &m_gNormalTex); m_gNormalTex = 0; }
        if (m_gDepthTex) { glDeleteTextures(1, &m_gDepthTex); m_gDepthTex = 0; }
        if (m_gDepthRB) { glDeleteRenderbuffers(1, &m_gDepthRB); m_gDepthRB = 0; }
        // B2 SSAO
        if (m_ssaoProgram) { glDeleteProgram(m_ssaoProgram); m_ssaoProgram = 0; }
        if (m_ssaoBlurProgram) { glDeleteProgram(m_ssaoBlurProgram); m_ssaoBlurProgram = 0; }
        if (m_ssaoNoiseTex) { glDeleteTextures(1, &m_ssaoNoiseTex); m_ssaoNoiseTex = 0; }
        if (m_ssaoKernelUbo) { glDeleteBuffers(1, &m_ssaoKernelUbo); m_ssaoKernelUbo = 0; }
        if (m_ssaoAoRaw) { glDeleteTextures(1, &m_ssaoAoRaw); m_ssaoAoRaw = 0; }
        if (m_ssaoAoBlur) { glDeleteTextures(1, &m_ssaoAoBlur); m_ssaoAoBlur = 0; }
        if (m_white1x1Ao) { glDeleteTextures(1, &m_white1x1Ao); m_white1x1Ao = 0; }
        // B5 Bloom (mirror SSAO teardown on resize)
        cleanupBloomResources();
        if (m_lightingVAO) { glDeleteVertexArrays(1, &m_lightingVAO); m_lightingVAO = 0; }
        if (m_meshVAO) { glDeleteVertexArrays(1, &m_meshVAO); m_meshVAO = 0; }
        if (m_meshEBO) { glDeleteBuffers(1, &m_meshEBO); m_meshEBO = 0; }
        if (m_meshInstanceVBO) { glDeleteBuffers(1, &m_meshInstanceVBO); m_meshInstanceVBO = 0; }
        for (auto& [mesh, tex] : m_meshTexCache) { (void)mesh; if (tex.tex) glDeleteTextures(1, &tex.tex); }
        m_meshTexCache.clear();
        if (m_whiteTex) { glDeleteTextures(1, &m_whiteTex); m_whiteTex = 0; }
        m_sceneW = m_sceneH = 0;
    }

    static GLuint compileProgram(const char* vsSrc, const char* fsSrc) {
        auto check = [](GLuint obj, bool isProgram, const char* what) {
            GLint ok = 0;
            if (isProgram) glGetProgramiv(obj, GL_LINK_STATUS, &ok);
            else glGetShaderiv(obj, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char log[2048] = {};
                if (isProgram) glGetProgramInfoLog(obj, sizeof(log), nullptr, log);
                else glGetShaderInfoLog(obj, sizeof(log), nullptr, log);
                std::cerr << "[RHI-GL] " << what << " failed: " << log << std::endl;
            }
            return ok != 0;
        };
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &vsSrc, nullptr);
        glCompileShader(vs);
        if (!check(vs, false, "scene VS")) return 0;
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &fsSrc, nullptr);
        glCompileShader(fs);
        if (!check(fs, false, "scene FS")) return 0;
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        if (!check(prog, true, "scene program")) return 0;
        glDeleteShader(vs);
        glDeleteShader(fs);
        return prog;
    }

    // GL readback is bottom-up (row 0 = bottom); the RHI contract is row 0 =
    // TOP (matching the Vulkan copy order). Swap rows in place.
    static void FlipRows(unsigned char* rgba, int w, int h) {
        std::vector<unsigned char> row(static_cast<size_t>(w) * 4);
        for (int y = 0; y < h / 2; ++y) {
            unsigned char* top = rgba + static_cast<size_t>(y) * w * 4;
            unsigned char* bot = rgba + static_cast<size_t>(h - 1 - y) * w * 4;
            std::memcpy(row.data(), top, row.size());
            std::memcpy(top, bot, row.size());
            std::memcpy(bot, row.data(), row.size());
        }
    }

    void ensureTriangleResources() {
        if (m_triProgram) return;

        static const char* vsSrc =
            "#version 330 core\n"
            "layout(location=0) in vec2 aPos;\n"
            "void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }\n";
        static const char* fsSrc =
            "#version 330 core\n"
            "out vec4 c;\n"
            "void main(){ c = vec4(0.0, 1.0, 0.0, 1.0); }\n";

        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &vsSrc, nullptr);
        glCompileShader(vs);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &fsSrc, nullptr);
        glCompileShader(fs);

        m_triProgram = glCreateProgram();
        glAttachShader(m_triProgram, vs);
        glAttachShader(m_triProgram, fs);
        glLinkProgram(m_triProgram);
        {
            GLint linked = 0;
            glGetProgramiv(m_triProgram, GL_LINK_STATUS, &linked);
            if (linked == GL_FALSE) {
                char log[1024];
                glGetProgramInfoLog(m_triProgram, sizeof(log), nullptr, log);
                std::cerr << "[RHI-GL] Triangle readback program link failed: " << log << "\n";
                glDeleteProgram(m_triProgram);
                m_triProgram = 0;
            }
        }
        glDeleteShader(vs);
        glDeleteShader(fs);

        // Centered triangle (NDC): spans [-0.5,0.5] in x, [-0.5,0.5] in y
        // with the apex up. Same geometry as the Vulkan path so the readback
        // test can assert the CENTER pixel is green and the CORNER is the
        // clear color on both backends.
        const float verts[6] = {-0.5f, -0.5f,  0.5f, -0.5f,  0.0f, 0.5f};
        glGenVertexArrays(1, &m_triVAO);
        glGenBuffers(1, &m_triVBO);
        glBindVertexArray(m_triVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_triVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
        glBindVertexArray(0);
    }

    GLFWwindow* m_window = nullptr;
    int m_width = 0;
    int m_height = 0;
    GLuint m_triProgram = 0;
    GLuint m_triVAO = 0;
    GLuint m_triVBO = 0;
    GLuint m_sceneProgram = 0;
    GLuint m_sceneVAO = 0;
    GLuint m_sceneVBO = 0;
    GLuint m_sceneInstanceVBO = 0;
    GLuint m_sceneUBO = 0;
    GLuint m_sceneFBO = 0;
    GLuint m_sceneColorTex = 0;
    GLuint m_sceneDepthRB = 0;
    GLuint m_sceneVelocityTex = 0;  // Phase 3b: RG32f velocity attachment
    GLuint m_meshProgram = 0;
    GLuint m_pbrMeshProgram = 0;
    GLuint m_skinnedPbrProgram = 0;   // Phase 3c: GL twin of pbr_skinned.vert
    GLuint m_shadowPbrProgram = 0;    // B1-forward: pbrVsSrc + pbrFsShadowSrc (GL CSM parity)
    GLuint m_shadowSkinnedPbrProgram = 0; // B1-forward: skinned + CSM (BoneUBO@1, ShadowUBO@2)
    GLuint m_shadowDepthProgram = 0;  // B1-forward: GL shadow atlas depth vert+frag
    GLuint m_glShadowDepthTex = 0;    // B1-forward: 2048x2048 GL_TEXTURE_2D depth atlas
    GLuint m_glShadowFBO = 0;         // B1-forward: shadow atlas FBO
    GLuint m_glShadowUbo = 0;         // B1-forward: ShadowUBO (std140) @ binding 2
    GLuint m_meshVAO = 0;
    GLuint m_meshVBO = 0;
    GLuint m_meshEBO = 0;
    GLuint m_meshInstanceVBO = 0;
    GLuint m_boneVBO = 0;            // per-vertex boneIdx/Weight (loc12/13)
    GLuint m_boneUBO = 0;            // joint palette (mat4 bones[], binding 1)
    GLuint m_gbufferProgram = 0;     // Phase 3d: GL deferred geometry (pbrVsSrc + gbuffer.fs)
    GLuint m_lightingProgram = 0;    // Phase 3d: GL deferred fullscreen lighting
    GLuint m_gbufferFBO = 0;         // Phase 3d: G-Buffer framebuffer
    GLuint m_gAlbedoTex = 0;         // Phase 3d: G-Buffer albedo (RGBA8)
    GLuint m_gNormalTex = 0;         // Phase 3d: G-Buffer normal/roughness (RGBA16f)
    GLuint m_gDepthRB = 0;           // Phase 3d: G-Buffer depth
    GLuint m_gDepthTex = 0;          // B2 SSAO: samplerable G-Buffer depth texture (DEPTH24) - replaces m_gDepthRB when SSAO runs
    GLuint m_ssaoProgram = 0;        // B2 SSAO: compute shader (geometry -> AO factor)
    GLuint m_ssaoBlurProgram = 0;    // B2 SSAO: 4x4 bilateral blur (noise removal)
    GLuint m_ssaoNoiseTex = 0;       // B2 SSAO: 4x4 RG8 noise
    GLuint m_ssaoKernelUbo = 0;     // B2 SSAO: SsaoUBOData (viewProj + invViewProj + 16-sample kernel)
    GLuint m_ssaoAoRaw = 0;          // B2 SSAO: raw AO output (R8)
    GLuint m_ssaoAoBlur = 0;         // B2 SSAO: blurred AO output (R8) fed to the lighting pass
    GLuint m_white1x1Ao = 0;         // B2 SSAO: 1x1 white R8 (uAo default -> ambient untouched -> 60/60 parity)
    // B5 Bloom: 3-pass (extract -> bilateral gauss blur -> additive composite)
    // GL compute twins of rhi/shaders/bloom_*.comp. NO-OP when scene.bloom is
    // false so the 60/60 deferred parity tests stay byte-identical.
    GLuint m_bloomExtractProgram = 0;   // brightpass: HDR scene -> outExtract (rgba16f)
    GLuint m_bloomBlurProgram   = 0;    // 9-tap gauss: outExtract -> outBlur (rgba16f)
    GLuint m_bloomCompositeProgram = 0; // HDR + bloom -> outFinal (rgba16f) read back when bloom on
    GLuint m_bloomUBO = 0;              // BloomUBO{float threshold, float strength} @ std140 binding 1
    GLuint m_bloomRaw  = 0;             // extract target (rgba16f, w x h)
    GLuint m_bloomBlur = 0;             // blur target (rgba16f, w x h)
    GLuint m_bloomFinal = 0;            // composite target (rgba16f, w x h) -- read back when scene.bloom
    bool   m_bloomCreated = false;
    GLuint m_lightingVAO = 0;        // Phase 3d: fullscreen-triangle VAO
    // Phase-B B4 skybox (GL twin of rhi/shaders/skybox.{vert,frag})
    GLuint m_skyboxProgram = 0;     // inline GLSL 3.30 (samplers via glUniform, not layout(binding))
    std::vector<GLuint> m_cubemaps; // 1-indexed cubemap texture handles (0 == null)
    // Per-mesh albedo texture cache (mesh pointer -> texture) + shared 1x1
    // white fallback for untextured meshes.
    struct MeshTex { GLuint tex = 0; int w = 0; int h = 0; };
    std::map<const OffscreenMesh*, MeshTex> m_meshTexCache;
    GLuint m_whiteTex = 0;
    int m_sceneW = 0;
    int m_sceneH = 0;
    // FSR3 (Phase 1): previous frame's viewProj + frame counter for Halton jitter.
    RHI::Mat4 m_prevViewProj{};
    uint32_t m_frame = 0;

    // FSR3 EASU (Phase 4 / Phase 5b GL fallback): canonical AMD edge-directed
    // spatial upscale as a GLSL 3.30 fullscreen-fragment pass (GL has no
    // compute shaders). Built once; the full-res output FBO/texture is resized
    // per call in renderOffscreenSceneEasu.
    GLuint m_easuProgram = 0;
    GLuint m_easuVAO = 0;
    GLuint m_easuVBO = 0;
    GLuint m_easuOutFBO = 0;
    GLuint m_easuOutTex = 0;
    int m_easuOutW = 0;
    int m_easuOutH = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// API helpers + factory
// ---------------------------------------------------------------------------

const char* graphicsApiName(GraphicsAPI api) {
    switch (api) {
        case GraphicsAPI::OpenGL: return "opengl";
        case GraphicsAPI::Vulkan: return "vulkan";
    }
    return "unknown";
}

bool parseGraphicsApi(const std::string& token, GraphicsAPI& out) {
    if (token == "opengl") { out = GraphicsAPI::OpenGL; return true; }
    if (token == "vulkan") { out = GraphicsAPI::Vulkan; return true; }
    return false;
}

std::unique_ptr<IRHI> createRHI(GraphicsAPI api) {
    switch (api) {
        case GraphicsAPI::OpenGL:
            return std::make_unique<RHIGL>();
        case GraphicsAPI::Vulkan:
            // Implemented in RHIVulkan.cpp (createVulkanRHI()). Declared in
            // RHI.h so the GLFW/lavapipe probe in setGraphicsApi() and the RHI
            // tests actually exercise the Vulkan backend instead of falling
            // through to a nullptr stub.
            return createVulkanRHI();
    }
    return nullptr;
}

} // namespace RHI
