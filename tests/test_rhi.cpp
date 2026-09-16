/**
 * RHI (Render Hardware Interface) tests.
 *
 * Verifies the dual-backend foundation:
 *  - API parsing + naming helpers (pure logic)
 *  - OpenGL backend: window + context initialize, frames begin/end/present
 *  - Vulkan backend: instance/device/swapchain create, clear-frame present
 *
 * Both backends use the hidden-window pattern so the tests run headless (no
 * display server required). The Vulkan backend is skipped cleanly when no
 * Vulkan driver is present (virtio/radeon here), matching CI boxes.
 */

#include <gtest/gtest.h>
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>
#include <fstream>
#include <cstring>
#include <imgui.h>
#include "imgui_impl_vulkan.h"

#include "../rhi/RHI.h"
#include "../rhi/RHIMath.h"

namespace {

TEST(RHI, ConfigPersistence) {
    // The Graphics menu writes graphics_api.cfg; the entry point reads it back
    // on the next launch. Verify the round-trip (and that an unknown token
    // falls back to OpenGL rather than crashing).
    EXPECT_TRUE(RHI::setGraphicsApi(RHI::GraphicsAPI::OpenGL));
    EXPECT_EQ(RHI::activeGraphicsApi(), RHI::GraphicsAPI::OpenGL);

    // Parse back the file the way editor_main does.
    {
        std::ifstream f(RHI::graphicsApiConfigPath());
        std::string token;
        RHI::GraphicsAPI api = RHI::GraphicsAPI::OpenGL;
        ASSERT_TRUE(f >> token);
        EXPECT_TRUE(RHI::parseGraphicsApi(token, api));
        EXPECT_EQ(api, RHI::GraphicsAPI::OpenGL);
    }

    // Vulkan persistence is gated on availability: on this machine the loader
    // exists, so setGraphicsApi should succeed (device is creatable headless).
    if (RHI::setGraphicsApi(RHI::GraphicsAPI::Vulkan)) {
        std::ifstream f(RHI::graphicsApiConfigPath());
        std::string token;
        f >> token;
        EXPECT_EQ(token, std::string("vulkan"));
        // Restore OpenGL so the editor default stays sane.
        EXPECT_TRUE(RHI::setGraphicsApi(RHI::GraphicsAPI::OpenGL));
    }
}

TEST(RHI, ApiNameAndParsing) {
    EXPECT_STREQ(RHI::graphicsApiName(RHI::GraphicsAPI::OpenGL), "opengl");
    EXPECT_STREQ(RHI::graphicsApiName(RHI::GraphicsAPI::Vulkan), "vulkan");

    RHI::GraphicsAPI api = RHI::GraphicsAPI::OpenGL;
    EXPECT_TRUE(RHI::parseGraphicsApi("opengl", api));
    EXPECT_EQ(api, RHI::GraphicsAPI::OpenGL);
    EXPECT_TRUE(RHI::parseGraphicsApi("vulkan", api));
    EXPECT_EQ(api, RHI::GraphicsAPI::Vulkan);
    EXPECT_FALSE(RHI::parseGraphicsApi("directx", api));
    EXPECT_FALSE(RHI::parseGraphicsApi("", api));
}

namespace {

// A centered NDC triangle (green) on a dark background: the CENTER pixel is
// green, the CORNER pixel is the clear color. Shared by both backends so the
// ported Vulkan path must produce the same pixels as OpenGL.
bool VerifyTrianglePixels(const unsigned char* rgba, int w, int h) {
    const unsigned char* center = rgba + ((h / 2) * w + (w / 2)) * 4;
    const unsigned char* corner = rgba;   // bottom-left

    // Center: green (0,255,0).
    if (center[0] > 40 || center[1] < 200 || center[2] > 40) return false;
    // Corner: clear dark gray (25,25,25) - the triangle does not reach it.
    if (corner[0] > 60 || corner[1] > 60 || corner[2] > 60) return false;
    return true;
}

} // namespace

TEST(RHI, OpenGLBackend_RendersTriangleOffscreen) {
    auto rhi = RHI::createRHI(RHI::GraphicsAPI::OpenGL);
    ASSERT_NE(rhi, nullptr);
    ASSERT_TRUE(rhi->initialize(64, 64, "rhi-gl-offscreen"));

    std::vector<unsigned char> px(64 * 64 * 4);
    ASSERT_TRUE(rhi->renderOffscreenTriangle(64, 64, px.data()));
    EXPECT_TRUE(VerifyTrianglePixels(px.data(), 64, 64))
        << "OpenGL must render the triangle (center green, corner clear)";

    rhi->shutdown();
}

TEST(RHI, OpenGLBackend_InitializesAndPresents) {
    auto rhi = RHI::createRHI(RHI::GraphicsAPI::OpenGL);
    ASSERT_NE(rhi, nullptr);
    ASSERT_TRUE(rhi->initialize(64, 64, "rhi-gl-test"));

    EXPECT_EQ(rhi->api(), RHI::GraphicsAPI::OpenGL);
    EXPECT_NE(rhi->window(), nullptr);
    EXPECT_EQ(rhi->width(), 64);
    EXPECT_EQ(rhi->height(), 64);

    // A few full frames must present without error.
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(rhi->beginFrame());
        rhi->endFrame();
    }

    rhi->shutdown();
    // After shutdown the backend must be idle-safe (double shutdown OK).
    rhi->shutdown();
}

// ---------------------------------------------------------------------------
// Offscreen 3D scene tests (camera UBO + depth + instancing)
// ---------------------------------------------------------------------------
// A unit cube scaled by s and centered at (x,y,z), tinted (r,g,b). The model
// matrix is translate*scale, column-major.
RHI::OffscreenInstance MakeCube(float x, float y, float z, float s,
                                float r, float g, float b) {
    RHI::OffscreenInstance inst{};
    const RHI::Mat4 m = RHI::mat4TranslateScale(x, y, z, s);
    std::memcpy(inst.model, m.m, sizeof(m.m));
    inst.color[0] = r; inst.color[1] = g; inst.color[2] = b; inst.color[3] = 1.0f;
    return inst;
}

// Camera A: in FRONT of the cubes (looking -Z). Camera B: BEHIND them.
RHI::OffscreenScene DepthScene(bool fromBehind) {
    RHI::OffscreenScene scene;
    // Slight downward look (y=0.4). Kept low so the near cube's silhouette
    // fully covers the far cube's angular extent (a steeper look-down would
    // let the far cube's top corners peek over the near cube's edge).
    scene.camera.eye[0] = 0; scene.camera.eye[1] = 0.4f; scene.camera.eye[2] = fromBehind ? -6.0f : 0.0f;
    scene.camera.target[0] = 0; scene.camera.target[1] = 0; scene.camera.target[2] = -3.0f;
    scene.camera.fovDeg = 60.0f;
    // Red cube near (z=-2), blue cube far (z=-4) - same world placement in
    // both views, only the camera moves.
    scene.instances.push_back(MakeCube(0, 0, -2.0f, 1.0f, 1.0f, 0.1f, 0.1f));
    scene.instances.push_back(MakeCube(0, 0, -4.0f, 1.0f, 0.1f, 0.1f, 1.0f));
    return scene;
}

// Four differently-colored cubes along X at z=-4 (instancing test).
RHI::OffscreenScene InstancedScene() {
    RHI::OffscreenScene scene;
    scene.camera.eye[0] = 0; scene.camera.eye[1] = 0.6f; scene.camera.eye[2] = 0;
    scene.camera.target[0] = 0; scene.camera.target[1] = 0; scene.camera.target[2] = -4.0f;
    scene.camera.fovDeg = 60.0f;
    scene.instances.push_back(MakeCube(-1.5f, 0, -4.0f, 0.8f, 1.0f, 0.1f, 0.1f));  // red
    scene.instances.push_back(MakeCube(-0.5f, 0, -4.0f, 0.8f, 0.1f, 1.0f, 0.1f));  // green
    scene.instances.push_back(MakeCube( 0.5f, 0, -4.0f, 0.8f, 0.1f, 0.1f, 1.0f));  // blue
    scene.instances.push_back(MakeCube( 1.5f, 0, -4.0f, 0.8f, 1.0f, 0.9f, 0.1f));  // yellow
    return scene;
}

bool IsRed(const unsigned char* p)    { return p[0] > 150 && p[1] < 60 && p[2] < 60; }
bool IsBlue(const unsigned char* p)   { return p[2] > 150 && p[0] < 60 && p[1] < 60; }
bool IsGreen(const unsigned char* p)  { return p[1] > 150 && p[0] < 60 && p[2] < 60; }
bool IsYellow(const unsigned char* p) { return p[0] > 150 && p[1] > 150 && p[2] < 60; }

int CountColor(const unsigned char* rgba, int w, int h, bool (*pred)(const unsigned char*)) {
    int n = 0;
    for (int i = 0; i < w * h; ++i) {
        if (pred(rgba + i * 4)) ++n;
    }
    return n;
}

// The center pixel of the image (row 0 = top).
const unsigned char* CenterPixel(const unsigned char* rgba, int w, int h) {
    return rgba + (static_cast<size_t>(h / 2) * w + w / 2) * 4;
}

TEST(RHI, OpenGLBackend_SceneDepthAndCamera) {
    auto rhi = RHI::createRHI(RHI::GraphicsAPI::OpenGL);
    ASSERT_NE(rhi, nullptr);
    ASSERT_TRUE(rhi->initialize(64, 64, "rhi-gl-scene"));

    std::vector<unsigned char> px(128 * 128 * 4);

    // Camera in front: the near RED cube must occlude the far blue one.
    const RHI::OffscreenScene front = DepthScene(false);
    ASSERT_TRUE(rhi->renderOffscreenScene(128, 128, front, px.data()));
    const unsigned char* c = CenterPixel(px.data(), 128, 128);
    EXPECT_TRUE(IsRed(c)) << "center must be red (near cube) - got (" << (int)c[0]
                          << "," << (int)c[1] << "," << (int)c[2] << ")";
    EXPECT_GT(CountColor(px.data(), 128, 128, IsRed), 1000);
    EXPECT_EQ(CountColor(px.data(), 128, 128, IsBlue), 0)
        << "far blue cube must be fully occluded by depth testing";

    // Camera behind: now the blue cube is nearer, so it occludes red.
    const RHI::OffscreenScene back = DepthScene(true);
    ASSERT_TRUE(rhi->renderOffscreenScene(128, 128, back, px.data()));
    c = CenterPixel(px.data(), 128, 128);
    EXPECT_TRUE(IsBlue(c)) << "center must be blue after camera move - got ("
                           << (int)c[0] << "," << (int)c[1] << "," << (int)c[2] << ")";
    EXPECT_GT(CountColor(px.data(), 128, 128, IsBlue), 1000);
    EXPECT_EQ(CountColor(px.data(), 128, 128, IsRed), 0)
        << "red cube must be occluded from behind";

    rhi->shutdown();
}

TEST(RHI, OpenGLBackend_SceneInstancing) {
    auto rhi = RHI::createRHI(RHI::GraphicsAPI::OpenGL);
    ASSERT_NE(rhi, nullptr);
    ASSERT_TRUE(rhi->initialize(64, 64, "rhi-gl-inst"));

    std::vector<unsigned char> px(128 * 128 * 4);
    const RHI::OffscreenScene scene = InstancedScene();
    ASSERT_TRUE(rhi->renderOffscreenScene(128, 128, scene, px.data()));

    // All four instances must actually be drawn (per-instance model matrices).
    EXPECT_GT(CountColor(px.data(), 128, 128, IsRed), 100);
    EXPECT_GT(CountColor(px.data(), 128, 128, IsGreen), 100);
    EXPECT_GT(CountColor(px.data(), 128, 128, IsBlue), 100);
    EXPECT_GT(CountColor(px.data(), 128, 128, IsYellow), 100);
    // Top-left corner stays clear - cubes do not reach the screen edge.
    const unsigned char* tl = px.data();
    EXPECT_LT(tl[0] + tl[1] + tl[2], 60) << "corner must be background";

    rhi->shutdown();
}

TEST(RHI, VulkanBackend_SceneDepthAndCamera) {
    auto rhi = RHI::createRHI(RHI::GraphicsAPI::Vulkan);
    ASSERT_NE(rhi, nullptr);
    if (!rhi->available()) GTEST_SKIP() << "no Vulkan on this machine";
    if (!rhi->initialize(64, 64, "rhi-vk-scene")) GTEST_SKIP() << "Vulkan unavailable";

    std::vector<unsigned char> px(128 * 128 * 4);

    const RHI::OffscreenScene front = DepthScene(false);
    ASSERT_TRUE(rhi->renderOffscreenScene(128, 128, front, px.data()));
    const unsigned char* c = CenterPixel(px.data(), 128, 128);
    EXPECT_TRUE(IsRed(c)) << "Vulkan: center must be red (near cube) - got ("
                          << (int)c[0] << "," << (int)c[1] << "," << (int)c[2] << ")";
    EXPECT_GT(CountColor(px.data(), 128, 128, IsRed), 1000);
    EXPECT_EQ(CountColor(px.data(), 128, 128, IsBlue), 0)
        << "Vulkan: far blue cube must be occluded (depth buffer)";

    const RHI::OffscreenScene back = DepthScene(true);
    ASSERT_TRUE(rhi->renderOffscreenScene(128, 128, back, px.data()));
    c = CenterPixel(px.data(), 128, 128);
    EXPECT_TRUE(IsBlue(c)) << "Vulkan: center must be blue after camera move";
    EXPECT_EQ(CountColor(px.data(), 128, 128, IsRed), 0);

    rhi->shutdown();
}

TEST(RHI, VulkanBackend_SceneInstancing) {
    auto rhi = RHI::createRHI(RHI::GraphicsAPI::Vulkan);
    ASSERT_NE(rhi, nullptr);
    if (!rhi->available()) GTEST_SKIP() << "no Vulkan on this machine";
    if (!rhi->initialize(64, 64, "rhi-vk-inst")) GTEST_SKIP() << "Vulkan unavailable";

    std::vector<unsigned char> px(128 * 128 * 4);
    const RHI::OffscreenScene scene = InstancedScene();
    ASSERT_TRUE(rhi->renderOffscreenScene(128, 128, scene, px.data()));

    EXPECT_GT(CountColor(px.data(), 128, 128, IsRed), 100);
    EXPECT_GT(CountColor(px.data(), 128, 128, IsGreen), 100);
    EXPECT_GT(CountColor(px.data(), 128, 128, IsBlue), 100);
    EXPECT_GT(CountColor(px.data(), 128, 128, IsYellow), 100);
    const unsigned char* tl = px.data();
    EXPECT_LT(tl[0] + tl[1] + tl[2], 60) << "corner must be background";

    rhi->shutdown();
}

TEST(RHI, GLAndVulkanRenderIdenticalScene) {
    // The strongest parity check: the SAME scene must produce the SAME image
    // on both backends (identical camera, depth ordering, instance placement).
    // The two backends are run SEQUENTIALLY (GL rendered + torn down, then
    // Vulkan): the engine itself only ever runs one backend per launch, and on
    // some driver stacks (radeonsi GL + RADV Vulkan sharing a virtio display)
    // keeping both contexts alive at once corrupts the GL context. The pixel
    // comparison is unaffected by the ordering.
    std::vector<unsigned char> glPx(128 * 128 * 4);
    {
        auto gl = RHI::createRHI(RHI::GraphicsAPI::OpenGL);
        ASSERT_NE(gl, nullptr);
        ASSERT_TRUE(gl->initialize(64, 64, "rhi-parity-gl"));
        const RHI::OffscreenScene scene = DepthScene(false);
        ASSERT_TRUE(gl->renderOffscreenScene(128, 128, scene, glPx.data()));
        gl->shutdown();
    }

    auto vk = RHI::createRHI(RHI::GraphicsAPI::Vulkan);
    ASSERT_NE(vk, nullptr);
    if (!vk->available() || !vk->initialize(64, 64, "rhi-parity-vk")) {
        GTEST_SKIP() << "no Vulkan on this machine";
    }
    std::vector<unsigned char> vkPx(128 * 128 * 4);
    const RHI::OffscreenScene scene = DepthScene(false);
    ASSERT_TRUE(vk->renderOffscreenScene(128, 128, scene, vkPx.data()));

    // Center pixel matches exactly.
    for (int ch = 0; ch < 3; ++ch) {
        EXPECT_NEAR(glPx[(128 / 2 * 128 + 128 / 2) * 4 + ch],
                    vkPx[(128 / 2 * 128 + 128 / 2) * 4 + ch], 8);
    }
    // Color region sizes match (silhouette rasterization may differ by a few
    // edge pixels, so compare counts with tolerance).
    EXPECT_NEAR(CountColor(glPx.data(), 128, 128, IsRed),
                CountColor(vkPx.data(), 128, 128, IsRed), 300);
    EXPECT_NEAR(CountColor(glPx.data(), 128, 128, IsBlue),
                CountColor(vkPx.data(), 128, 128, IsBlue), 300);

    vk->shutdown();
}

TEST(RHI, VulkanBackend_RenderFrameSceneToSwapchain) {
    // The Vulkan swapchain scene path: acquire -> render the scene into the
    // swapchain image (depth + instancing) -> present. Skipped on truly
    // headless boxes where the Vulkan surface cannot be created. (The GL
    // backend is deliberately NOT involved here: keeping a GL context alive
    // alongside Vulkan makes some driver stacks - radeonsi + RADV on a
    // shared virtio display - crash, including the validation layer.)
    auto vk = RHI::createRHI(RHI::GraphicsAPI::Vulkan);
    ASSERT_NE(vk, nullptr);
    if (!vk->initialize(64, 64, "rhi-vk-swapchain")) {
        GTEST_SKIP() << "Vulkan backend unavailable (no surface on this box)";
    }

    const RHI::OffscreenScene scene = DepthScene(false);
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(vk->beginFrame()) << "frame " << i << " acquire";
        ASSERT_TRUE(vk->renderFrameScene(scene)) << "frame " << i << " scene render";
        vk->endFrame();   // presents the rendered scene
    }

    vk->shutdown();
}

// =============================================================================
// FSR3 Phase 1: Halton(2,3) sub-pixel jitter + CameraUBOData staging.
// Pure-host math + layout — no GL/VK surface required, so it runs in this
// headless environment and pins the deterministic sequence the FSR3 dispatch
// (Phase 4) consumes.
// =============================================================================
TEST(HaltonJitter, SequenceIsDeterministicAndInRange) {
    using namespace RHI;
    // Frame 0 -> no jitter (stable first frame).
    EXPECT_FLOAT_EQ(halton23(0).x, 0.0f);
    EXPECT_FLOAT_EQ(halton23(0).y, 0.0f);
    // Canonical Halton(2,3) radical-inverse sequence.
    EXPECT_NEAR(halton23(1).x, 0.5f, 1e-6f); EXPECT_NEAR(halton23(1).y, 1.0f/3.0f, 1e-6f);
    EXPECT_NEAR(halton23(2).x, 0.25f, 1e-6f); EXPECT_NEAR(halton23(2).y, 2.0f/3.0f, 1e-6f);
    EXPECT_NEAR(halton23(3).x, 0.75f, 1e-6f); EXPECT_NEAR(halton23(3).y, 1.0f/9.0f, 1e-6f);
    EXPECT_NEAR(halton23(4).x, 0.125f, 1e-6f); EXPECT_NEAR(halton23(4).y, 4.0f/9.0f, 1e-6f);
    // 8 = (1000)_2 -> 1/16 ; 8 = (22)_3 -> 2/3 + 2/9 = 8/9.
    EXPECT_NEAR(halton23(8).x, 1.0f/16.0f, 1e-6f); EXPECT_NEAR(halton23(8).y, 8.0f/9.0f, 1e-6f);
    // Monotonicity / range invariants for a longer span.
    for (int i = 1; i < 64; ++i) {
        const Vec2 h = halton23(i);
        EXPECT_GE(h.x, 0.0f); EXPECT_LT(h.x, 1.0f);
        EXPECT_GE(h.y, 0.0f); EXPECT_LT(h.y, 1.0f);
    }
}

TEST(HaltonJitter, NdcJitterSpansHalfAPixel) {
    using namespace RHI;
    const int W = 64, H = 64;
    // Frame 0 is the FSR temporal-history reference: no jitter (stable).
    EXPECT_FLOAT_EQ(ndcJitter(0, W, H).x, 0.0f);
    EXPECT_FLOAT_EQ(ndcJitter(0, W, H).y, 0.0f);
    EXPECT_FLOAT_EQ(ndcJitter(-1, W, H).x, 0.0f);
    EXPECT_FLOAT_EQ(ndcJitter(-1, W, H).y, 0.0f);
    // (halton - 0.5)/dim maps [0,1) -> [-0.5/dim, +0.5/dim) in NDC per axis.
    for (int i = 1; i < 32; ++i) {
        const Vec2 n = ndcJitter(i, W, H);
        EXPECT_GE(n.x, -0.5f / W); EXPECT_LE(n.x, 0.5f / W);
        EXPECT_GE(n.y, -0.5f / H); EXPECT_LE(n.y, 0.5f / H);
    }
}

TEST(HaltonJitter, CameraUboLayoutIsStd140Compatible) {
    using namespace RHI;
    // Appended FSR3 history fields must not shift the original 96-byte block,
    // and the whole struct must land on a 16-byte std140 boundary so both the
    // GL inline and Vulkan SPIR-V CameraUBO blocks stay in sync with the CPU.
    static_assert(sizeof(CameraUBOData) == 176,
                  "CameraUBOData std140 size must be 96 base + 64 prevViewProj + 16 history");
    EXPECT_EQ(sizeof(CameraUBOData), 176u);
    EXPECT_EQ(__builtin_offsetof(CameraUBOData, viewProj), 0u);
    EXPECT_EQ(__builtin_offsetof(CameraUBOData, camPos), 64u);
    EXPECT_EQ(__builtin_offsetof(CameraUBOData, fogColor), 80u);
    EXPECT_EQ(__builtin_offsetof(CameraUBOData, pad), 92u);
    EXPECT_EQ(__builtin_offsetof(CameraUBOData, prevViewProj), 96u);
    EXPECT_EQ(__builtin_offsetof(CameraUBOData, jitterOffset), 160u);
}

TEST(RHI, OffscreenInstanceLayoutIsStd140Compatible) {
    using namespace RHI;
    // OffscreenInstance is the CPU<->GPU instance record shared by BOTH backends:
    //  - GL: uploaded raw into the instance VBO; model/color are read via
    //    glVertexAttribPointer at fixed byte offsets.
    //  - Vulkan: uploaded raw into the scalar-layout InstanceBuffer SSBO whose
    //    `struct Instance { mat4 model; vec4 color; mat4 prevModel; }`
    //    offsets/stride MUST equal sizeof(OffscreenInstance) per element or the
    //    GL<->Vulkan mesh/cube parity breaks.
    // Appending the Phase-3 prevModel field MUST NOT shift model(0)/color(64)
    // and must keep the 144-byte stride 16-aligned (scalar std140/scalar).
    // prevModel(@80) is the motion-vector source; in Phase 3A no shader reads
    // it (inert), so the offscreen parity scene renders byte-identical to before.
    static_assert(sizeof(OffscreenInstance) == 144,
                  "OffscreenInstance = 64 model + 16 color + 64 prevModel");
    EXPECT_EQ(sizeof(OffscreenInstance), 144u);
    EXPECT_EQ(__builtin_offsetof(OffscreenInstance, model),     0u);
    EXPECT_EQ(__builtin_offsetof(OffscreenInstance, color),    64u);
    EXPECT_EQ(__builtin_offsetof(OffscreenInstance, prevModel), 80u);
    EXPECT_EQ(0u  % 16u, 0u); EXPECT_EQ(64u % 16u, 0u);
    EXPECT_EQ(80u % 16u, 0u); EXPECT_EQ(144u % 16u, 0u);
}

TEST(RHI, VulkanBackend_RendersImGuiOverScene) {
    // The editor UI on Vulkan: a Dear ImGui overlay rendered on top of the
    // scene in the same swapchain image, then presented. Exercises the whole
    // chain - imgui_impl_vulkan init, the LOAD-over-scene render pass, and
    // the presentable-layout handoff.
    auto vk = RHI::createRHI(RHI::GraphicsAPI::Vulkan);
    ASSERT_NE(vk, nullptr);
    if (!vk->initialize(64, 64, "rhi-vk-imgui")) GTEST_SKIP() << "Vulkan unavailable";

    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    // The test has no imgui_impl_glfw to set the display size - do it here
    // (the editor path gets it from imgui_impl_glfw each frame).
    ImGui::GetIO().DisplaySize = ImVec2(64, 64);
    ASSERT_TRUE(vk->initializeImGui());

    const RHI::OffscreenScene scene = DepthScene(false);
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(vk->beginFrame());
        ASSERT_TRUE(vk->renderFrameScene(scene));
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();
        ImGui::Text("Vulkan overlay - frame %d", i);
        ImGui::Render();
        ASSERT_TRUE(vk->renderFrameImGui(ImGui::GetDrawData()));
        vk->endFrame();
    }

    vk->shutdown();
    ImGui::DestroyContext();
}

TEST(RHI, VulkanBackend_RenderFrameSceneHandlesNoSwapchain) {
    // Device-only mode (no WSI surface): renderFrameScene must be a graceful
    // no-op (false, no crash) rather than a device error. On machines WITH a
    // display the swapchain exists and the scene renders normally - both
    // branches are valid and exercised depending on the box.
    auto vk = RHI::createRHI(RHI::GraphicsAPI::Vulkan);
    ASSERT_NE(vk, nullptr);
    if (!vk->available()) GTEST_SKIP() << "no Vulkan on this machine";
    if (!vk->initialize(64, 64, "rhi-vk-headless")) GTEST_SKIP() << "Vulkan unavailable";

    const RHI::OffscreenScene scene = DepthScene(false);
    if (vk->beginFrame()) {
        // WSI available: the scene renders into the swapchain image.
        EXPECT_TRUE(vk->renderFrameScene(scene));
        vk->endFrame();
    } else {
        // No swapchain: graceful no-op, no crash.
        EXPECT_FALSE(vk->renderFrameScene(scene));
    }

    vk->shutdown();
}

// ---------------------------------------------------------------------------
// Generic mesh tests (interleaved pos/normal/uv + indices, drawn instanced)
// ---------------------------------------------------------------------------
// A unit quad in the XY plane facing +Z (normal (0,0,1)), 8 floats per vertex
// (pos.xyz, normal.xyz, uv). The lambert light dir (0.4,0.8,0.3) is above and
// to the side, so the quad's color is dimmed by the dot product.
RHI::OffscreenMesh MakeQuadMesh(const std::vector<RHI::OffscreenInstance>& instances) {
    RHI::OffscreenMesh mesh;
    // pos.xyz        normal.xyz    uv
    const float v[4][8] = {
        {-0.5f, -0.5f, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f},
        { 0.5f, -0.5f, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f},
        { 0.5f,  0.5f, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 1.0f},
        {-0.5f,  0.5f, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 1.0f},
    };
    for (const auto& row : v) {
        for (int i = 0; i < 8; ++i) mesh.vertices.push_back(row[i]);
    }
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.instances = instances;
    return mesh;
}

// Camera looking down -Z at a quad placed at (x,z) with scale s and color.
RHI::OffscreenScene QuadScene(float x, float z, float s, float r, float g, float b) {
    RHI::OffscreenScene scene;
    scene.camera.eye[0] = 0; scene.camera.eye[1] = 0; scene.camera.eye[2] = 0;
    scene.camera.target[0] = 0; scene.camera.target[1] = 0; scene.camera.target[2] = -4.0f;
    scene.camera.fovDeg = 60.0f;
    RHI::OffscreenInstance inst{};
    const RHI::Mat4 m = RHI::mat4TranslateScale(x, 0.0f, z, s);
    std::memcpy(inst.model, m.m, sizeof(m.m));
    inst.color[0] = r; inst.color[1] = g; inst.color[2] = b; inst.color[3] = 1.0f;
    scene.meshes.push_back(MakeQuadMesh({inst}));
    return scene;
}

TEST(RHI, OpenGLBackend_RendersGenericMesh) {
    auto rhi = RHI::createRHI(RHI::GraphicsAPI::OpenGL);
    ASSERT_NE(rhi, nullptr);
    ASSERT_TRUE(rhi->initialize(64, 64, "rhi-gl-mesh"));

    std::vector<unsigned char> px(128 * 128 * 4);
    // A red quad at z=-4, scale 3 -> fills the center of the frame.
    const RHI::OffscreenScene scene = QuadScene(0.0f, -4.0f, 3.0f, 1.0f, 0.1f, 0.1f);
    ASSERT_TRUE(rhi->renderOffscreenScene(128, 128, scene, px.data()));

    const unsigned char* c = CenterPixel(px.data(), 128, 128);
    EXPECT_TRUE(IsRed(c)) << "GL mesh: center must be the red quad - got ("
                          << (int)c[0] << "," << (int)c[1] << "," << (int)c[2] << ")";
    // The quad's silhouette must cover a large center region.
    EXPECT_GT(CountColor(px.data(), 128, 128, IsRed), 4000);
    // Corner stays the clear color (quad does not reach the edge).
    const unsigned char* tl = px.data();
    EXPECT_LT(tl[0] + tl[1] + tl[2], 60) << "corner must be background";

    rhi->shutdown();
}

TEST(RHI, VulkanBackend_RendersGenericMesh) {
    auto rhi = RHI::createRHI(RHI::GraphicsAPI::Vulkan);
    ASSERT_NE(rhi, nullptr);
    if (!rhi->available()) GTEST_SKIP() << "no Vulkan on this machine";
    if (!rhi->initialize(64, 64, "rhi-vk-mesh")) GTEST_SKIP() << "Vulkan unavailable";

    std::vector<unsigned char> px(128 * 128 * 4);
    const RHI::OffscreenScene scene = QuadScene(0.0f, -4.0f, 3.0f, 1.0f, 0.1f, 0.1f);
    ASSERT_TRUE(rhi->renderOffscreenScene(128, 128, scene, px.data()));

    const unsigned char* c = CenterPixel(px.data(), 128, 128);
    EXPECT_TRUE(IsRed(c)) << "Vulkan mesh: center must be the red quad - got ("
                          << (int)c[0] << "," << (int)c[1] << "," << (int)c[2] << ")";
    EXPECT_GT(CountColor(px.data(), 128, 128, IsRed), 4000);
    const unsigned char* tl = px.data();
    EXPECT_LT(tl[0] + tl[1] + tl[2], 60) << "corner must be background";

    rhi->shutdown();
}

TEST(RHI, MeshInstancingAndParity) {
    // Two instances of the SAME quad geometry at different positions: proves
    // per-mesh instancing (one vertex/index buffer, N draws). Then the GL and
    // Vulkan backends must render the scene to matching pixels (same camera,
    // geometry, lambert math).
    RHI::OffscreenScene scene;
    scene.camera.eye[0] = 0; scene.camera.eye[1] = 0; scene.camera.eye[2] = 0;
    scene.camera.target[0] = 0; scene.camera.target[1] = 0; scene.camera.target[2] = -4.0f;
    scene.camera.fovDeg = 60.0f;
    RHI::OffscreenInstance left{}, right{};
    const RHI::Mat4 ml = RHI::mat4TranslateScale(-1.6f, 0.0f, -4.0f, 2.0f);
    const RHI::Mat4 mr = RHI::mat4TranslateScale( 1.6f, 0.0f, -4.0f, 2.0f);
    std::memcpy(left.model, ml.m, sizeof(ml.m));
    std::memcpy(right.model, mr.m, sizeof(mr.m));
    left.color[0] = 1.0f; left.color[1] = 0.1f; left.color[2] = 0.1f; left.color[3] = 1.0f;
    right.color[0] = 0.1f; right.color[1] = 0.1f; right.color[2] = 1.0f; right.color[3] = 1.0f;
    scene.meshes.push_back(MakeQuadMesh({left, right}));

    std::vector<unsigned char> glPx(128 * 128 * 4);
    {
        auto gl = RHI::createRHI(RHI::GraphicsAPI::OpenGL);
        ASSERT_NE(gl, nullptr);
        ASSERT_TRUE(gl->initialize(64, 64, "rhi-mesh-parity-gl"));
        ASSERT_TRUE(gl->renderOffscreenScene(128, 128, scene, glPx.data()));
        gl->shutdown();
    }
    // Both instances drawn: left red + right blue regions.
    EXPECT_GT(CountColor(glPx.data(), 128, 128, IsRed), 500);
    EXPECT_GT(CountColor(glPx.data(), 128, 128, IsBlue), 500);

    auto vk = RHI::createRHI(RHI::GraphicsAPI::Vulkan);
    ASSERT_NE(vk, nullptr);
    if (!vk->available() || !vk->initialize(64, 64, "rhi-mesh-parity-vk")) {
        GTEST_SKIP() << "no Vulkan on this machine";
    }
    std::vector<unsigned char> vkPx(128 * 128 * 4);
    ASSERT_TRUE(vk->renderOffscreenScene(128, 128, scene, vkPx.data()));

    EXPECT_GT(CountColor(vkPx.data(), 128, 128, IsRed), 500);
    EXPECT_GT(CountColor(vkPx.data(), 128, 128, IsBlue), 500);
    // Center pixel parity between the two backends.
    for (int ch = 0; ch < 3; ++ch) {
        EXPECT_NEAR(glPx[(128 / 2 * 128 + 128 / 2) * 4 + ch],
                    vkPx[(128 / 2 * 128 + 128 / 2) * 4 + ch], 8);
    }
    EXPECT_NEAR(CountColor(glPx.data(), 128, 128, IsRed),
                CountColor(vkPx.data(), 128, 128, IsRed), 100);
    EXPECT_NEAR(CountColor(glPx.data(), 128, 128, IsBlue),
                CountColor(vkPx.data(), 128, 128, IsBlue), 100);

    vk->shutdown();
}

TEST(RHI, TexturedMeshRendersAndParity) {
    // A 4x4 black/white checkerboard on a WHITE quad: proves the albedo
    // texture is actually sampled on both backends (dark AND light cells in
    // the quad region - a flat color would have only one), and that GL and
    // Vulkan render the SAME pixels (both upload the same bytes and sample
    // the same UV convention, so parity holds). The 1x1 white fallback keeps
    // untextured meshes pixel-identical, which the older mesh tests already
    // assert.
    RHI::OffscreenMesh mesh = MakeQuadMesh({});
    RHI::OffscreenInstance inst{};
    const RHI::Mat4 m = RHI::mat4TranslateScale(0.0f, 0.0f, -4.0f, 3.0f);
    std::memcpy(inst.model, m.m, sizeof(m.m));
    inst.color[0] = inst.color[1] = inst.color[2] = inst.color[3] = 1.0f;
    mesh.instances.push_back(inst);
    mesh.textureWidth = 4;
    mesh.textureHeight = 4;
    mesh.texturePixels.resize(4 * 4 * 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const unsigned char v = ((x + y) % 2 == 0) ? 255u : 0u;
            unsigned char* p = mesh.texturePixels.data() + (y * 4 + x) * 4;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    RHI::OffscreenScene scene;
    scene.camera.eye[0] = 0; scene.camera.eye[1] = 0; scene.camera.eye[2] = 0;
    scene.camera.target[0] = 0; scene.camera.target[1] = 0; scene.camera.target[2] = -4.0f;
    scene.camera.fovDeg = 60.0f;
    scene.meshes.push_back(mesh);

    // The lambert dim factor on a +Z normal is ~0.66, so light cells render
    // ~168/channel and dark cells ~0 - separated by generous margins.
    auto isDark = [](const unsigned char* p) { return p[0] < 60 && p[1] < 60 && p[2] < 60; };
    auto isLight = [](const unsigned char* p) { return p[0] > 120 && p[1] > 120 && p[2] > 120; };

    std::vector<unsigned char> glPx(128 * 128 * 4);
    {
        auto gl = RHI::createRHI(RHI::GraphicsAPI::OpenGL);
        ASSERT_NE(gl, nullptr);
        ASSERT_TRUE(gl->initialize(64, 64, "rhi-tex-parity-gl"));
        ASSERT_TRUE(gl->renderOffscreenScene(128, 128, scene, glPx.data()));
        gl->shutdown();
    }
    const int glDark = CountColor(glPx.data(), 128, 128, isDark);
    const int glLight = CountColor(glPx.data(), 128, 128, isLight);
    EXPECT_GT(glDark, 200) << "GL textured quad must contain dark checker cells";
    EXPECT_GT(glLight, 200) << "GL textured quad must contain light checker cells";

    auto vk = RHI::createRHI(RHI::GraphicsAPI::Vulkan);
    ASSERT_NE(vk, nullptr);
    if (!vk->available() || !vk->initialize(64, 64, "rhi-tex-parity-vk")) {
        GTEST_SKIP() << "no Vulkan on this machine";
    }
    std::vector<unsigned char> vkPx(128 * 128 * 4);
    ASSERT_TRUE(vk->renderOffscreenScene(128, 128, scene, vkPx.data()));
    EXPECT_GT(CountColor(vkPx.data(), 128, 128, isDark), 200);
    EXPECT_GT(CountColor(vkPx.data(), 128, 128, isLight), 200);

    // Center pixel + checker-region parity between the two backends.
    for (int ch = 0; ch < 3; ++ch) {
        EXPECT_NEAR(glPx[(64 * 128 + 64) * 4 + ch], vkPx[(64 * 128 + 64) * 4 + ch], 8);
    }
    EXPECT_NEAR(glDark, CountColor(vkPx.data(), 128, 128, isDark), 150);
    EXPECT_NEAR(glLight, CountColor(vkPx.data(), 128, 128, isLight), 150);

    vk->shutdown();
}

// ---------------------------------------------------------------------------
// PBR texture path parity (Cook-Torrance + ACES + gamma on both backends)
// ---------------------------------------------------------------------------
// When OffscreenScene::pbrEnabled is set, generic meshes render through the
// PBR shaders (pbr_mesh.* on Vulkan, the GL PBR port) instead of the lambert
// path. The shader math is bit-identical on both backends (same light dir/
// color, GGX D, Smith G, Fresnel-Schlick, ACESFilm, pow(1/2.2), exp-squared
// fog) and they share the same CameraUBO + per-mesh albedo texture, so GL and
// Vulkan must produce matching pixels. This test proves (a) PBR actually runs
// (a specular highlight beats the lambert reference), (b) the albedo texture
// is sampled under PBR (both dark and light checker cells appear), and (c) GL
// and Vulkan PBR pixels match within float tolerance.
TEST(RHI, PBRMeshParity) {
    // 4x4 black/white checkerboard on a quad facing +Z (normal (0,0,1)).
    RHI::OffscreenMesh mesh = MakeQuadMesh({});
    RHI::OffscreenInstance inst{};
    const RHI::Mat4 m = RHI::mat4TranslateScale(0.0f, 0.0f, -4.0f, 3.0f);
    std::memcpy(inst.model, m.m, sizeof(m.m));
    inst.color[0] = inst.color[1] = inst.color[2] = inst.color[3] = 1.0f;
    mesh.instances.push_back(inst);
    mesh.textureWidth = 4; mesh.textureHeight = 4;
    mesh.texturePixels.resize(4 * 4 * 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const unsigned char v = ((x + y) % 2 == 0) ? 255u : 0u;
            unsigned char* p = mesh.texturePixels.data() + ((size_t)y * 4 + x) * 4;
            p[0] = p[1] = p[2] = v; p[3] = 255;
        }
    RHI::OffscreenScene scene;
    scene.camera.eye[0] = 0; scene.camera.eye[1] = 0; scene.camera.eye[2] = 0;
    scene.camera.target[0] = 0; scene.camera.target[1] = 0; scene.camera.target[2] = -4.0f;
    scene.camera.fovDeg = 60.0f;
    scene.meshes.push_back(mesh);
    scene.pbrEnabled = true;   // switch the mesh path to PBR

    auto isDark = [](const unsigned char* p) { return p[0] < 60 && p[1] < 60 && p[2] < 60; };
    auto Lum = [](const unsigned char* p) { return (p[0] + p[1] + p[2]) / 3; };

    // GL with PBR ON.
    std::vector<unsigned char> glPbr(128 * 128 * 4);
    {
        auto gl = RHI::createRHI(RHI::GraphicsAPI::OpenGL);
        ASSERT_NE(gl, nullptr);
        ASSERT_TRUE(gl->initialize(64, 64, "rhi-pbr-parity-gl"));
        ASSERT_TRUE(gl->renderOffscreenScene(128, 128, scene, glPbr.data()));
        gl->shutdown();
    }
    // GL with PBR OFF - the lambert reference (same scene, same texture).
    std::vector<unsigned char> glLambert(128 * 128 * 4);
    {
        scene.pbrEnabled = false;
        auto gl = RHI::createRHI(RHI::GraphicsAPI::OpenGL);
        ASSERT_NE(gl, nullptr);
        ASSERT_TRUE(gl->initialize(64, 64, "rhi-pbr-parity-gl-lambert"));
        ASSERT_TRUE(gl->renderOffscreenScene(128, 128, scene, glLambert.data()));
        gl->shutdown();
    }

    // (a) PBR is wired: the center texel samples a gray 50% (bilinear mix of
    // the checkerboard). PBR (Cook-Torrance + ACES + gamma) and lambert are
    // DIFFERENT shaders, so the same pixel must NOT match the lambert output —
    // ACES/gamma compress mid-gray under PBR, producing a measurably different
    // value. Equality here would mean the PBR program was skipped/fell back.
    const unsigned char* glPbrCenter = CenterPixel(glPbr.data(), 128, 128);
    const unsigned char* glLamCenter = CenterPixel(glLambert.data(), 128, 128);
    {
        int d = (int)Lum(glPbrCenter) - (int)Lum(glLamCenter);
        if (d < 0) d = -d;
        EXPECT_GT(d, 20)
            << "PBR and lambert must diverge on the same pixel (PBR not wired?):"
            << " PBR=" << (int)Lum(glPbrCenter)
            << " lambert=" << (int)Lum(glLamCenter);
    }

    // (b) The albedo texture is actually sampled under PBR: the black checker
    // cells get only ambient IBL (~0) so they render dark, while white cells
    // render markedly brighter. A flat/single-color shader would have NO dark
    // cells (lambert-on-white is ~167 everywhere).
    EXPECT_GT(CountColor(glPbr.data(), 128, 128, isDark), 200)
        << "PBR must sample the albedo texture (dark checker cells should appear)";

    // (c) Vulkan renders the SAME PBR scene.
    auto vk = RHI::createRHI(RHI::GraphicsAPI::Vulkan);
    ASSERT_NE(vk, nullptr);
    if (!vk->available() || !vk->initialize(64, 64, "rhi-pbr-parity-vk")) {
        GTEST_SKIP() << "no Vulkan on this machine";
    }
    scene.pbrEnabled = true;
    std::vector<unsigned char> vkPbr(128 * 128 * 4);
    ASSERT_TRUE(vk->renderOffscreenScene(128, 128, scene, vkPbr.data()));

    // Center pixel parity between GL and Vulkan PBR.
    const unsigned char* vkPbrCenter = CenterPixel(vkPbr.data(), 128, 128);
    for (int ch = 0; ch < 3; ++ch) {
        EXPECT_NEAR(glPbrCenter[ch], vkPbrCenter[ch], 10)
            << "PBR channel " << ch << " divergence:"
            << " GL=" << (int)glPbrCenter[ch]
            << " VK=" << (int)vkPbrCenter[ch];
    }
    // Region parity: GL and Vulkan PBR must show the same checkerboard
    // structure (dark-cell counts match) — proves both backends sample the
    // SAME texture bytes under the SAME PBR math. (Relative count, robust to
    // silhouette edge differences.)
    EXPECT_NEAR(CountColor(glPbr.data(), 128, 128, isDark),
                CountColor(vkPbr.data(), 128, 128, isDark), 150);

    vk->shutdown();
}

TEST(RHI, VulkanBackend_InitializesDeviceSwapchainAndPresents) {
    auto rhi = RHI::createRHI(RHI::GraphicsAPI::Vulkan);
    ASSERT_NE(rhi, nullptr);
    if (!rhi->available()) {
        GTEST_SKIP() << "no Vulkan loader/driver on this machine";
    }
    if (!rhi->initialize(64, 64, "rhi-vk-test")) {
        GTEST_SKIP() << "Vulkan backend unavailable (no loader/driver)";
    }
    // Headless is FINE here: the device is created device-only and the
    // offscreen render works without any surface/swapchain.
    EXPECT_TRUE(rhi->available());

    std::vector<unsigned char> px(64 * 64 * 4);
    ASSERT_TRUE(rhi->renderOffscreenTriangle(64, 64, px.data()))
        << "Vulkan offscreen render must produce pixels (SPIR-V pipeline, "
           "buffers, render pass, submit, readback)";
    EXPECT_TRUE(VerifyTrianglePixels(px.data(), 64, 64))
        << "Vulkan must render the same triangle as OpenGL (center green, "
           "corner clear)";

    rhi->shutdown();
}

TEST(RHI, VelocityAttachmentZeroStaticAndNonZeroMoved) {
    // Phase 3b: the offscreen RG32f velocity attachment must be ~0 on a STATIC
    // mesh (prevModel == model -> currNDC == prevNDC) and clearly non-zero
    // when the previous frame moved the mesh (prevModel != model). The velocity
    // math is identical on GL/Vulkan (viewProj * prevModel * pos, then
    // currNDC - prevNDC), so the GL and VK velocity images must agree to a tight
    // tolerance.
    //
    // Each render uses a FRESH backend so it lands on frame 0, where the Halton
    // jitter is {0,0} on BOTH APIs, making velocity pure object motion and the
    // GL<->VK images directly comparable. (VK offscreen now advances its jitter
    // via m_offscreenFrame; that is validated at the end of this test.)
    // A single quad faces -Z, viewed from (0,0,2); model = identity places it
    // at the origin (filling the viewport). prevModel is the motion knob:
    //   static -> prevModel == model (identity) -> velocity == 0
    //   moved  -> prevModel == translate(-2,0,0) -> screen-space motion
    // One quad facing -Z, viewed from (0,0,2). model = identity (quad at the
    // origin, filling the viewport). prevModel is the motion knob:
    //   static -> prevModel == model (identity) -> velocity == 0
    //   moved  -> prevModel == translate(-2,0,0) -> screen-space motion
    auto buildVelScene = [](bool moved) -> RHI::OffscreenScene {
        RHI::OffscreenScene s;
        s.camera.eye[2] = 2.0f; s.camera.target[2] = 0.0f;
        s.camera.fovDeg = 60.0f; s.camera.farPlane = 100.0f;
        RHI::OffscreenMesh quad;
        const float verts[32] = {
            -1,-1,0,  0,0,-1,  0,0,
             1,-1,0,  0,0,-1,  1,0,
             1, 1,0,  0,0,-1,  1,1,
            -1, 1,0,  0,0,-1,  0,1
        };
        quad.vertices.assign(verts, verts + 32);
        quad.indices = { 0u, 1u, 2u, 2u, 3u, 0u };
        quad.texturePixels = { 255, 255, 255, 255 };
        quad.textureWidth = 1; quad.textureHeight = 1;
        RHI::OffscreenInstance inst{};
        const RHI::Mat4 ident{};
        std::memcpy(inst.model, ident.m, sizeof(ident.m));
        inst.color[0] = inst.color[1] = inst.color[2] = 1.0f;
        inst.color[3] = 1.0f;
        if (moved) {
            const RHI::Mat4 prev = RHI::mat4TranslateScale(-2.0f, 0.0f, 0.0f, 1.0f);
            std::memcpy(inst.prevModel, prev.m, sizeof(prev.m));
        } else {
            std::memcpy(inst.prevModel, inst.model, sizeof(inst.model));
        }
        quad.instances.push_back(inst);
        s.meshes.push_back(quad);
        return s;
    };

    // Render the velocity attachment into a FRESH backend (frame 0 -> Halton
    // jitter {0,0} on both APIs) so velocity is pure object motion and the
    // GL<->VK images are directly comparable.
    auto renderVel = [buildVelScene](RHI::GraphicsAPI api, bool moved) -> std::vector<float> {
        auto rhi = RHI::createRHI(api);
        if (!rhi || !rhi->initialize(64, 64, "rhi-vel")) return {};
        std::vector<float> vel(64 * 64 * 2, 7.0f);  // 7.0 sentinel -> unwritten = failure
        rhi->renderOffscreenScene(64, 64, buildVelScene(moved),
                                  /*outRGBA*/nullptr, vel.data());
        rhi->shutdown();
        return vel;
    };

    // STATIC on both backends: velocity must be ~0 (jitter is 0 at frame 0).
    for (RHI::GraphicsAPI api : { RHI::GraphicsAPI::OpenGL, RHI::GraphicsAPI::Vulkan }) {
        const std::vector<float> vel = renderVel(api, /*moved=*/false);
        ASSERT_FALSE(vel.empty()) << "backend " << RHI::graphicsApiName(api)
            << " unavailable; cannot render velocity";
        float maxStatic = 0.0f;
        for (float f : vel) { float a = fabsf(f); if (a > maxStatic) maxStatic = a; }
        EXPECT_LE(maxStatic, 1e-4f) << "api=" << RHI::graphicsApiName(api)
            << " static (prevModel==model) velocity must be ~0";
    }

    // MOVED on both backends: velocity must be clearly non-zero (pure x-motions
    // ~1.7 NDC units).
    const std::vector<float> glVel = renderVel(RHI::GraphicsAPI::OpenGL, /*moved=*/true);
    const std::vector<float> vkVel = renderVel(RHI::GraphicsAPI::Vulkan, /*moved=*/true);
    ASSERT_FALSE(glVel.empty()) << "OpenGL backend unavailable; cannot render velocity";
    ASSERT_FALSE(vkVel.empty()) << "Vulkan backend unavailable; cannot render velocity";

    float maxMovedGL = 0.0f, maxMovedVK = 0.0f;
    for (float f : glVel) { float a = fabsf(f); if (a > maxMovedGL) maxMovedGL = a; }
    for (float f : vkVel) { float a = fabsf(f); if (a > maxMovedVK) maxMovedVK = a; }
    EXPECT_GT(maxMovedGL, 0.05f) << "OpenGL moved mesh must produce non-zero velocity";
    EXPECT_GT(maxMovedVK, 0.05f) << "Vulkan moved mesh must produce non-zero velocity";

    // GL<->Vulkan velocity parity: identical motion-vector math -> identical
    // velocity image. Both rendered at frame 0 (jitter {0,0}).
    float maxDiff = 0.0f;
    for (size_t i = 0; i < glVel.size() && i < vkVel.size(); ++i) {
        const float d = fabsf(glVel[i] - vkVel[i]);
        if (d > maxDiff) maxDiff = d;
    }
    EXPECT_LE(maxDiff, 1e-3f) << "GL and Vulkan velocity images must match";

    // Fix-validation ("fix vk for jitter"): Vulkan offscreen rendering used to
    // never advance its frame counter (m_frame is the swapchain image index,
    // which must not be bumped from uploadSceneUBO), freezing the Halton jitter
    // at {0,0}. Render the moved scene TWICE on ONE fresh Vulkan RHI: frame 0
    // (jitter {0,0}) then frame 1 (jitter ndcJitter(1,64,64) != {0,0}). The
    // velocity images MUST differ, proving m_offscreenFrame now drives the
    // offscreen jitter sequence.
    {
        auto rhi = RHI::createRHI(RHI::GraphicsAPI::Vulkan);
        ASSERT_NE(rhi, nullptr);
        ASSERT_TRUE(rhi->initialize(64, 64, "rhi-jitter")) << "Vulkan unavailable; cannot advance jitter";
        const RHI::OffscreenScene s = buildVelScene(true);
        std::vector<float> v0(64 * 64 * 2), v1(64 * 64 * 2);
        rhi->renderOffscreenScene(64, 64, s, nullptr, v0.data());  // frame 0 (jitter {0,0})
        rhi->renderOffscreenScene(64, 64, s, nullptr, v1.data());  // frame 1 (jitter advances)
        rhi->shutdown();
        float jitterDelta = 0.0f;
        for (size_t i = 0; i < v0.size() && i < v1.size(); ++i) {
            const float d = fabsf(v0[i] - v1[i]);
            if (d > jitterDelta) jitterDelta = d;
        }
        EXPECT_GT(jitterDelta, 1e-4f) << "Vulkan offscreen jitter must advance "
            "(frame 1 velocity must differ from frame 0)";
    }
}

TEST(RHI, Fsr3EasuUpscalesLowResScene) {
    // Phase 4: AMD FSR3 EASU (edge-directed spatial upscale) on Vulkan, headless.
    // The scene is rendered at HALF the output resolution into a low-res
    // G-buffer, then AMD's canonical EASU compute shader (reusing the SDK's
    // fsr1 EASU math, compiled by glslc — no SDK CMake needed) upscales it to
    // (OUT_W x OUT_H). No swapchain/surface is required: this is the
    // headless-verifiable slice of FSR3. GL EASU (no compute shaders in 3.3) is
    // deferred to Phase 5, so this test is Vulkan-only.
    auto rhi = RHI::createRHI(RHI::GraphicsAPI::Vulkan);
    if (!rhi || !rhi->available())
        GTEST_SKIP() << "no Vulkan on this machine; cannot test EASU";
    ASSERT_TRUE(rhi->initialize(64, 64, "rhi-fsr3-easu")) << "Vulkan unavailable; cannot test EASU";

    // A single viewport-filling quad textured with a 2x1 red|blue ramp. At low
    // res the viewport shows a red->purple->blue vertical split; EASU must
    // upscale that split faithfully. (Model = identity fills the viewport.)
    RHI::OffscreenScene scene;
    scene.camera.eye[2] = 2.0f;            // camera at +2 looking at the origin (quad at z=0)
    scene.camera.target[2] = 0.0f;
    scene.camera.fovDeg = 60.0f;
    scene.camera.farPlane = 100.0f;
    scene.clearColor[0] = scene.clearColor[1] = scene.clearColor[2] = 0.0f;
    scene.clearColor[3] = 1.0f;
    RHI::OffscreenMesh quad;
    const float verts[32] = {
        -1,-1,0,  0,0,-1,  0,0,
         1,-1,0,  0,0,-1,  1,0,
         1, 1,0,  0,0,-1,  1,1,
        -1, 1,0,  0,0,-1,  0,1
    };
    quad.vertices.assign(verts, verts + 32);
    quad.indices = { 0u, 1u, 2u, 2u, 3u, 0u };
    quad.texturePixels = { 255, 0, 0, 255,   0, 0, 255, 255 }; // left=red right=blue
    quad.textureWidth = 2; quad.textureHeight = 1;
    RHI::OffscreenInstance inst{};
    const RHI::Mat4 ident{};
    std::memcpy(inst.model, ident.m, sizeof(ident.m));
    inst.color[0] = inst.color[1] = inst.color[2] = inst.color[3] = 1.0f;
    quad.instances.push_back(inst);
    scene.meshes.push_back(quad);

    constexpr int OUT_W = 64, OUT_H = 64;   // low-res render = 32x32 -> EASU -> 64x64

    std::vector<unsigned char> px(OUT_W * OUT_H * 4, 0xAA);
    ASSERT_TRUE(rhi->renderOffscreenSceneEasu(OUT_W, OUT_H, scene, px.data()))
        << "FSR3 EASU dispatch must produce an upscaled frame";

    // A fully-still 0xAA sentinel means EASU never wrote — it must have read
    // the low-res buffer and produced real upscaled content.
    int nonzero = 0;
    for (unsigned char c : px) if (c != 0xAA) { nonzero = 1; break; }
    ASSERT_TRUE(nonzero) << "EASU output buffer untouched (dispatcher no-op?)";

    // EASU preserved the red|blue vertical split: left half red-dominant,
    // right half blue-dominant. A clear or a broken/1:1 pass would not show
    // this structure at the 64x64 output resolution.
    int leftR = 0, leftB = 0, rightR = 0, rightB = 0;
    for (int y = 0; y < OUT_H; ++y) {
        for (int x = 0; x < OUT_W; ++x) {
            const int i = (y * OUT_W + x) * 4;
            if (x < OUT_W / 2) { leftR += px[i]; leftB += px[i + 2]; }
            else               { rightR += px[i]; rightB += px[i + 2]; }
        }
    }
    EXPECT_GT(leftR, leftB)
        << "EASU must keep the left half red-dominant (red=" << leftR << ", blue=" << leftB << ")";
    EXPECT_GT(rightB, rightR)
        << "EASU must keep the right half blue-dominant (blue=" << rightB << ", red=" << rightR << ")";

    rhi->shutdown();
}

TEST(RHI, Fsr3EasuGlUpscalesLowResScene) {
    // Phase 5b: FSR3 EASU on the OpenGL backend. GL 3.3 has no compute shaders,
    // so the upscale is a fullscreen-fragment pass that ports AMD's canonical
    // ffxFsrEasuFloat 12-tap edge-directed filter. This is the GL parity twin of
    // Fsr3EasuUpscalesLowResScene: same red|blue split scene, same assertions.
    auto rhi = RHI::createRHI(RHI::GraphicsAPI::OpenGL);
    if (!rhi || !rhi->available())
        GTEST_SKIP() << "no OpenGL on this machine; cannot test GL EASU";
    ASSERT_TRUE(rhi->initialize(64, 64, "rhi-fsr3-easu-gl")) << "GL context unavailable";

    RHI::OffscreenScene scene;
    scene.camera.eye[2] = 2.0f;            // camera at +2 looking at origin (quad at z=0)
    scene.camera.target[2] = 0.0f;
    scene.camera.fovDeg = 60.0f;
    scene.camera.farPlane = 100.0f;
    scene.clearColor[0] = scene.clearColor[1] = scene.clearColor[2] = 0.0f;
    scene.clearColor[3] = 1.0f;
    RHI::OffscreenMesh quad;
    const float verts[32] = {
        -1,-1,0,  0,0,-1,  0,0,
         1,-1,0,  0,0,-1,  1,0,
         1, 1,0,  0,0,-1,  1,1,
        -1, 1,0,  0,0,-1,  0,1
    };
    quad.vertices.assign(verts, verts + 32);
    quad.indices = { 0u, 1u, 2u, 2u, 3u, 0u };
    quad.texturePixels = { 255, 0, 0, 255,   0, 0, 255, 255 }; // left=red right=blue
    quad.textureWidth = 2; quad.textureHeight = 1;
    RHI::OffscreenInstance inst{};
    const RHI::Mat4 ident{};
    std::memcpy(inst.model, ident.m, sizeof(ident.m));
    inst.color[0] = inst.color[1] = inst.color[2] = inst.color[3] = 1.0f;
    quad.instances.push_back(inst);
    scene.meshes.push_back(quad);

    constexpr int OUT_W = 64, OUT_H = 64;   // low-res render = 32x32 -> EASU -> 64x64

    std::vector<unsigned char> px(OUT_W * OUT_H * 4, 0xAA);
    ASSERT_TRUE(rhi->renderOffscreenSceneEasu(OUT_W, OUT_H, scene, px.data()))
        << "GL EASU pass must produce an upscaled frame";

    // Untouched 0xAA sentinel => the EASU pass never wrote.
    int nonzero = 0;
    for (unsigned char c : px) if (c != 0xAA) { nonzero = 1; break; }
    ASSERT_TRUE(nonzero) << "GL EASU output buffer untouched";

    // EASU preserved the red|blue vertical split at the 64x64 output.
    int leftR = 0, leftB = 0, rightR = 0, rightB = 0;
    for (int y = 0; y < OUT_H; ++y) {
        for (int x = 0; x < OUT_W; ++x) {
            const int i = (y * OUT_W + x) * 4;
            if (x < OUT_W / 2) { leftR += px[i]; leftB += px[i + 2]; }
            else               { rightR += px[i]; rightB += px[i + 2]; }
        }
    }
    EXPECT_GT(leftR, leftB)
        << "GL EASU must keep the left half red-dominant (red=" << leftR << ", blue=" << leftB << ")";
    EXPECT_GT(rightB, rightR)
        << "GL EASU must keep the right half blue-dominant (blue=" << rightB << ", red=" << rightR << ")";

    rhi->shutdown();
}

} // namespace
