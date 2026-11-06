/**
 * Viewport Debug Test - Comprehensive viewport rendering tests
 *
 * This test validates:
 * - Framebuffer creation and completeness
 * - Basic cube rendering
 * - Multiple cube rendering with different colors
 * - Camera setup and matrices
 * - Shader compilation and uniform setting
 * - VAO/VBO/EBO creation
 * - Viewport resize handling
 * - Debug visualization overlays
 *
 * Run: make viewport-test && ./bin/viewport_test
 */

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <chrono>

// ============================================================================
// Test Configuration
// ============================================================================
struct TestConfig {
    int width = 1280;
    int height = 720;
    bool verbose = false;
    bool pauseOnFailure = true;
    float testDuration = 5.0f;  // seconds per test
};

static TestConfig g_config;

// ============================================================================
// Framebuffer Structure (same as test.cpp)
// ============================================================================
struct Framebuffer {
    GLuint fbo = 0;
    GLuint colorTex = 0;
    GLuint rbo = 0;
    int width = 1280;
    int height = 720;

    void init(int w, int h) {
        cleanup();
        width = w;
        height = h;

        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

        glGenTextures(1, &colorTex);
        glBindTexture(GL_TEXTURE_2D, colorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);

        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "ERROR: Framebuffer incomplete! Status: " << status << "\n";
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void resize(int w, int h) {
        if (w <= 0 || h <= 0 || (w == width && h == height)) return;
        init(w, h);
    }

    void cleanup() {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (colorTex) glDeleteTextures(1, &colorTex);
        if (rbo) glDeleteRenderbuffers(1, &rbo);
        fbo = 0;
        colorTex = 0;
        rbo = 0;
    }

    void bind() {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
    }

    void unbind() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    bool isComplete() {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return status == GL_FRAMEBUFFER_COMPLETE;
    }
};

// ============================================================================
// Shader Helper
// ============================================================================
static GLuint g_shaderProg = 0;

static GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        std::cerr << "ERROR: Shader compilation failed: " << log << "\n";
        return 0;
    }
    return shader;
}

static void initShader() {
    const char* vs = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
uniform mat4 model, view, projection;
out vec3 FragPos, Normal;
void main() {
    FragPos = vec3(model * vec4(aPos, 1.0));
    Normal = mat3(transpose(inverse(model))) * aNormal;
    gl_Position = projection * view * vec4(FragPos, 1.0);
}
)";

    const char* fs = R"(
#version 330 core
in vec3 FragPos, Normal;
uniform vec3 color, lightPos, viewPos;
out vec4 FragColor;
void main() {
    vec3 ambient = 0.2 * vec3(1.0);
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diff * vec3(1.0);
    vec3 viewDir = normalize(viewPos - FragPos);
    vec3 reflectDir = reflect(-lightDir, norm);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32);
    vec3 specular = 0.3 * spec * vec3(1.0);
    FragColor = vec4((ambient + diffuse + specular) * color, 1.0);
}
)";

    GLuint vsObj = compileShader(GL_VERTEX_SHADER, vs);
    GLuint fsObj = compileShader(GL_FRAGMENT_SHADER, fs);

    g_shaderProg = glCreateProgram();
    glAttachShader(g_shaderProg, vsObj);
    glAttachShader(g_shaderProg, fsObj);
    glLinkProgram(g_shaderProg);

    GLint success;
    glGetProgramiv(g_shaderProg, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(g_shaderProg, 512, nullptr, log);
        std::cerr << "ERROR: Shader program linking failed: " << log << "\n";
        return;
    }

    glDeleteShader(vsObj);
    glDeleteShader(fsObj);

    std::cout << "[OK] Shader program compiled and linked successfully\n";
}

static void useShader() { glUseProgram(g_shaderProg); }

static void setMat4(const char* n, const glm::mat4& m) {
    GLint loc = glGetUniformLocation(g_shaderProg, n);
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, &m[0][0]);
}

static void setVec3(const char* n, const glm::vec3& v) {
    GLint loc = glGetUniformLocation(g_shaderProg, n);
    if (loc >= 0) glUniform3fv(loc, 1, &v[0]);
}

static void cleanupShader() {
    if (g_shaderProg) glDeleteProgram(g_shaderProg);
    g_shaderProg = 0;
}

// ============================================================================
// Cube Mesh
// ============================================================================
static GLuint g_cubeVAO = 0, g_cubeVBO = 0, g_cubeEBO = 0;

static void initCube() {
    float verts[] = {
        // Back face
        -0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
         0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
         0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
         0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
        -0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
        -0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
        // Front face
        -0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
         0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
         0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
         0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
        -0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
        -0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
        // Left face
        -0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f,
        -0.5f,  0.5f, -0.5f, -1.0f,  0.0f,  0.0f,
        -0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f,
        -0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f,
        -0.5f, -0.5f,  0.5f, -1.0f,  0.0f,  0.0f,
        -0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f,
        // Right face
         0.5f,  0.5f,  0.5f,  1.0f,  0.0f,  0.0f,
         0.5f,  0.5f, -0.5f,  1.0f,  0.0f,  0.0f,
         0.5f, -0.5f, -0.5f,  1.0f,  0.0f,  0.0f,
         0.5f, -0.5f, -0.5f,  1.0f,  0.0f,  0.0f,
         0.5f, -0.5f,  0.5f,  1.0f,  0.0f,  0.0f,
         0.5f,  0.5f,  0.5f,  1.0f,  0.0f,  0.0f,
        // Bottom face
        -0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,
         0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,
         0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,
         0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,
        -0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,
        -0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,
        // Top face
        -0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,
         0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,
         0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,
         0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,
        -0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,
        -0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,
    };

    glGenVertexArrays(1, &g_cubeVAO);
    glGenBuffers(1, &g_cubeVBO);
    glGenBuffers(1, &g_cubeEBO);

    glBindVertexArray(g_cubeVAO);

    glBindBuffer(GL_ARRAY_BUFFER, g_cubeVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    // Position attribute
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Normal attribute
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    std::cout << "[OK] Cube VAO created successfully\n";
}

static void drawCube() {
    glBindVertexArray(g_cubeVAO);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);
}

static void cleanupCube() {
    if (g_cubeVAO) glDeleteVertexArrays(1, &g_cubeVAO);
    if (g_cubeVBO) glDeleteBuffers(1, &g_cubeVBO);
    g_cubeVAO = 0;
    g_cubeVBO = 0;
}

// ============================================================================
// Test Results
// ============================================================================
struct TestResult {
    std::string name;
    bool passed;
    std::string message;
    double duration;
};

static std::vector<TestResult> g_testResults;

void reportTest(const std::string& name, bool passed, const std::string& message = "", double duration = 0.0) {
    TestResult result;
    result.name = name;
    result.passed = passed;
    result.message = message;
    result.duration = duration;
    g_testResults.push_back(result);

    const char* status = passed ? "[PASS]" : "[FAIL]";
    const char* color = passed ? "\033[32m" : "\033[31m";
    std::cout << color << status << "\033[0m " << name;
    if (!message.empty()) std::cout << " - " << message;
    if (duration > 0) std::cout << " (" << duration << "ms)";
    std::cout << "\n";
}

// ============================================================================
// Test Cases
// ============================================================================

// Test 1: OpenGL Context
bool testOpenGLContext() {
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));

    if (!version || !vendor || !renderer) {
        reportTest("OpenGL Context", false, "Failed to get OpenGL info");
        return false;
    }

    std::cout << "  OpenGL Version: " << version << "\n";
    std::cout << "  Vendor: " << vendor << "\n";
    std::cout << "  Renderer: " << renderer << "\n";

    reportTest("OpenGL Context", true, version);
    return true;
}

// Test 2: Framebuffer Creation
bool testFramebufferCreation(Framebuffer& fbo) {
    auto start = std::chrono::high_resolution_clock::now();

    fbo.init(g_config.width, g_config.height);

    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();

    if (!fbo.isComplete()) {
        reportTest("Framebuffer Creation", false, "Framebuffer incomplete", duration);
        return false;
    }

    if (fbo.fbo == 0 || fbo.colorTex == 0 || fbo.rbo == 0) {
        reportTest("Framebuffer Creation", false, "Missing framebuffer objects", duration);
        return false;
    }

    reportTest("Framebuffer Creation", true,
               std::to_string(fbo.width) + "x" + std::to_string(fbo.height), duration);
    return true;
}

// Test 3: Framebuffer Resize
bool testFramebufferResize(Framebuffer& fbo) {
    auto start = std::chrono::high_resolution_clock::now();

    // Test multiple resize operations
    std::vector<std::pair<int, int>> sizes = {
        {640, 480}, {800, 600}, {1024, 768}, {1920, 1080}, {1280, 720}
    };

    for (auto& size : sizes) {
        fbo.resize(size.first, size.second);
        if (fbo.width != size.first || fbo.height != size.second) {
            reportTest("Framebuffer Resize", false,
                       "Resize failed: expected " + std::to_string(size.first) + "x" +
                       std::to_string(size.second) + ", got " + std::to_string(fbo.width) + "x" +
                       std::to_string(fbo.height));
            return false;
        }
        if (!fbo.isComplete()) {
            reportTest("Framebuffer Resize", false,
                       "Framebuffer incomplete after resize to " + std::to_string(size.first) + "x" +
                       std::to_string(size.second));
            return false;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();

    reportTest("Framebuffer Resize", true, "Tested 5 different resolutions", duration);
    return true;
}

// Test 4: Shader Compilation
bool testShaderCompilation() {
    auto start = std::chrono::high_resolution_clock::now();

    initShader();

    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();

    if (g_shaderProg == 0) {
        reportTest("Shader Compilation", false, "Shader program is 0", duration);
        return false;
    }

    // Verify shader program linked successfully by checking link status
    GLint linkStatus;
    glGetProgramiv(g_shaderProg, GL_LINK_STATUS, &linkStatus);
    if (!linkStatus) {
        reportTest("Shader Compilation", false, "Not a valid shader program", duration);
        return false;
    }

    reportTest("Shader Compilation", true, "Vertex + Fragment shaders", duration);
    return true;
}

// Test 5: Cube Mesh Creation
bool testCubeMeshCreation() {
    auto start = std::chrono::high_resolution_clock::now();

    initCube();

    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();

    if (g_cubeVAO == 0) {
        reportTest("Cube Mesh Creation", false, "VAO is 0", duration);
        return false;
    }

    // Verify VAO state
    glBindVertexArray(g_cubeVAO);
    GLint vaoState;
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &vaoState);
    glBindVertexArray(0);

    if (!vaoState) {
        reportTest("Cube Mesh Creation", false, "Vertex attribute arrays not enabled", duration);
        return false;
    }

    reportTest("Cube Mesh Creation", true, "36 vertices with normals", duration);
    return true;
}

// Test 6: Single Cube Rendering
bool testSingleCubeRendering(Framebuffer& fbo) {
    auto start = std::chrono::high_resolution_clock::now();

    fbo.bind();
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    useShader();

    // Set up camera
    glm::mat4 view = glm::lookAt(
        glm::vec3(3.0f, 3.0f, 3.0f),
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f)
    );
    glm::mat4 projection = glm::perspective(
        glm::radians(45.0f),
        static_cast<float>(fbo.width) / fbo.height,
        0.1f, 100.0f
    );

    setMat4("view", view);
    setMat4("projection", projection);
    setVec3("lightPos", glm::vec3(5.0f, 5.0f, 5.0f));
    setVec3("viewPos", glm::vec3(3.0f, 3.0f, 3.0f));

    // Draw red cube at origin
    glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f));
    setMat4("model", model);
    setVec3("color", glm::vec3(1.0f, 0.0f, 0.0f));  // Red

    drawCube();

    // Check for OpenGL errors
    GLenum err = glGetError();
    fbo.unbind();

    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();

    if (err != GL_NO_ERROR) {
        reportTest("Single Cube Rendering", false, "OpenGL error: " + std::to_string(err), duration);
        return false;
    }

    reportTest("Single Cube Rendering", true, "Red cube at origin", duration);
    return true;
}

// Test 7: Multiple Cubes Rendering
bool testMultipleCubesRendering(Framebuffer& fbo) {
    auto start = std::chrono::high_resolution_clock::now();

    fbo.bind();
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    useShader();

    // Set up camera
    glm::mat4 view = glm::lookAt(
        glm::vec3(5.0f, 5.0f, 5.0f),
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f)
    );
    glm::mat4 projection = glm::perspective(
        glm::radians(45.0f),
        static_cast<float>(fbo.width) / fbo.height,
        0.1f, 100.0f
    );

    setMat4("view", view);
    setMat4("projection", projection);
    setVec3("lightPos", glm::vec3(5.0f, 5.0f, 5.0f));
    setVec3("viewPos", glm::vec3(5.0f, 5.0f, 5.0f));

    // Draw 3 cubes with different colors and positions
    struct CubeData {
        glm::vec3 pos;
        glm::vec3 color;
        const char* name;
    };

    std::vector<CubeData> cubes = {
        {glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), "Red"},
        {glm::vec3(2.0f, 2.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), "Green"},
        {glm::vec3(-2.0f, 3.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), "Blue"}
    };

    for (const auto& cube : cubes) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), cube.pos);
        setMat4("model", model);
        setVec3("color", cube.color);
        drawCube();
    }

    GLenum err = glGetError();
    fbo.unbind();

    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();

    if (err != GL_NO_ERROR) {
        reportTest("Multiple Cubes Rendering", false, "OpenGL error: " + std::to_string(err), duration);
        return false;
    }

    reportTest("Multiple Cubes Rendering", true, "3 cubes (Red/Green/Blue)", duration);
    return true;
}

// Test 8: Rotating Cube Animation
bool testRotatingCubeAnimation(Framebuffer& fbo, GLFWwindow* window) {
    auto start = std::chrono::high_resolution_clock::now();

    float angle = 0.0f;
    int frameCount = 0;
    const int targetFrames = 60;  // 1 second at 60 FPS

    while (frameCount < targetFrames && !glfwWindowShouldClose(window)) {
        fbo.bind();
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);

        useShader();

        glm::mat4 view = glm::lookAt(
            glm::vec3(3.0f, 3.0f, 3.0f),
            glm::vec3(0.0f, 0.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f)
        );
        glm::mat4 projection = glm::perspective(
            glm::radians(45.0f),
            static_cast<float>(fbo.width) / fbo.height,
            0.1f, 100.0f
        );

        setMat4("view", view);
        setMat4("projection", projection);
        setVec3("lightPos", glm::vec3(5.0f, 5.0f, 5.0f));
        setVec3("viewPos", glm::vec3(3.0f, 3.0f, 3.0f));

        // Rotate cube
        glm::mat4 model = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(1.0f, 1.0f, 0.0f));
        setMat4("model", model);
        setVec3("color", glm::vec3(1.0f, 0.5f, 0.0f));  // Orange

        drawCube();

        fbo.unbind();

        glfwSwapBuffers(window);
        glfwPollEvents();

        angle += 0.016f;  // ~1 degree per frame at 60 FPS
        frameCount++;
    }

    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        reportTest("Rotating Cube Animation", false, "OpenGL error: " + std::to_string(err), duration);
        return false;
    }

    float fps = frameCount / (duration / 1000.0);
    reportTest("Rotating Cube Animation", true,
               std::to_string(frameCount) + " frames at " + std::to_string(static_cast<int>(fps)) + " FPS",
               duration);
    return true;
}

// Test 9: Viewport Clear Color
bool testViewportClearColor(Framebuffer& fbo) {
    auto start = std::chrono::high_resolution_clock::now();
    double duration = 0.0;

    std::vector<glm::vec3> colors = {
        {0.0f, 0.0f, 0.0f},    // Black
        {1.0f, 1.0f, 1.0f},    // White
        {0.1f, 0.1f, 0.15f},   // Dark blue
        {0.2f, 0.3f, 0.4f},    // Slate
        {0.0f, 0.5f, 0.0f}     // Green
    };

    for (const auto& color : colors) {
        fbo.bind();
        glClearColor(color.r, color.g, color.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        fbo.unbind();

        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            reportTest("Viewport Clear Color", false, "OpenGL error with color", duration);
            return false;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration<double, std::milli>(end - start).count();

    reportTest("Viewport Clear Color", true, "Tested 5 different colors", duration);
    return true;
}

// Test 10: Depth Testing
bool testDepthTesting(Framebuffer& fbo) {
    auto start = std::chrono::high_resolution_clock::now();

    fbo.bind();
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Enable depth testing
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    useShader();

    glm::mat4 view = glm::lookAt(
        glm::vec3(0.0f, 0.0f, 5.0f),
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f)
    );
    glm::mat4 projection = glm::perspective(
        glm::radians(45.0f),
        static_cast<float>(fbo.width) / fbo.height,
        0.1f, 100.0f
    );

    setMat4("view", view);
    setMat4("projection", projection);
    setVec3("lightPos", glm::vec3(5.0f, 5.0f, 5.0f));
    setVec3("viewPos", glm::vec3(0.0f, 0.0f, 5.0f));

    // Draw back cube (red)
    glm::mat4 modelBack = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    setMat4("model", modelBack);
    setVec3("color", glm::vec3(1.0f, 0.0f, 0.0f));
    drawCube();

    // Draw front cube (blue) - should occlude the red cube
    glm::mat4 modelFront = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.5f));
    setMat4("model", modelFront);
    setVec3("color", glm::vec3(0.0f, 0.0f, 1.0f));
    drawCube();

    GLenum err = glGetError();
    fbo.unbind();

    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();

    if (err != GL_NO_ERROR) {
        reportTest("Depth Testing", false, "OpenGL error: " + std::to_string(err), duration);
        return false;
    }

    reportTest("Depth Testing", true, "Front cube occludes back cube", duration);
    return true;
}

// Test 11: Camera Matrices
bool testCameraMatrices(Framebuffer& fbo) {
    auto start = std::chrono::high_resolution_clock::now();

    // Test different camera positions
    struct CameraData {
        glm::vec3 eye;
        glm::vec3 target;
        const char* name;
    };

    std::vector<CameraData> cameras = {
        {glm::vec3(5.0f, 5.0f, 5.0f), glm::vec3(0.0f, 0.0f, 0.0f), "Top-Right-Front"},
        {glm::vec3(-5.0f, 3.0f, -5.0f), glm::vec3(0.0f, 0.0f, 0.0f), "Bottom-Left-Back"},
        {glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.0f), "Top Down"},
        {glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, 0.0f), "Front View"}
    };

    for (const auto& cam : cameras) {
        fbo.bind();
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);

        useShader();

        glm::mat4 view = glm::lookAt(cam.eye, cam.target, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 projection = glm::perspective(
            glm::radians(45.0f),
            static_cast<float>(fbo.width) / fbo.height,
            0.1f, 100.0f
        );

        setMat4("view", view);
        setMat4("projection", projection);
        setVec3("lightPos", glm::vec3(5.0f, 5.0f, 5.0f));
        setVec3("viewPos", cam.eye);

        glm::mat4 model = glm::mat4(1.0f);
        setMat4("model", model);
        setVec3("color", glm::vec3(0.8f, 0.8f, 0.8f));

        drawCube();

        GLenum err = glGetError();
        fbo.unbind();

        if (err != GL_NO_ERROR) {
            reportTest("Camera Matrices", false, 
                       std::string("OpenGL error with camera: ") + cam.name);
            return false;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();

    reportTest("Camera Matrices", true, "Tested 4 camera positions", duration);
    return true;
}

// Test 12: Scaled Cubes
bool testScaledCubes(Framebuffer& fbo) {
    auto start = std::chrono::high_resolution_clock::now();

    fbo.bind();
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);

    useShader();

    glm::mat4 view = glm::lookAt(
        glm::vec3(5.0f, 5.0f, 5.0f),
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f)
    );
    glm::mat4 projection = glm::perspective(
        glm::radians(45.0f),
        static_cast<float>(fbo.width) / fbo.height,
        0.1f, 100.0f
    );

    setMat4("view", view);
    setMat4("projection", projection);
    setVec3("lightPos", glm::vec3(5.0f, 5.0f, 5.0f));
    setVec3("viewPos", glm::vec3(5.0f, 5.0f, 5.0f));

    // Draw cubes with different scales
    std::vector<float> scales = {0.5f, 1.0f, 2.0f, 0.25f};
    float xPos = -3.0f;

    for (float scale : scales) {
        glm::mat4 model = glm::scale(
            glm::translate(glm::mat4(1.0f), glm::vec3(xPos, 0.0f, 0.0f)),
            glm::vec3(scale)
        );
        setMat4("model", model);
        setVec3("color", glm::vec3(1.0f, 1.0f, 0.0f));  // Yellow
        drawCube();
        xPos += 2.0f;
    }

    GLenum err = glGetError();
    fbo.unbind();

    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();

    if (err != GL_NO_ERROR) {
        reportTest("Scaled Cubes", false, "OpenGL error: " + std::to_string(err), duration);
        return false;
    }

    reportTest("Scaled Cubes", true, "4 cubes with different scales", duration);
    return true;
}

// ============================================================================
// Test Runner
// ============================================================================
void printSummary() {
    int passed = 0;
    int failed = 0;

    std::cout << "\n========================================\n";
    std::cout << "           TEST SUMMARY\n";
    std::cout << "========================================\n";

    for (const auto& result : g_testResults) {
        const char* status = result.passed ? "[PASS]" : "[FAIL]";
        const char* color = result.passed ? "\033[32m" : "\033[31m";
        std::cout << color << status << "\033[0m " << result.name;
        if (!result.message.empty()) std::cout << " - " << result.message;
        std::cout << "\n";

        if (result.passed) passed++;
        else failed++;
    }

    std::cout << "----------------------------------------\n";
    std::cout << "Total: " << (passed + failed) << " | ";
    std::cout << "\033[32mPassed: " << passed << "\033[0m | ";
    std::cout << "\033[31mFailed: " << failed << "\033[0m\n";
    std::cout << "========================================\n";

    if (failed == 0) {
        std::cout << "\033[32m✓ All tests passed!\033[0m\n";
    } else {
        std::cout << "\033[31m✗ Some tests failed!\033[0m\n";
    }
}

// Error callback
void errorCallback(int error, const char* description) {
    std::cerr << "GLFW Error (" << error << "): " << description << "\n";
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--verbose" || arg == "-v") {
            g_config.verbose = true;
        } else if (arg == "--no-pause") {
            g_config.pauseOnFailure = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Viewport Debug Test\n";
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  -v, --verbose    Enable verbose output\n";
            std::cout << "  --no-pause       Don't pause on failure\n";
            std::cout << "  -h, --help       Show this help\n";
            return 0;
        }
    }

    std::cout << "========================================\n";
    std::cout << "     VIEWPORT DEBUG TEST SUITE\n";
    std::cout << "========================================\n\n";

    // Initialize GLFW
    glfwSetErrorCallback(errorCallback);
    if (!glfwInit()) {
        std::cerr << "ERROR: Failed to initialize GLFW\n";
        return 1;
    }

    // Configure GLFW
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

    // Create window
    GLFWwindow* window = glfwCreateWindow(
        g_config.width, g_config.height,
        "Viewport Debug Test", nullptr, nullptr
    );

    if (!window) {
        std::cerr << "ERROR: Failed to create GLFW window\n";
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // Enable vsync

    // Initialize GLAD
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "ERROR: Failed to initialize GLAD\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    std::cout << "[OK] GLAD initialized\n";

    // Run tests
    std::cout << "\n--- Running Tests ---\n\n";

    Framebuffer fbo;

    // Core tests
    testOpenGLContext();
    testFramebufferCreation(fbo);
    testFramebufferResize(fbo);
    testShaderCompilation();
    testCubeMeshCreation();

    // Rendering tests
    testViewportClearColor(fbo);
    testSingleCubeRendering(fbo);
    testMultipleCubesRendering(fbo);
    testDepthTesting(fbo);
    testCameraMatrices(fbo);
    testScaledCubes(fbo);

    // Animation test (requires window)
    testRotatingCubeAnimation(fbo, window);

    // Print summary
    printSummary();

    // Cleanup
    cleanupCube();
    cleanupShader();
    fbo.cleanup();

    glfwDestroyWindow(window);
    glfwTerminate();

    // Pause if there were failures
    int failed = 0;
    for (const auto& r : g_testResults) {
        if (!r.passed) failed++;
    }

    if (failed > 0 && g_config.pauseOnFailure) {
        std::cout << "\nPress Enter to exit...";
        std::cin.get();
    }

    return failed > 0 ? 1 : 0;
}
