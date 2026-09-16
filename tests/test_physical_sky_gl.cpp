/**
 * PhysicalSky GL test (suggestions.txt #4) — verifies the procedural sky
 * shader (`shaderSystem/procedural_sky.{vert,frag}`, a GLSL port of
 * PhysicalSky::evaluate) actually renders the *tested* color model:
 *   - overhead sun -> bright, pixel-matches PhysicalSky::skyColor (C++)
 *   - sun below horizon -> dark
 *   - turbidity increases off-sun haze (GLSL Mie path)
 * This is the render-pixel test that was previously missing, so the sky
 * shader is now runtime-verified (not just glslc-validated). GL is headless
 * via EnsureGL(); skips cleanly if no context.
 */
#include <gtest/gtest.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vector>

#include "lighting/PhysicalSky.h"
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
        g_win=glfwCreateWindow(64,64,"physical-sky-gl-test",nullptr,nullptr); if(!g_win){glfwTerminate();return false;}
        glfwMakeContextCurrent(g_win);
        return gladLoadGLLoader((GLADloadproc)glfwGetProcAddress) != 0; }();
    return ok;
}

// Draw a fullscreen quad through `sky` with the given uniforms, return the
// center pixel as [0,1] linear RGBA.
glm::vec4 drawAndReadPixel(Shader& sky,
                           const glm::vec3& sunDir,
                           const glm::vec3& viewDir,
                           float turbidity, float sunIntensity) {
    sky.use();
    sky.setVec3("uSunDir", sunDir);
    sky.setVec3("uViewDir", viewDir);
    sky.setFloat("uTurbidity", turbidity);
    sky.setFloat("uSunIntensity", sunIntensity);
    glViewport(0, 0, 64, 64);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);   // fullscreen quad via gl_VertexID
    glFinish();
    unsigned char px[4];
    glReadPixels(31, 31, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return glm::vec4(px[0] / 255.0f, px[1] / 255.0f,
                     px[2] / 255.0f, px[3] / 255.0f);
}
} // namespace

TEST(PhysicalSkyGL, DayOverheadIsBrightAndMatchesCppModel) {
    if (!EnsureGL()) GTEST_SKIP() << "No OpenGL context available";
    GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao); // core-profile requires a VAO
    Shader sky("shaderSystem/procedural_sky.vert", "shaderSystem/procedural_sky.frag");
    ASSERT_TRUE(sky.ID != 0) << "procedural sky shader failed to compile";

    const glm::vec3 sun(0, 1, 0);
    const glm::vec3 view(0, 1, 0);
    const glm::vec4 px = drawAndReadPixel(sky, sun, view, 2.0f, 1.0f);

    // Regime: overhead day sky is clearly bright (not starfield-dark).
    EXPECT_GT(PhysicalSky::brightness(glm::vec3(px)), 0.5f)
        << "day/overhead sky must be bright";

    // Faithfulness: GLSL pixel matches the C++ PhysicalSky::skyColor reference
    // within byte-quantization tolerance (the shader is a port, not a new model).
    const glm::vec3 ref = PhysicalSky::skyColor(view, sun, 2.0f, 1.0f);
    const float tol = 4.0f / 255.0f;
    EXPECT_NEAR(px.r, ref.r, tol);
    EXPECT_NEAR(px.g, ref.g, tol);
    EXPECT_NEAR(px.b, ref.b, tol);
}

TEST(PhysicalSkyGL, NightIsDark) {
    if (!EnsureGL()) GTEST_SKIP() << "No OpenGL context available";
    Shader sky("shaderSystem/procedural_sky.vert", "shaderSystem/procedural_sky.frag");
    ASSERT_TRUE(sky.ID != 0);

    // Sun below the horizon -> sunContrib collapses -> only the starfield.
    const glm::vec4 px = drawAndReadPixel(sky, glm::vec3(0,-1,0), glm::vec3(0,1,0), 2.0f, 1.0f);
    EXPECT_LT(PhysicalSky::brightness(glm::vec3(px)), 0.05f)
        << "night (sun below horizon) must be dark in the shader too";

    const glm::vec3 ref = PhysicalSky::skyColor(glm::vec3(0,1,0), glm::vec3(0,-1,0), 2.0f, 1.0f);
    const float tol = 4.0f / 255.0f;
    EXPECT_NEAR(px.r, ref.r, tol);
    EXPECT_NEAR(px.g, ref.g, tol);
    EXPECT_NEAR(px.b, ref.b, tol);
}

TEST(PhysicalSkyGL, TurbidityBrightensOffSunHaze) {
    if (!EnsureGL()) GTEST_SKIP() << "No OpenGL context available";
    Shader sky("shaderSystem/procedural_sky.vert", "shaderSystem/procedural_sky.frag");
    ASSERT_TRUE(sky.ID != 0);
    const glm::vec3 sun(0,1,0);
    const glm::vec3 viewOff = glm::normalize(glm::vec3(0,1,-1)); // ~45 deg off the sun
    const float clear = PhysicalSky::brightness(glm::vec3(drawAndReadPixel(sky, sun, viewOff, 2.0f, 1.0f)));
    const float hazy  = PhysicalSky::brightness(glm::vec3(drawAndReadPixel(sky, sun, viewOff, 32.0f, 1.0f)));
    EXPECT_GT(hazy, clear) << "higher turbidity must brighten off-sun haze in the shader";
}
