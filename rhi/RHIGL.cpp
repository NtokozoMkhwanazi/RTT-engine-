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
#include <cstring>

namespace RHI {

// ---------------------------------------------------------------------------
// Graphics API selection state (shared by both backends)
// ---------------------------------------------------------------------------
namespace {

GraphicsAPI g_activeApi = GraphicsAPI::OpenGL;   // default: low/optimized mode
constexpr const char* kApiConfigFile = "graphics_api.cfg";

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
        glfwSwapInterval(1);

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
        return true;
    }

    void shutdown() override {
        // Free GL-owned resources while the context is still alive.
        destroyEasuResources();
        destroySceneResources();
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

    bool renderFrameScene(const OffscreenScene& scene) override {
        // GL analog of the Vulkan swapchain render: draw the scene into the
        // window's DEFAULT framebuffer (which GLFW gives a depth buffer).
        // endFrame() then swaps it to screen. This is the GL path the
        // editor's windowed rendering can use; the engine renderer currently
        // draws via its own GL code between beginFrame/endFrame.
        if (!m_window || (scene.instances.empty() && scene.meshes.empty())) return false;
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
                              float* outVelocityRG = nullptr) override {
        if (!m_window || (scene.instances.empty() && scene.meshes.empty())) return false;
        if (!ensureSceneResources(w, h)) return false;

        // Camera -> viewProj, GL convention (no clip correction needed).
        uploadSceneUBO(scene, w, h);

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

        // Readback while the FBO is still bound, then flip to row 0 = top.
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        if (outRGBA) glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, outRGBA);
        if (outVelocityRG) {
            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glReadPixels(0, 0, w, h, GL_RG, GL_FLOAT, outVelocityRG);
            RHI::FlipRows(outVelocityRG, w, h);
        }
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
        if (m_pbrMeshProgram) {
            glUseProgram(m_sceneProgram);
            glUniform2f(glGetUniformLocation(m_sceneProgram, "uJitter"), j.x, j.y);
            glUseProgram(m_meshProgram);
            glUniform2f(glGetUniformLocation(m_meshProgram, "uJitter"), j.x, j.y);
            glUseProgram(m_pbrMeshProgram);
            glUniform2f(glGetUniformLocation(m_pbrMeshProgram, "uJitter"), j.x, j.y);
            glUseProgram(0);
        }
        // Advance per-frame history: next frame reprojects from this viewProj.
        m_prevViewProj = viewProj;
        ++m_frame;
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
    void drawMeshes(const OffscreenScene& scene) {
        if (scene.meshes.empty()) return;
        // PBR path: Cook-Torrance + ACES + gamma, bit-identical math to the
        // Vulkan pbr_mesh.* shaders (parity). Falls back to the lambert mesh
        // program when PBR is disabled or not compiled.
        const bool usePbr = scene.pbrEnabled && m_pbrMeshProgram;
        GLuint prog = usePbr ? m_pbrMeshProgram : m_meshProgram;
        if (!prog) return;
        glUseProgram(prog);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_sceneUBO);
        glBindVertexArray(m_meshVAO);
        glActiveTexture(GL_TEXTURE0);
        for (const auto& mesh : scene.meshes) {
            if (mesh.vertices.empty() || mesh.indices.empty() || mesh.instances.empty()) continue;
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
        glBindTexture(GL_TEXTURE_2D, 0);
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
            "void main(){ vColor = aColor; vUV = aUV;"
            " vWorldPos = vec3(aModel * vec4(aPos, 1.0));"
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
            "void main(){\n"
            "  vec3 albedo = texture(uTex, vUV).rgb * vColor.rgb;\n"
            "  vec3 N = normalize(vNormal);\n"
            "  vec3 V = normalize(camPos - vWorldPos);\n"
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
        return true;
    }

    void destroySceneResources() {
        if (m_sceneProgram) { glDeleteProgram(m_sceneProgram); m_sceneProgram = 0; }
        if (m_sceneVAO) { glDeleteVertexArrays(1, &m_sceneVAO); m_sceneVAO = 0; }
        if (m_sceneVBO) { glDeleteBuffers(1, &m_sceneVBO); m_sceneVBO = 0; }
        if (m_sceneInstanceVBO) { glDeleteBuffers(1, &m_sceneInstanceVBO); m_sceneInstanceVBO = 0; }
        if (m_sceneUBO) { glDeleteBuffers(1, &m_sceneUBO); m_sceneUBO = 0; }
        if (m_sceneFBO) { glDeleteFramebuffers(1, &m_sceneFBO); m_sceneFBO = 0; }
        if (m_sceneColorTex) { glDeleteTextures(1, &m_sceneColorTex); m_sceneColorTex = 0; }
        if (m_sceneDepthRB) { glDeleteRenderbuffers(1, &m_sceneDepthRB); m_sceneDepthRB = 0; }
        if (m_sceneVelocityTex) { glDeleteTextures(1, &m_sceneVelocityTex); m_sceneVelocityTex = 0; }
        if (m_meshProgram) { glDeleteProgram(m_meshProgram); m_meshProgram = 0; }
        if (m_pbrMeshProgram) { glDeleteProgram(m_pbrMeshProgram); m_pbrMeshProgram = 0; }
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
    GLuint m_meshVAO = 0;
    GLuint m_meshVBO = 0;
    GLuint m_meshEBO = 0;
    GLuint m_meshInstanceVBO = 0;
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
            // Implemented in RHIVulkan.cpp - defined there via forward decl.
            extern std::unique_ptr<IRHI> createVulkanRHI();
            return createVulkanRHI();
    }
    return nullptr;
}

} // namespace RHI
