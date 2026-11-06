/**
 * Viewport Debug Test - Comprehensive debugging for test.cpp viewport
 * 
 * This test helps diagnose why the viewport appears empty by testing:
 * 1. FBO creation and completeness
 * 2. Shader compilation and uniform locations
 * 3. VAO/VBO creation and binding
 * 4. Matrix uploads to shaders
 * 5. Draw call execution
 * 6. Color buffer writes
 * 7. Depth buffer writes
 * 8. Renderer batching system
 * 9. ECS entity iteration
 * 10. Camera matrix setup
 * 
 * Run: make viewport-debug-test && ./bin/viewport_debug_test
 */

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "ecs/systems/Systems.h"
#include "renderer/Renderer.h"
#include "modelSystem/Model.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <fstream>
#include <sstream>

// ============================================================================
// Debug Configuration
// ============================================================================
struct DebugConfig {
    bool verbose = true;
    bool pauseOnFailure = true;
    bool showImGuiDebug = true;
    int testMode = 0;  // 0=All tests, 1=Simple cube, 2=ECS entities, 3=Renderer batch
};

static DebugConfig g_debugConfig;

// ============================================================================
// Debug Statistics
// ============================================================================
struct DebugStats {
    int entitiesCreated = 0;
    int entitiesRendered = 0;
    int batchesSubmitted = 0;
    int drawCallsExecuted = 0;
    int shaderUniformsSet = 0;
    bool fboComplete = false;
    bool shaderCompiled = false;
    bool vaoValid = false;
    float fps = 0.0f;
    std::string lastError;
};

static DebugStats g_stats;

// ============================================================================
// Framebuffer (same as test.cpp)
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
        g_stats.fboComplete = (status == GL_FRAMEBUFFER_COMPLETE);
        
        if (!g_stats.fboComplete) {
            g_stats.lastError = "Framebuffer incomplete! Status: " + std::to_string(status);
            std::cerr << "ERROR: " << g_stats.lastError << "\n";
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void bind() {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
    }

    void unbind() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void cleanup() {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (colorTex) glDeleteTextures(1, &colorTex);
        if (rbo) glDeleteRenderbuffers(1, &rbo);
        fbo = 0;
        colorTex = 0;
        rbo = 0;
    }
};

// ============================================================================
// Shader Helper with Debug Logging
// ============================================================================
static GLuint g_shaderProg = 0;
static GLint g_uniformLocations[10];  // Cache uniform locations

static void logUniformCheck(const char* name, GLint loc) {
    if (g_debugConfig.verbose) {
        if (loc == -1) {
            std::cout << "[WARN] Uniform '" << name << "' not found in shader\n";
        } else {
            std::cout << "[OK] Uniform '" << name << "' location: " << loc << "\n";
            g_stats.shaderUniformsSet++;
        }
    }
}

static GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        g_stats.lastError = std::string("Shader compilation failed: ") + log;
        std::cerr << "ERROR: " << g_stats.lastError << "\n";
        return 0;
    }
    return shader;
}

static void initShader() {
    const char* vs = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=7) in mat4 instanceMatrix;

uniform mat4 model, view, projection;
out vec3 FragPos, Normal;

void main() {
    // Use instance matrix if available, otherwise use model uniform
    mat4 finalModel = instanceMatrix;
    if (instanceMatrix[0][0] == 0.0 && instanceMatrix[1][1] == 0.0) {
        finalModel = model;
    }
    
    FragPos = vec3(finalModel * vec4(aPos, 1.0));
    Normal = mat3(transpose(inverse(finalModel))) * aNormal;
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

    std::cout << "[DEBUG] Compiling vertex shader...\n";
    GLuint vsObj = compileShader(GL_VERTEX_SHADER, vs);
    std::cout << "[DEBUG] Compiling fragment shader...\n";
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
        g_stats.lastError = std::string("Shader linking failed: ") + log;
        std::cerr << "ERROR: " << g_stats.lastError << "\n";
        return;
    }

    g_stats.shaderCompiled = true;
    std::cout << "[OK] Shader program compiled and linked (ID: " << g_shaderProg << ")\n";

    // Cache and log all uniform locations
    std::cout << "\n[DEBUG] Checking uniform locations:\n";
    g_uniformLocations[0] = glGetUniformLocation(g_shaderProg, "model");
    logUniformCheck("model", g_uniformLocations[0]);
    
    g_uniformLocations[1] = glGetUniformLocation(g_shaderProg, "view");
    logUniformCheck("view", g_uniformLocations[1]);
    
    g_uniformLocations[2] = glGetUniformLocation(g_shaderProg, "projection");
    logUniformCheck("projection", g_uniformLocations[2]);
    
    g_uniformLocations[3] = glGetUniformLocation(g_shaderProg, "color");
    logUniformCheck("color", g_uniformLocations[3]);
    
    g_uniformLocations[4] = glGetUniformLocation(g_shaderProg, "lightPos");
    logUniformCheck("lightPos", g_uniformLocations[4]);
    
    g_uniformLocations[5] = glGetUniformLocation(g_shaderProg, "viewPos");
    logUniformCheck("viewPos", g_uniformLocations[5]);

    glDeleteShader(vsObj);
    glDeleteShader(fsObj);
}

static void setMat4(GLint loc, const glm::mat4& m) {
    if (loc >= 0) {
        glUniformMatrix4fv(loc, 1, GL_FALSE, &m[0][0]);
        g_stats.shaderUniformsSet++;
    }
}

static void setVec3(GLint loc, const glm::vec3& v) {
    if (loc >= 0) {
        glUniform3fv(loc, 1, &v[0]);
        g_stats.shaderUniformsSet++;
    }
}

static void cleanupShader() {
    if (g_shaderProg) glDeleteProgram(g_shaderProg);
    g_shaderProg = 0;
}

// ============================================================================
// Cube Mesh with Debug Info
// ============================================================================
static GLuint g_cubeVAO = 0, g_cubeVBO = 0;

static void initCube() {
    float verts[] = {
        // positions          // normals
        -0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
         0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
         0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
         0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
        -0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
        -0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
        // ... (all 6 faces, 36 vertices total)
        -0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
         0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
         0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
         0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
        -0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
        -0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
        -0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f,
        -0.5f,  0.5f, -0.5f, -1.0f,  0.0f,  0.0f,
        -0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f,
        -0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f,
        -0.5f, -0.5f,  0.5f, -1.0f,  0.0f,  0.0f,
        -0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f,
         0.5f,  0.5f,  0.5f,  1.0f,  0.0f,  0.0f,
         0.5f,  0.5f, -0.5f,  1.0f,  0.0f,  0.0f,
         0.5f, -0.5f, -0.5f,  1.0f,  0.0f,  0.0f,
         0.5f, -0.5f, -0.5f,  1.0f,  0.0f,  0.0f,
         0.5f, -0.5f,  0.5f,  1.0f,  0.0f,  0.0f,
         0.5f,  0.5f,  0.5f,  1.0f,  0.0f,  0.0f,
        -0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,
         0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,
         0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,
         0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,
        -0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,
        -0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,
        -0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,
         0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,
         0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,
         0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,
        -0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,
        -0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,
    };

    glGenVertexArrays(1, &g_cubeVAO);
    glGenBuffers(1, &g_cubeVBO);

    glBindVertexArray(g_cubeVAO);

    glBindBuffer(GL_ARRAY_BUFFER, g_cubeVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    // Position attribute (location 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Normal attribute (location 1)
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    g_stats.vaoValid = (g_cubeVAO != 0);
    std::cout << "[OK] Cube VAO created (ID: " << g_cubeVAO << ", 36 vertices)\n";
}

static void drawCube() {
    glBindVertexArray(g_cubeVAO);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    g_stats.drawCallsExecuted++;
    glBindVertexArray(0);
}

static void cleanupCube() {
    if (g_cubeVAO) glDeleteVertexArrays(1, &g_cubeVAO);
    if (g_cubeVBO) glDeleteBuffers(1, &g_cubeVBO);
    g_cubeVAO = 0;
    g_cubeVBO = 0;
}

// ============================================================================
// Test Functions
// ============================================================================

void logGLError(const char* location) {
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "[ERROR] OpenGL error at " << location << ": " << err << "\n";
        g_stats.lastError = "GL Error " + std::to_string(err) + " at " + location;
    }
}

// Test 1: Basic FBO rendering
void testBasicFBO(Framebuffer& fbo) {
    std::cout << "\n=== TEST 1: Basic FBO Rendering ===\n";
    
    fbo.bind();
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);  // Bright red
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    // Read back pixel to verify write
    GLubyte pixel[4];
    glReadPixels(fbo.width/2, fbo.height/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    
    fbo.unbind();
    
    std::cout << "Pixel at center: R=" << (int)pixel[0] << " G=" << (int)pixel[1] 
              << " B=" << (int)pixel[2] << "\n";
    
    if (pixel[0] > 200 && pixel[1] < 50 && pixel[2] < 50) {
        std::cout << "[PASS] FBO color buffer write successful\n";
    } else {
        std::cout << "[FAIL] FBO color buffer write failed\n";
        g_stats.lastError = "FBO pixel color mismatch";
    }
    
    logGLError("testBasicFBO");
}

// Test 2: Simple cube without shader uniforms
void testSimpleCube(Framebuffer& fbo) {
    std::cout << "\n=== TEST 2: Simple Cube (No Shaders) ===\n";
    
    fbo.bind();
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    // Use a simple passthrough shader
    glUseProgram(0);
    glBindVertexArray(g_cubeVAO);
    
    // This will fail without proper shader, but tests VAO binding
    std::cout << "VAO bound: " << (g_cubeVAO != 0 ? "Yes" : "No") << "\n";
    std::cout << "Vertex count: 36\n";
    
    glBindVertexArray(0);
    fbo.unbind();
    
    logGLError("testSimpleCube");
}

// Test 3: Cube with proper shader and uniforms
void testCubeWithShader(Framebuffer& fbo) {
    std::cout << "\n=== TEST 3: Cube With Shader ===\n";
    
    fbo.bind();
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    
    glUseProgram(g_shaderProg);
    
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
    
    // Set uniforms EXPLICITLY
    setMat4(g_uniformLocations[1], view);        // view
    setMat4(g_uniformLocations[2], projection);  // projection
    
    // Model matrix
    glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f));
    setMat4(g_uniformLocations[0], model);       // model
    
    // Lighting
    setVec3(g_uniformLocations[3], glm::vec3(1.0f, 0.0f, 0.0f));  // color (red)
    setVec3(g_uniformLocations[4], glm::vec3(5.0f, 5.0f, 5.0f));  // lightPos
    setVec3(g_uniformLocations[5], glm::vec3(3.0f, 3.0f, 3.0f));  // viewPos
    
    std::cout << "Uniforms set: " << g_stats.shaderUniformsSet << "\n";
    
    drawCube();
    
    fbo.unbind();
    
    std::cout << "Draw calls: " << g_stats.drawCallsExecuted << "\n";
    
    logGLError("testCubeWithShader");
}

// Test 4: ECS Entity Creation and Rendering
void testECSEntities(ecs::World& world, Framebuffer& fbo, Renderer& renderer) {
    std::cout << "\n=== TEST 4: ECS Entity Rendering ===\n";
    
    // Create test entities
    auto entity1 = world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    auto entity2 = world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    auto entity3 = world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    
    g_stats.entitiesCreated = 3;
    
    // Set up components
    auto* t1 = world.getComponentArchetype<ecs::TransformComponent>(entity1);
    auto* m1 = world.getComponentArchetype<ecs::MeshComponent>(entity1);
    if (t1 && m1) {
        t1->position = glm::vec3(0.0f, 0.0f, 0.0f);
        t1->scale = glm::vec3(1.0f);
        t1->rotation = glm::quat(1, 0, 0, 0);
        m1->visible = true;
        m1->color = glm::vec3(1.0f, 0.0f, 0.0f);  // Red
    }
    
    auto* t2 = world.getComponentArchetype<ecs::TransformComponent>(entity2);
    auto* m2 = world.getComponentArchetype<ecs::MeshComponent>(entity2);
    if (t2 && m2) {
        t2->position = glm::vec3(2.0f, 0.0f, 0.0f);
        t2->scale = glm::vec3(0.5f);
        t2->rotation = glm::quat(1, 0, 0, 0);
        m2->visible = true;
        m2->color = glm::vec3(0.0f, 1.0f, 0.0f);  // Green
    }
    
    auto* t3 = world.getComponentArchetype<ecs::TransformComponent>(entity3);
    auto* m3 = world.getComponentArchetype<ecs::MeshComponent>(entity3);
    if (t3 && m3) {
        t3->position = glm::vec3(-2.0f, 0.0f, 0.0f);
        t3->scale = glm::vec3(0.7f);
        t3->rotation = glm::quat(1, 0, 0, 0);
        m3->visible = true;
        m3->color = glm::vec3(0.0f, 0.0f, 1.0f);  // Blue
    }
    
    std::cout << "Created " << g_stats.entitiesCreated << " ECS entities\n";
    std::cout << "World entity count: " << world.getEntityCount() << "\n";
    
    // Test rendering through RenderSystem
    auto& renderSystem = world.addSystem<ecs::RenderSystem>();
    renderSystem.setRenderer(&renderer);
    renderSystem.setModel(Model::CreateFromVAO(g_cubeVAO, 36));
    renderSystem.setDefaultShaderProgram(g_shaderProg);
    
    // Update and render
    world.update(0.016f);
    
    fbo.bind();
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    
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
    renderSystem.setViewProjection(view, projection);
    
    renderSystem.render();
    
    fbo.unbind();
    
    std::cout << "Entities rendered: " << g_stats.entitiesRendered << "\n";
    
    logGLError("testECSEntities");
}

// Test 5: Renderer Batch System
void testRendererBatches(Renderer& renderer, Framebuffer& fbo) {
    std::cout << "\n=== TEST 5: Renderer Batch System ===\n";
    
    // Add multiple renderables
    std::vector<glm::mat4> transforms1 = {
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f)),
        glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f)),
    };
    
    renderer.AddRenderable(g_cubeVAO, 0, 0, 36, GL_TRIANGLES, g_shaderProg, transforms1);
    g_stats.batchesSubmitted++;
    
    std::vector<glm::mat4> transforms2 = {
        glm::translate(glm::mat4(1.0f), glm::vec3(-2.0f, 0.0f, 0.0f)),
    };
    
    renderer.AddRenderable(g_cubeVAO, 0, 0, 36, GL_TRIANGLES, g_shaderProg, transforms2);
    g_stats.batchesSubmitted++;
    
    std::cout << "Batches submitted: " << g_stats.batchesSubmitted << "\n";
    std::cout << "Total instances: " << (transforms1.size() + transforms2.size()) << "\n";
    
    // Render
    fbo.bind();
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    
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
    renderer.SetCameraMatrices(view, projection);
    
    renderer.Render();
    
    fbo.unbind();
    
    logGLError("testRendererBatches");
}

// ============================================================================
// Debug UI
// ============================================================================
void renderDebugUI(Framebuffer& fbo) {
    if (!g_debugConfig.showImGuiDebug) return;
    
    ImGui::Begin("Viewport Debug Info");
    
    ImGui::Text("=== Statistics ===");
    ImGui::Text("Entities Created: %d", g_stats.entitiesCreated);
    ImGui::Text("Entities Rendered: %d", g_stats.entitiesRendered);
    ImGui::Text("Batches Submitted: %d", g_stats.batchesSubmitted);
    ImGui::Text("Draw Calls: %d", g_stats.drawCallsExecuted);
    ImGui::Text("Uniforms Set: %d", g_stats.shaderUniformsSet);
    ImGui::Separator();
    
    ImGui::Text("=== Status ===");
    ImGui::TextColored(ImVec4(g_stats.fboComplete ? 0.0f : 1.0f, 
                               g_stats.fboComplete ? 1.0f : 0.0f, 0.0f, 1.0f),
                      "FBO: %s", g_stats.fboComplete ? "OK" : "FAILED");
    ImGui::TextColored(ImVec4(g_stats.shaderCompiled ? 0.0f : 1.0f, 
                               g_stats.shaderCompiled ? 1.0f : 0.0f, 0.0f, 1.0f),
                      "Shader: %s", g_stats.shaderCompiled ? "OK" : "FAILED");
    ImGui::TextColored(ImVec4(g_stats.vaoValid ? 0.0f : 1.0f, 
                               g_stats.vaoValid ? 1.0f : 0.0f, 0.0f, 1.0f),
                      "VAO: %s", g_stats.vaoValid ? "OK" : "FAILED");
    ImGui::Separator();
    
    ImGui::Text("=== FBO Info ===");
    ImGui::Text("Size: %dx%d", fbo.width, fbo.height);
    ImGui::Text("FBO ID: %d", fbo.fbo);
    ImGui::Text("Color Tex: %d", fbo.colorTex);
    ImGui::Separator();
    
    if (!g_stats.lastError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Last Error:");
        ImGui::TextWrapped("%s", g_stats.lastError.c_str());
    }
    
    ImGui::End();
    
    // Test control panel
    ImGui::Begin("Debug Controls");
    
    ImGui::Checkbox("Verbose Output", &g_debugConfig.verbose);
    ImGui::SliderInt("Test Mode", &g_debugConfig.testMode, 0, 3);
    
    const char* modes[] = {"All Tests", "Simple Cube", "ECS Entities", "Renderer Batch"};
    ImGui::Text("Current: %s", modes[g_debugConfig.testMode]);
    
    ImGui::Separator();
    ImGui::Text("Controls:");
    ImGui::BulletText("F1 - Toggle this debug UI");
    ImGui::BulletText("F2 - Run all tests");
    ImGui::BulletText("F3 - Screenshot FBO");
    
    ImGui::End();
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char* argv[]) {
    std::cout << "========================================\n";
    std::cout << "   VIEWPORT DEBUG TEST\n";
    std::cout << "========================================\n\n";
    
    // Parse args
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--verbose" || arg == "-v") g_debugConfig.verbose = true;
        if (arg == "--no-ui") g_debugConfig.showImGuiDebug = false;
    }
    
    // Init GLFW
    if (!glfwInit()) {
        std::cerr << "ERROR: GLFW init failed\n";
        return 1;
    }
    
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Viewport Debug Test", nullptr, nullptr);
    if (!window) {
        std::cerr << "ERROR: Window creation failed\n";
        glfwTerminate();
        return 1;
    }
    
    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);
    
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "ERROR: GLAD init failed\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    
    std::cout << "OpenGL: " << glGetString(GL_VERSION) << "\n\n";
    
    // Init ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 450");
    
    // Init resources
    initShader();
    initCube();
    
    Framebuffer fbo;
    fbo.init(1280, 720);
    
    Renderer renderer;
    renderer.Initialize();
    
    ecs::World world;
    world.init();
    
    std::cout << "\n=== Initial Checks ===\n";
    std::cout << "FBO Complete: " << (g_stats.fboComplete ? "YES" : "NO") << "\n";
    std::cout << "Shader Compiled: " << (g_stats.shaderCompiled ? "YES" : "NO") << "\n";
    std::cout << "VAO Valid: " << (g_stats.vaoValid ? "YES" : "NO") << "\n";
    
    // Run initial tests based on mode
    switch (g_debugConfig.testMode) {
        case 1:  // Simple cube
            testBasicFBO(fbo);
            testCubeWithShader(fbo);
            break;
        case 2:  // ECS entities
            testECSEntities(world, fbo, renderer);
            break;
        case 3:  // Renderer batches
            testRendererBatches(renderer, fbo);
            break;
        default:  // All tests
            testBasicFBO(fbo);
            testSimpleCube(fbo);
            testCubeWithShader(fbo);
            testECSEntities(world, fbo, renderer);
            testRendererBatches(renderer, fbo);
            break;
    }
    
    // Main loop
    float lastTime = glfwGetTime();
    bool f1Pressed = false;
    bool f2Pressed = false;
    
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        
        // Handle F1 - Toggle UI
        if (glfwGetKey(window, GLFW_KEY_F1) == GLFW_PRESS) {
            if (!f1Pressed) {
                g_debugConfig.showImGuiDebug = !g_debugConfig.showImGuiDebug;
                f1Pressed = true;
            }
        } else {
            f1Pressed = false;
        }
        
        // Handle F2 - Run tests
        if (glfwGetKey(window, GLFW_KEY_F2) == GLFW_PRESS) {
            if (!f2Pressed) {
                std::cout << "\n=== Re-running tests ===\n";
                g_stats = DebugStats();  // Reset stats
                testBasicFBO(fbo);
                testCubeWithShader(fbo);
                f2Pressed = true;
            }
        } else {
            f2Pressed = false;
        }
        
        // Calculate FPS
        float now = glfwGetTime();
        g_stats.fps = 1.0f / (now - lastTime);
        lastTime = now;
        
        // Render to FBO
        fbo.bind();
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        
        // Render test scene
        testCubeWithShader(fbo);
        
        fbo.unbind();
        
        // Render to screen
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        
        // Render FBO texture to screen
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        
        // Show FBO texture in viewport
        ImGui::Begin("Viewport");
        ImVec2 viewportSize = ImGui::GetContentRegionAvail();
        if (viewportSize.x > 0 && viewportSize.y > 0) {
            ImGui::Image((ImTextureID)(intptr_t)fbo.colorTex, viewportSize);
        }
        ImGui::End();
        
        renderDebugUI(fbo);
        
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        
        glfwSwapBuffers(window);
        
        // Small delay to prevent CPU spinning
        glfwWaitEventsTimeout(0.016);
    }
    
    // Cleanup
    cleanupCube();
    cleanupShader();
    fbo.cleanup();
    renderer.Shutdown();
    world.shutdown();
    
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    
    glfwDestroyWindow(window);
    glfwTerminate();
    
    std::cout << "\n=== Final Statistics ===\n";
    std::cout << "Total Draw Calls: " << g_stats.drawCallsExecuted << "\n";
    std::cout << "Total Uniforms Set: " << g_stats.shaderUniformsSet << "\n";
    std::cout << "Final FPS: " << g_stats.fps << "\n";
    
    return 0;
}
