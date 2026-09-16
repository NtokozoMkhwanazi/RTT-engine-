/**
 * TEMP verification harness for suggestions.txt #1 (forward pbrFS.glsl port).
 * Mirrors ForwardPlusGL's EnsureGL pattern. Verifies that pbrVS.glsl +
 * pbrFS.glsl compile/link under the GL driver after porting the forward PBR
 * light loop to the shared GPULightData SSBO, AND that a point light driven
 * through that SSBO actually brightens the rendered pixel (point/spot
 * attenuation path).
 */
#include <gtest/gtest.h>
#include <glad/glad.h>
#include <vector>
// GLAD was generated for GL 1.x-3.1; GL_SHADER_STORAGE_BUFFER (OpenGL 4.3+
// SSBO binding target) isn't in our glad.h. Define it so the test compiles
// and then skips cleanly via GTEST_SKIP() when there's no GL context.
#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#endif
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "shaderSystem/Shader.h"
#include "lighting/LightData.h"

namespace {
GLFWwindow* g_win = nullptr;
bool EnsureGL() {
    static const bool ok = []() {
        if (!glfwInit()) return false;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        g_win = glfwCreateWindow(64, 64, "pbr-fwd-test", nullptr, nullptr);
        if (!g_win) { glfwTerminate(); return false; }
        glfwMakeContextCurrent(g_win);
        return gladLoadGLLoader((GLADloadproc)glfwGetProcAddress) != 0;
    }();
    return ok;
}

float brightness(const glm::vec3& c) { return glm::max(glm::max(c.r, c.g), c.b); }

GLuint MakePixelTexture(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    unsigned char px[4] = {r, g, b, a};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}
} // namespace

TEST(PbrForwardGL, CompilesAndLinks) {
    if (!EnsureGL()) GTEST_SKIP() << "No OpenGL context available";
    Shader s("shaderSystem/pbrVS.glsl", "shaderSystem/pbrFS.glsl");
    ASSERT_NE(s.ID, 0u) << "pbrVS.glsl + pbrFS.glsl must compile and link under GL";
}

TEST(PbrForwardGL, PointLightThroughSSBOIsLit) {
    if (!EnsureGL()) GTEST_SKIP() << "No OpenGL context available";
    Shader shader("shaderSystem/pbrVS.glsl", "shaderSystem/pbrFS.glsl");
    ASSERT_NE(shader.ID, 0u);

    // --- vertex data: single triangle facing +Z at z=-5 ---
    // pbrVS layout: 0=aPos 1=aNormal 2=aTexCoords 3=aInstancePos
    //               4=aInstanceColor 5-8=aModel(mat4). One vertex = aPos(3) +
    // aNormal(3) + aTexCoords(2) + aInstancePos(3, PAD to 4) + aInstanceColor(4)
    // + aModel(16) = 32 floats = 128 bytes, 16-byte aligned for the mat4.
    const GLsizei stride = 32 * sizeof(float);
    float verts[] = {
        -1.0f,-1.0f,-5.0f,  0,0,1,  0,0,  0,0,0,0,  1,1,1,1,  1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
         1.0f,-1.0f,-5.0f,  0,0,1,  1,0,  0,0,0,0,  1,1,1,1,  1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
         0.0f, 1.0f,-5.0f,  0,0,1,  0.5f,1, 0,0,0,0,  1,1,1,1,  1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
    };
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)(0));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(12));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)(24));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, (void*)(32));
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (void*)(48));
    glEnableVertexAttribArray(5); glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, (void*)(64));
    glEnableVertexAttribArray(6); glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, stride, (void*)(80));
    glEnableVertexAttribArray(7); glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, stride, (void*)(96));
    glEnableVertexAttribArray(8); glVertexAttribPointer(8, 4, GL_FLOAT, GL_FALSE, stride, (void*)(112));

    // --- textures: white diffuse + SSAO, black for the rest (fall back to UBO mat) ---
    GLuint white = MakePixelTexture(255,255,255,255);
    GLuint black = MakePixelTexture(0,0,0,255);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, white);   // texture_diffuse1
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, black);   // texture_metallic1
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, black);   // texture_roughness1
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, black);   // texture_ao1
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, black);   // texture_emissive1
    glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, black);   // texture_normal1
    glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D, white);   // uSSAOMap
    // uShadowMapArray is a sampler2DArray; core profile raises GL_INVALID_OPERATION
    // at draw if its unit holds a differently-typed texture. Bind a dummy 2D
    // array so the type matches (never sampled for a point light).
    GLuint shadowArr; glGenTextures(1, &shadowArr);
    glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D_ARRAY, shadowArr);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, 1, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    shader.use();
    glUniform1i(glGetUniformLocation(shader.ID,"texture_diffuse1"),0);
    glUniform1i(glGetUniformLocation(shader.ID,"texture_metallic1"),1);
    glUniform1i(glGetUniformLocation(shader.ID,"texture_roughness1"),2);
    glUniform1i(glGetUniformLocation(shader.ID,"texture_ao1"),3);
    glUniform1i(glGetUniformLocation(shader.ID,"texture_emissive1"),4);
    glUniform1i(glGetUniformLocation(shader.ID,"texture_normal1"),5);
    glUniform1i(glGetUniformLocation(shader.ID,"uShadowMapArray"),6);
    glUniform1i(glGetUniformLocation(shader.ID,"uSSAOMap"),8);

    // --- MaterialBlock UBO (binding 2): white matte diffuse, non-metal ---
    struct MatUBO { glm::vec4 albedo; glm::vec4 emissive; float metallic,roughness,ao,pad; };
    static_assert(sizeof(MatUBO)==48,"");
    MatUBO mat{}; mat.albedo=glm::vec4(0.8f,0.7f,0.6f,1.0f);
    mat.metallic=0.0f; mat.roughness=0.5f; mat.ao=1.0f;
    GLuint matUBO; glGenBuffers(1,&matUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, matUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(MatUBO), &mat, GL_STATIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, matUBO);

    // --- camera + zeroed ambient (so un-lit pixel is black) ---
    glm::vec3 eye(0,0,0);
    glm::mat4 view = glm::lookAt(eye, glm::vec3(0,0,-1), glm::vec3(0,1,0));
    glm::mat4 proj = glm::perspective(glm::radians(45.0f),1.0f,0.1f,100.0f);
    glUniformMatrix4fv(glGetUniformLocation(shader.ID,"view"),1,GL_FALSE,&view[0][0]);
    glUniformMatrix4fv(glGetUniformLocation(shader.ID,"projection"),1,GL_FALSE,&proj[0][0]);
    glUniform3fv(glGetUniformLocation(shader.ID,"uCameraPos"),1,&eye[0]);
    glUniform3f(glGetUniformLocation(shader.ID,"uGroundBounce"),0,0,0);
    glUniform3f(glGetUniformLocation(shader.ID,"uSkyTint"),0,0,0);
    glUniform1f(glGetUniformLocation(shader.ID,"uAmbientStrength"),0.0f);
    glUniform1f(glGetUniformLocation(shader.ID,"uSkyLightStrength"),0.0f);
    glUniform1i(glGetUniformLocation(shader.ID,"uSSAOMap"),8);

    GLuint ssbo; glGenBuffers(1,&ssbo);
    glDisable(GL_DEPTH_TEST);

    // Render the triangle once per scenario, re-uploading the light SSBO.
    // pbrVS.glsl needs the VAO bound for its aPos/aModel attributes.
    auto renderOnce = [&](uint32_t lightCount, const GPULightData* lights) -> glm::vec3 {
        LightUBO ubo{};
        ubo.lightCount = lightCount;
        for (uint32_t i = 0; i < lightCount && i < kMaxGpuLights; ++i) ubo.lights[i] = lights[i];
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(LightUBO), &ubo, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);

        glViewport(0,0,64,64);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        shader.use();
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
        unsigned char p[4]; glReadPixels(31,31,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);
        return glm::vec3(p[0],p[1],p[2])/255.0f;
    };

    // Point light in front of the centroid of the triangle; the SSBO is the
    // only light data path pbrFS.glsl consumes (#1).
    GPULightData pt = makeGpuLight(1u, glm::vec3(0,0,-2), glm::vec3(0),
                                  glm::vec3(1,1,1), 5.0f);
    glm::vec3 lit   = renderOnce(1u, &pt);
    glm::vec3 unlit = renderOnce(0u, nullptr);

    EXPECT_GT(brightness(lit), 0.4f)    << "point light via GPULightData SSBO must brighten the pixel";
    EXPECT_LT(brightness(unlit), 0.05f) << "no lights -> ambient-only pixel must be dark";
    EXPECT_GT(brightness(lit), 2.0f*brightness(unlit));

    glDeleteTextures(1,&white); glDeleteTextures(1,&black); glDeleteTextures(1,&shadowArr);
    glDeleteBuffers(1,&vao); glDeleteBuffers(1,&vbo); glDeleteBuffers(1,&matUBO); glDeleteBuffers(1,&ssbo);
}

// --- Helpers for the POM displacement test ----------------------------------

GLuint MakeGradientTexture(int w, int h, const unsigned char* pixels) {
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

GLuint MakeNormalWithAlpha(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    // Normal maps store tangent-space Z in [0,255] → unpacks to [−1,1].
    // (128,128,255) → (0,0,1) = flat up. Alpha = height in pbrFS.glsl.
    unsigned char px[4] = {r, g, b, a};
    return MakeGradientTexture(1, 1, px);
}

static std::vector<unsigned char> MakeGrayscaleGradient(int w) {
    std::vector<unsigned char> px(w * 4);
    for (int i = 0; i < w; ++i) {
        unsigned char v = static_cast<unsigned char>(255 * i / (w - 1));
        px[i*4+0] = v; px[i*4+1] = v; px[i*4+2] = v; px[i*4+3] = 255;
    }
    return px;
}

// Find the brightest pixel in the current framebuffer.
static glm::vec3 BrightestPixel() {
    unsigned char px[64 * 64 * 4];
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glm::vec3 best(0,0,0); float maxB = 0;
    for (int i = 0; i < 64*64; ++i) {
        glm::vec3 c(px[i*4], px[i*4+1], px[i*4+2]);
        c /= 255.0f;
        if (brightness(c) > maxB) { maxB = brightness(c); best = c; }
    }
    return best;
}

// PomDisplacementChangesOutput
//
// pbrFS.glsl calls CalculatePOM() (parallax occlusion mapping) using the
// alpha channel of texture_normal1 as the height field:
//   height = 1.0 - texture(...).a
//
// With alpha=255 (height=0) the POM ray-march is a no-op → UVs pass through
// unchanged.  With alpha=0   (height=1) the ray marches the full height
// field → UVs are displaced along the view direction.
//
// We use a 256-texel grayscale gradient as the diffuse map and a side-view
// camera so the view vector has a large tangent-space X component.  The
// resulting POM displacement (≥0.05 UV ≈ 12+ gradient texels) is large
// enough that the brightest screen pixel differs between the two renders,
// proving POM is wired in end-to-end.
TEST(PbrForwardGL, PomDisplacementChangesOutput) {
    if (!EnsureGL()) GTEST_SKIP() << "No OpenGL context available";
    Shader shader("shaderSystem/pbrVS.glsl", "shaderSystem/pbrFS.glsl");
    ASSERT_NE(shader.ID, 0u);

    const GLsizei stride = 32 * sizeof(float);
    float verts[] = {
        -1.0f,-1.0f,-5.0f,  0,0,1,  0,0,  0,0,0,0,  1,1,1,1,  1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
         1.0f,-1.0f,-5.0f,  0,0,1,  1,0,  0,0,0,0,  1,1,1,1,  1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
         0.0f, 1.0f,-5.0f,  0,0,1,  0.5f,1, 0,0,0,0,  1,1,1,1,  1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
    };
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)(0));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(12));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)(24));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, (void*)(32));
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (void*)(48));
    glEnableVertexAttribArray(5); glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, (void*)(64));
    glEnableVertexAttribArray(6); glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, stride, (void*)(80));
    glEnableVertexAttribArray(7); glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, stride, (void*)(96));
    glEnableVertexAttribArray(8); glVertexAttribPointer(8, 4, GL_FLOAT, GL_FALSE, stride, (void*)(112));

    // 256-texel grayscale gradient (black→white) so a UV shift produces a
    // measurable color change in the rendered pixel.
    auto gradPixels = MakeGrayscaleGradient(256);
    GLuint diffuse = MakeGradientTexture(256, 1, gradPixels.data());

    // Ambient = 0; pixel is dark unless the point light hits it.
    shader.use();
    glUniform3f(glGetUniformLocation(shader.ID,"uGroundBounce"),0,0,0);
    glUniform3f(glGetUniformLocation(shader.ID,"uSkyTint"),0,0,0);
    glUniform1f(glGetUniformLocation(shader.ID,"uAmbientStrength"),0.0f);
    glUniform1f(glGetUniformLocation(shader.ID,"uSkyLightStrength"),0.0f);

    // Material UBO (binding 2): white matte, non-metal
    struct MatUBO { glm::vec4 albedo; glm::vec4 emissive; float metallic,roughness,ao,pad; };
    static_assert(sizeof(MatUBO)==48,"");
    MatUBO mat{}; mat.albedo=glm::vec4(1,1,1,1);
    mat.metallic=0.0f; mat.roughness=0.5f; mat.ao=1.0f;
    GLuint matUBO; glGenBuffers(1,&matUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, matUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(MatUBO), &mat, GL_STATIC_DRAW);

    // Light SSBO (binding 0): one point light in front of the triangle
    GPULightData pt = makeGpuLight(1u, glm::vec3(0,0,-2), glm::vec3(0),
                                   glm::vec3(1,1,1), 5.0f);
    LightUBO ubo{}; ubo.lightCount = 1; ubo.lights[0] = pt;
    GLuint ssbo; glGenBuffers(1,&ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(LightUBO), &ubo, GL_DYNAMIC_DRAW);

    // Dummy shadow array + SSAO
    GLuint shadowArr; glGenTextures(1,&shadowArr);
    glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D_ARRAY, shadowArr);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, 1,1,1, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    GLuint white = MakePixelTexture(255,255,255,255);
    glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D, white);

    // Texture unit assignments
    glUniform1i(glGetUniformLocation(shader.ID,"texture_diffuse1"),0);
    glUniform1i(glGetUniformLocation(shader.ID,"texture_metallic1"),1);
    glUniform1i(glGetUniformLocation(shader.ID,"texture_roughness1"),2);
    glUniform1i(glGetUniformLocation(shader.ID,"texture_ao1"),3);
    glUniform1i(glGetUniformLocation(shader.ID,"texture_emissive1"),4);
    glUniform1i(glGetUniformLocation(shader.ID,"texture_normal1"),5);
    glUniform1i(glGetUniformLocation(shader.ID,"uShadowMapArray"),6);
    glUniform1i(glGetUniformLocation(shader.ID,"uSSAOMap"),8);

    // Side-view camera: large X component in viewDirTS → large POM shift in U.
    glm::vec3 eye(-8, 0, -2);
    glm::mat4 view  = glm::lookAt(eye, glm::vec3(0,0,-5), glm::vec3(0,1,0));
    glm::mat4 proj  = glm::perspective(glm::radians(45.0f),1.0f,0.1f,100.0f);

    auto renderWithAlpha = [&](unsigned char alpha) -> glm::vec3 {
        GLuint norm = MakeNormalWithAlpha(128,128,255, alpha);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, diffuse);
        glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, norm);
        glUniform3fv(glGetUniformLocation(shader.ID,"uCameraPos"),1,&eye[0]);
        glUniformMatrix4fv(glGetUniformLocation(shader.ID,"view"),1,GL_FALSE,&view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(shader.ID,"projection"),1,GL_FALSE,&proj[0][0]);

        glViewport(0,0,64,64);
        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_DEPTH_TEST);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, matUBO);
        shader.use();
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3); glFinish();

        glm::vec3 brightest = BrightestPixel();
        glDeleteTextures(1,&norm);
        return brightest;
    };

    // alpha=255 → height=0 → POM ray-march is a no-op → UVs unchanged
    glm::vec3 noPom  = renderWithAlpha(255);
    // alpha=0   → height=1 → POM ray-marches full depth → UVs displaced
    glm::vec3 withPom = renderWithAlpha(0);

    // If POM is active, the displaced UV samples a different texel of the
    // 256-texel gradient → a measurably different brightest pixel.
    EXPECT_NE(noPom, withPom)
        << "POM must displace UVs enough to sample a different diffuse texel";
    EXPECT_GT(brightness(withPom), 0.0f)
        << "point-light-lit pixel must be non-black even with max POM height";

    glDeleteTextures(1,&diffuse); glDeleteTextures(1,&white); glDeleteTextures(1,&shadowArr);
    glDeleteBuffers(1,&vao); glDeleteBuffers(1,&vbo); glDeleteBuffers(1,&matUBO); glDeleteBuffers(1,&ssbo);
}
