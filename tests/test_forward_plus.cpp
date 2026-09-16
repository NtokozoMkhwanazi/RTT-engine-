/**
 * Forward+ render test (suggestions.txt #2 Follow-up).
 *
 * Verifies the Forward+ *shader tile* that was the missing piece:
 * `shaderSystem/forward_plus.{vert,frag}` consumes the BinResult SSBOs produced
 * by ClusteredForward::binLights (the verified #2 binning core) and accumulates
 * per-cluster light contributions. This test:
 *   - builds a 1-cluster grid covering a single point light
 *   - bins the light with the real CPU binning core -> BinResult
 *   - uploads LightUBO + (tileCounts, tileOffsets, indexBuffer) SSBOs
 *   - renders a fullscreen quad through forward_plus and readPixels the center
 *   - asserts: bin puts the light in cluster 0, lit pixel is bright, removing
 *     the light (counts[0]=0) drives the pixel back to ambient-only.
 * glslc + spirv-val validate the shaders separately. Skips without a GL context.
 */
#include <gtest/gtest.h>
#include <glad/glad.h>
// GLAD was generated for GL 1.x-3.1; GL_SHADER_STORAGE_BUFFER (OpenGL 4.3+
// SSBO binding target) isn't in our glad.h. Define it so the test compiles
// and then skips cleanly via GTEST_SKIP() when there's no GL context.
#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#endif
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include "lighting/ClusteredForward.h"
#include "lighting/LightData.h"
#include "lighting/LightingSystem.h"   // Light, LightType
#include "shaderSystem/Shader.h"

namespace {
GLFWwindow* g_win = nullptr;
bool EnsureGL() {
    static const bool ok = []() {
        if (!glfwInit()) return false;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        g_win = glfwCreateWindow(64, 64, "fwdplus-test", nullptr, nullptr);
        if (!g_win) { glfwTerminate(); return false; }
        glfwMakeContextCurrent(g_win);
        return gladLoadGLLoader((GLADloadproc)glfwGetProcAddress) != 0;
    }();
    return ok;
}

float brightness(const glm::vec3& c) { return glm::max(glm::max(c.r, c.g), c.b); }
} // namespace

TEST(ForwardPlusGL, ClusterBinAndShaderLightLoop) {
    if (!EnsureGL()) GTEST_SKIP() << "No OpenGL context available";
    GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);

    // --- 1-cluster grid, camera at origin looking down -Z (view = identity) ---
    Clustered::LightBinning binning;
    const glm::mat4 view = glm::mat4(1.0f);
    Clustered::ClusterGrid grid = binning.buildGrid(1, 1, 1, 0.1f, 10.0f,
                                                    glm::radians(90.0f), 1.0f, view);
    ASSERT_TRUE(grid.built);
    ASSERT_EQ(grid.clusterCount(), 1);

    // Single point light at world (0,0,-5): in view space (0,0,-5) -> cluster 0.
    const Light light = Light(LightType::POINT, glm::vec3(0.0f, 0.0f, -5.0f),
                              glm::vec3(0,0,-1), glm::vec3(1.0f), 1.0f);
    const Clustered::BinResult bins = binning.binLights(grid, {light}, 64);

    ASSERT_EQ(bins.counts.size(), 1u);
    ASSERT_EQ(bins.offsets.size(), 1u);
    EXPECT_EQ(bins.counts[0], 1u) << "binning must place the light in cluster 0";
    EXPECT_EQ(bins.offsets[0], 0u);
    ASSERT_EQ(bins.indices.size(), 1u);
    EXPECT_EQ(bins.indices[0], 0u);

    // --- Upload GPU light buffer (CPU mirror of the shader's LightBlock) ---
    LightUBO ubo{};
    ubo.lightCount = 1;
    ubo.lights[0]  = makeGpuLight(1u, glm::vec3(0,0,-5), glm::vec3(0,0,-1),
                                  glm::vec3(1.0f), 1.0f, 1.0f, 0.0f, 0.0f);
    GLuint ssbo[4]; glGenBuffers(4, ssbo);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo[0]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(LightUBO), &ubo, GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo[0]);

    auto uploadTileLists = [&](uint32_t count, uint32_t index0) {
        uint32_t counts[1]   = { count };
        uint32_t offsets[1]  = { 0 };
        uint32_t indices[64] = { 0 };
        indices[0] = index0;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo[1]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(counts),   counts,   GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ssbo[1]);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo[2]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(offsets),  offsets,  GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ssbo[2]);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo[3]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(indices),  indices,  GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, ssbo[3]);
    };
    uploadTileLists(bins.counts[0], bins.indices[0]);

    Shader fwd("shaderSystem/forward_plus.vert", "shaderSystem/forward_plus.frag");
    ASSERT_TRUE(fwd.ID != 0) << "forward_plus shader failed to compile";
    fwd.use();
    // Fullscreen quad -> fixed shading point at world origin, normal facing -Z
    // (toward the light at z=-5).
    fwd.setVec3("uWorldPos",  glm::vec3(0.0f, 0.0f, 0.0f));
    fwd.setVec3("uNormal",    glm::vec3(0.0f, 0.0f, -1.0f));
    fwd.setVec3("uAlbedo",    glm::vec3(0.5f));
    fwd.setFloat("uAmbient",   0.05f);

    glViewport(0, 0, 64, 64);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    unsigned char p[4];
    glReadPixels(31, 31, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    glm::vec3 lit(glm::vec3(p[0], p[1], p[2]) / 255.0f);

    // Now drop the light (counts[0] = 0) -> shader falls back to ambient-only.
    uploadTileLists(0u, 0u);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    glReadPixels(31, 31, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    glm::vec3 unlit(glm::vec3(p[0], p[1], p[2]) / 255.0f);

    const float bLit  = brightness(lit);
    const float bNone = brightness(unlit);
    EXPECT_GT(bLit, 0.4f) << "point light + correct bin -> quad must be bright";
    EXPECT_LT(bNone, 0.05f) << "no lights in tile -> ambient-only quad must be dark";
    EXPECT_GT(bLit, 2.0f * bNone) << "lit pixel must clearly exceed ambient-only";
}
