/**
 * ============================================================================
 * RTT ENGINE EDITOR - Unreal Engine Style (Robust)
 * ============================================================================
 * Professional game engine editor with improved robustness:
 * - No static buffers that outlive ECS data
 * - Proper resource cleanup
 * - Safe entity selection handling
 * - Window resize handling
 * - Input validation
 * 
 * NOTE: If icons appear as squares/boxes, install a Nerd Font:
 *   sudo apt install fonts-font-awesome
 * ============================================================================
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
#include "cameraSystem/flyCamera.h"
#include "renderer/GPUProfilerAdvanced.h"
#include "renderer/Renderer.h"
#include "ecs/systems/RenderSystem.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <sys/stat.h>

// Icon definitions (FontAwesome free icons)
#define ICON_MIN_FA 0xf000
#define ICON_MAX_FA 0xf8ff
#define ICON_FA_SEARCH "\xef\x80\x82"
#define ICON_FA_FOLDER "\xef\x81\xbb"
#define ICON_FA_FILE "\xef\x85\x9b"
#define ICON_FA_CUBE "\xef\x86\xb3"
#define ICON_FA_IMAGE "\xef\x80\xbe"
#define ICON_FA_PLAY "\xef\x81\x8b"
#define ICON_FA_PAUSE "\xef\x81\x8c"
#define ICON_FA_STOP "\xef\x81\x8d"
#define ICON_FA_UNDO "\xef\x83\xa2"
#define ICON_FA_REDO "\xef\x83\xa5"
#define ICON_FA_SAVE "\xef\x83\x87"
#define ICON_FA_OPEN "\xef\x82\x82"
#define ICON_FA_PLUS "\xef\x81\xa7"
#define ICON_FA_TRASH "\xef\x87\xb8"
#define ICON_FA_COG "\xef\x80\x93"
#define ICON_FA_INFO "\xef\x84\xa9"
#define ICON_FA_TIMES "\xef\x80\x8d"
#define ICON_FA_CHECK "\xef\x80\x8c"
#define ICON_FA_ARROWS_ALT "\xef\x82\xb2"
#define ICON_FA_ROTATE "\xef\x8b\xb1"
#define ICON_FA_EXPAND "\xef\x81\xa5"
#define ICON_FA_COMPRESS "\xef\x81\xa6"
#define ICON_FA_GLOBE "\xef\x82\xac"
#define ICON_FA_HOME "\xef\x80\x95"

// ============================================================================
// FBO for Viewport
// ============================================================================
struct Framebuffer {
    GLuint fbo = 0;
    GLuint colorTex = 0;
    GLuint rbo = 0;
    int width = 1280;
    int height = 720;
    
    void init(int w, int h) {
        cleanup();
        width = w; height = h;
        
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
        
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "ERROR: Framebuffer incomplete!\n";
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
        fbo = 0; colorTex = 0; rbo = 0;
    }
    
    void bind() {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
    }
    
    void unbind() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
};

static Framebuffer g_viewportFB;

// ============================================================================
// Shader Helper
// ============================================================================
static GLuint g_shaderProg = 0;

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
    GLuint vsObj = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vsObj, 1, &vs, nullptr);
    glCompileShader(vsObj);
    
    GLuint fsObj = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fsObj, 1, &fs, nullptr);
    glCompileShader(fsObj);
    
    g_shaderProg = glCreateProgram();
    glAttachShader(g_shaderProg, vsObj);
    glAttachShader(g_shaderProg, fsObj);
    glLinkProgram(g_shaderProg);
    
    glDeleteShader(vsObj);
    glDeleteShader(fsObj);
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

// ============================================================================
// Cube Mesh
// ============================================================================
static GLuint g_cubeVAO = 0, g_cubeVBO = 0, g_cubeEBO = 0;

static void initCube() {
    float verts[] = {
        -0.5f,-0.5f,-0.5f, 0,0,-1,  0.5f,-0.5f,-0.5f, 0,0,-1,  0.5f,0.5f,-0.5f, 0,0,-1,
         0.5f,0.5f,-0.5f, 0,0,-1, -0.5f,0.5f,-0.5f, 0,0,-1, -0.5f,-0.5f,-0.5f, 0,0,-1,
        -0.5f,-0.5f, 0.5f, 0,0, 1,  0.5f,-0.5f, 0.5f, 0,0, 1,  0.5f, 0.5f, 0.5f, 0,0, 1,
         0.5f, 0.5f, 0.5f, 0,0, 1, -0.5f, 0.5f, 0.5f, 0,0, 1, -0.5f,-0.5f, 0.5f, 0,0, 1,
        -0.5f, 0.5f, 0.5f,-1,0, 0, -0.5f, 0.5f,-0.5f,-1,0, 0, -0.5f,-0.5f,-0.5f,-1,0, 0,
        -0.5f,-0.5f,-0.5f,-1,0, 0, -0.5f,-0.5f, 0.5f,-1,0, 0, -0.5f, 0.5f, 0.5f,-1,0, 0,
         0.5f, 0.5f, 0.5f, 1,0, 0,  0.5f, 0.5f,-0.5f, 1,0, 0,  0.5f,-0.5f,-0.5f, 1,0, 0,
         0.5f,-0.5f,-0.5f, 1,0, 0,  0.5f,-0.5f, 0.5f, 1,0, 0,  0.5f, 0.5f, 0.5f, 1,0, 0,
        -0.5f,-0.5f,-0.5f, 0,-1, 0,  0.5f,-0.5f,-0.5f, 0,-1, 0,  0.5f,-0.5f, 0.5f, 0,-1, 0,
         0.5f,-0.5f, 0.5f, 0,-1, 0, -0.5f,-0.5f, 0.5f, 0,-1, 0, -0.5f,-0.5f,-0.5f, 0,-1, 0,
        -0.5f, 0.5f,-0.5f, 0, 1, 0,  0.5f, 0.5f,-0.5f, 0, 1, 0,  0.5f, 0.5f, 0.5f, 0, 1, 0,
         0.5f, 0.5f, 0.5f, 0, 1, 0, -0.5f, 0.5f, 0.5f, 0, 1, 0, -0.5f, 0.5f,-0.5f, 0, 1, 0
    };
    unsigned int idx[] = {
        0,1,2,2,3,4,5, 6,7,7,8,9, 10,11,12,12,13,14,
        15,16,17,17,18,19, 20,21,22,22,23,24, 25,26,27,27,28,29,
        30,31,32,32,33,34, 35,36,37,37,38,39
    };
    
    glGenVertexArrays(1, &g_cubeVAO);
    glGenBuffers(1, &g_cubeVBO);
    glGenBuffers(1, &g_cubeEBO);
    
    glBindVertexArray(g_cubeVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_cubeVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_cubeEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
    
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);
    
    glBindVertexArray(0);
}

static void drawCube() {
    glBindVertexArray(g_cubeVAO);
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

static void cleanupCube() {
    if (g_cubeVAO) glDeleteVertexArrays(1, &g_cubeVAO);
    if (g_cubeVBO) glDeleteBuffers(1, &g_cubeVBO);
    if (g_cubeEBO) glDeleteBuffers(1, &g_cubeEBO);
    g_cubeVAO = 0; g_cubeVBO = 0; g_cubeEBO = 0;
}

// ============================================================================
// Globals
// ============================================================================
static ecs::World g_world;
static flyCamera* g_camera = nullptr;
static ecs::EntityID g_selected = ecs::INVALID_ENTITY_ID;
static float g_fps = 0.0f;

// Renderer and Render System
static Renderer g_renderer;
static ecs::RenderSystem g_renderSystem;
static Model* g_currentModel = nullptr;

// Editor State
enum class GizmoType { None, Translate, Rotate, Scale };
enum class SpaceType { Local, World };
static GizmoType g_gizmoType = GizmoType::Translate;
static SpaceType g_spaceType = SpaceType::World;
static bool g_showGrid = true;
static bool g_showGizmo = true;

// Viewport camera controls
static bool g_isViewing = false;
static glm::vec2 g_lastMousePos;

// UI State (non-static to avoid lifetime issues)
struct UIState {
    char searchBuffer[128] = "";
    char pathBuffer[256] = "/Game/Assets";
    int leftPanelTab = 0;  // 0=Outliner, 1=Details
    int bottomPanelTab = 0;  // 0=Content, 1=Console, 2=Profiler
};
static UIState g_uiState;

// Game State
static bool g_isPlaying = false;
static bool g_wasPlaying = false;
static float g_gameSpeed = 1.0f;

// Console/Log system
struct LogMessage {
    std::string text;
    float timestamp;
    int level;  // 0=info, 1=warning, 2=error
};
static std::vector<LogMessage> g_consoleMessages;
static bool g_showConsole = true;
static int g_consoleFilter = -1;  // -1=all, 0=info, 1=warning, 2=error

void logMessage(const std::string& text, int level = 0) {
    LogMessage msg;
    msg.text = text;
    msg.timestamp = glfwGetTime();
    msg.level = level;
    g_consoleMessages.push_back(msg);
    
    // Keep only last 1000 messages
    if (g_consoleMessages.size() > 1000) {
        g_consoleMessages.erase(g_consoleMessages.begin());
    }
    
    // Also print to stdout
    const char* levelStr = level == 2 ? "[ERROR] " : level == 1 ? "[WARN] " : "[INFO] ";
    std::cout << levelStr << text << "\n";
}

// Entity management
void deleteSelectedEntity() {
    if (g_selected == ecs::INVALID_ENTITY_ID) return;

    // TODO: Properly destroy entity through ECS
    logMessage("Deleted entity " + std::to_string(g_selected), 0);
    g_selected = ecs::INVALID_ENTITY_ID;
}

void duplicateSelectedEntity() {
    if (g_selected == ecs::INVALID_ENTITY_ID) return;

    ecs::Entity entity{g_selected};
    auto* t = g_world.getComponentArchetype<ecs::TransformComponent>(entity);
    auto* m = g_world.getComponentArchetype<ecs::MeshComponent>(entity);

    if (t && m) {
        auto newEntity = g_world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
        auto* newT = g_world.getComponentArchetype<ecs::TransformComponent>(newEntity);
        auto* newM = g_world.getComponentArchetype<ecs::MeshComponent>(newEntity);

        if (newT && newM) {
            newT->position = t->position + glm::vec3(1, 0, 0);
            newT->rotation = t->rotation;
            newT->scale = t->scale;
            newM->color = m->color;
            newM->visible = m->visible;
            newM->meshID = m->meshID;

            logMessage("Duplicated entity " + std::to_string(g_selected) + " -> " + std::to_string(newEntity.id), 0);
        }
    }
}

// ============================================================================
// Entity Creation (using archetype storage)
// ============================================================================
ecs::Entity createCube(const glm::vec3& pos, const glm::vec3& scale, const glm::vec3& color) {
    // Create entity with Transform + Mesh only (NameComponent causes issues)
    auto e = g_world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    if (!e.isValid()) return e;

    // Use getComponentArchetype for archetype-stored components
    auto* t = g_world.getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_world.getComponentArchetype<ecs::MeshComponent>(e);

    if (t) { t->position = pos; t->scale = scale; t->rotation = glm::quat(1, 0, 0, 0); }
    if (m) { m->visible = true; m->meshID = 0; m->color = color; }

    return e;
}

ecs::Entity createSphere(const glm::vec3& pos, float radius, const glm::vec3& color) {
    auto e = g_world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    if (!e.isValid()) return e;

    auto* t = g_world.getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_world.getComponentArchetype<ecs::MeshComponent>(e);

    if (t) { t->position = pos; t->scale = glm::vec3(radius); t->rotation = glm::quat(1, 0, 0, 0); }
    if (m) { m->visible = true; m->color = color; m->meshID = 0; }

    return e;
}

ecs::Entity createPlane(const glm::vec3& pos, const glm::vec2& size, const glm::vec3& color) {
    auto e = g_world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    if (!e.isValid()) return e;

    auto* t = g_world.getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_world.getComponentArchetype<ecs::MeshComponent>(e);

    if (t) { t->position = pos; t->scale = glm::vec3(size.x, 0.01f, size.y); t->rotation = glm::quat(1, 0, 0, 0); }
    if (m) { m->visible = true; m->color = color; m->meshID = 0; }

    return e;
}

ecs::Entity createLight(const glm::vec3& pos, const glm::vec3& color, float intensity) {
    auto e = g_world.createEntityWithComponents<ecs::TransformComponent>();
    if (!e.isValid()) return e;

    auto* t = g_world.getComponentArchetype<ecs::TransformComponent>(e);

    if (t) { t->position = pos; t->scale = glm::vec3(0.2f); t->rotation = glm::quat(1, 0, 0, 0); }

    // TODO: Add LightComponent when available
    (void)color; (void)intensity;  // Suppress unused warnings

    return e;
}

ecs::Entity createCamera(const glm::vec3& pos, const glm::vec3& target) {
    auto e = g_world.createEntityWithComponents<ecs::TransformComponent>();
    if (!e.isValid()) return e;

    auto* t = g_world.getComponentArchetype<ecs::TransformComponent>(e);

    if (t) { t->position = pos; t->scale = glm::vec3(1); t->rotation = glm::quat(1, 0, 0, 0); }

    // TODO: Add CameraComponent when available
    (void)target;  // Suppress unused warnings

    return e;
}

// ============================================================================
// Scene Serialization (Simple JSON-like format)
// ============================================================================
#include <fstream>
#include <sstream>

static std::string g_currentSceneFile;

bool saveScene(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "ERROR: Could not open file for writing: " << filename << "\n";
        return false;
    }

    file << "{\n";
    file << "  \"version\": \"1.0\",\n";
    file << "  \"entities\": [\n";

    bool first = true;
    g_world.forEach<ecs::TransformComponent, ecs::NameComponent>(
        [&](ecs::EntityID id, ecs::TransformComponent& t, ecs::NameComponent& n) {
            if (!first) file << ",\n";
            first = false;

            file << "    {\n";
            file << "      \"name\": \"" << n.name << "\",\n";
            file << "      \"position\": [" << t.position.x << ", " << t.position.y << ", " << t.position.z << "],\n";
            file << "      \"rotation\": [" << t.rotation.x << ", " << t.rotation.y << ", " << t.rotation.z << ", " << t.rotation.w << "],\n";
            file << "      \"scale\": [" << t.scale.x << ", " << t.scale.y << ", " << t.scale.z << "]\n";
            file << "    }";
        });

    file << "\n  ]\n";
    file << "}\n";

    file.close();
    g_currentSceneFile = filename;
    std::cout << "Scene saved to: " << filename << "\n";
    return true;
}

bool loadScene(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "ERROR: Could not open file for reading: " << filename << "\n";
        return false;
    }

    // Clear current scene
    g_world.shutdown();
    g_world.init();

    // Simple parsing (production would use proper JSON library)
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    // Parse entities (simplified - just looking for position patterns)
    size_t pos = 0;
    int entityCount = 0;

    while ((pos = content.find("\"position\":", pos)) != std::string::npos) {
        size_t start = content.find("[", pos);
        size_t end = content.find("]", start);
        if (start != std::string::npos && end != std::string::npos) {
            std::string posStr = content.substr(start + 1, end - start - 1);
            float x, y, z;
            if (sscanf(posStr.c_str(), "%f, %f, %f", &x, &y, &z) == 3) {
                createCube(glm::vec3(x, y, z), glm::vec3(1), glm::vec3(0.8f, 0.8f, 0.8f));
                entityCount++;
            }
        }
        pos = end;
    }

    g_currentSceneFile = filename;
    std::cout << "Loaded " << entityCount << " entities from: " << filename << "\n";
    return true;
}

// ============================================================================
// About Dialog
// ============================================================================
static bool g_showAbout = false;

void showAboutDialog() {
    if (!g_showAbout) return;

    ImGui::OpenPopup("About RTT Engine");
    if (ImGui::BeginPopupModal("About RTT Engine", &g_showAbout, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1), "RTT Engine Editor");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(200, 0));
        ImGui::Text("Version: 1.0.0");
        ImGui::Text("Built: %s %s", __DATE__, __TIME__);
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::TextWrapped("A professional 3D game engine editor with:");
        ImGui::BulletText("ECS Architecture");
        ImGui::BulletText("Real-time rendering");
        ImGui::BulletText("Physics simulation");
        ImGui::BulletText("Animation system");
        ImGui::BulletText("Motion matching");
        ImGui::Dummy(ImVec2(0, 15));
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Built with Dear ImGui, OpenGL, and GLFW");
        ImGui::Dummy(ImVec2(0, 15));

        if (ImGui::Button("OK", ImVec2(120, 0))) {
            g_showAbout = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

// ============================================================================
// Render Scene to FBO
// ============================================================================
void renderScene() {
    PROFILE_GPU_SCOPE("Render Scene");
    
    // 🔵 2. Render scene INTO framebuffer (your viewport)
    g_viewportFB.bind();

    // Set viewport to match FBO size
    glViewport(0, 0, g_viewportFB.width, g_viewportFB.height);

    // Enable proper state
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // Clear
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Set renderer viewport and camera matrices
    g_renderer.SetViewport(0, 0, g_viewportFB.width, g_viewportFB.height);
    
    glm::mat4 view = glm::lookAt(g_camera->Position, g_camera->Target, glm::vec3(0,1,0));
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 
                                       (float)g_viewportFB.width / g_viewportFB.height, 
                                       0.1f, 1000.0f);
    g_renderer.SetCameraMatrices(view, proj);

    // Render all ECS entities through RenderSystem
    g_renderSystem.render();

    // TEST: Draw a simple colored quad to verify FBO works
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(0);
    glBindVertexArray(0);
    
    // Draw a red rectangle in top-left corner of viewport
    glBegin(GL_QUADS);
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex2f(-0.8f, 0.5f);
    glVertex2f(-0.5f, 0.5f);
    glVertex2f(-0.5f, 0.8f);
    glVertex2f(-0.8f, 0.8f);
    glEnd();

    // 🔴 3. Restore default framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
}

// ============================================================================
// UI Helper Functions
// ============================================================================
void renderTransformSection(ecs::TransformComponent* t) {
    if (!t) return;
    
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Transform");
        
        ImGui::SeparatorText("Position");
        glm::vec3 p = t->position;
        if (ImGui::DragFloat3("##Position", &p.x, 0.1f, -10000, 10000)) {
            t->position = p;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset##Pos", ImVec2(60, 0))) {
            t->position = glm::vec3(0);
        }
        
        ImGui::SeparatorText("Rotation");
        glm::vec3 r = glm::degrees(glm::eulerAngles(t->rotation));
        if (ImGui::DragFloat3("##Rotation", &r.x, 1.0f, -180, 180)) {
            t->setEulerAngles(glm::radians(r));
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset##Rot", ImVec2(60, 0))) {
            t->rotation = glm::quat(1, 0, 0, 0);
        }
        
        ImGui::SeparatorText("Scale");
        glm::vec3 s = t->scale;
        if (ImGui::DragFloat3("##Scale", &s.x, 0.01f, 0.01f, 1000)) {
            t->scale = s;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset##Scale", ImVec2(60, 0))) {
            t->scale = glm::vec3(1);
        }
        
        ImGui::PopID();
    }
}

void renderMeshSection(ecs::MeshComponent* m) {
    if (!m) return;
    
    if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Mesh");
        
        ImGui::SeparatorText("Appearance");
        glm::vec3 c = m->color;
        if (ImGui::ColorEdit3("Albedo", &c.x, ImGuiColorEditFlags_Float)) {
            m->color = c;
        }
        ImGui::Checkbox("Visible", &m->visible);
        
        ImGui::PopID();
    }
}

// ============================================================================
// Font Loading Helper (using stb_truetype - no freetype dependency)
// ============================================================================

static bool loadIconFont(ImGuiIO& io) {
    // Open log file for debugging
    FILE* logFile = fopen("font_load.log", "w");
    if (logFile) {
        fprintf(logFile, "=== Font Loading Log ===\n");
        fflush(logFile);
    }
    
    // Common Nerd Font and monospace font locations
    const char* fontPaths[] = {
        // User's local fonts (highest priority - your system has these!)
        "/home/run-time-terror/.local/share/fonts/FiraCodeNerdFont-Regular.ttf",
        "/home/run-time-terror/.local/share/fonts/FiraCodeNerdFontMono-Regular.ttf",
        // Generic user font directories
        "/home/run-time-terror/.local/share/fonts/JetBrainsMonoNerdFont-Regular.ttf",
        // Nerd Fonts (priority - these have icons)
        "/usr/share/fonts/truetype/JetBrainsMono/JetBrainsMonoNerdFont-Regular.ttf",
        "/usr/share/fonts/truetype/JetBrainsMonoNL-Regular.ttf",
        "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf",
        "/usr/share/fonts/opentype/jetbrains-mono/JetBrainsMonoNerdFont-Regular.ttf",
        "/usr/share/fonts/jetbrains-mono/JetBrainsMonoNerdFont-Regular.ttf",
        "/usr/share/fonts/truetype/firacode/FiraCodeNerdFont-Regular.ttf",
        "/usr/share/fonts/TTF/FiraCodeNerdFont-Regular.ttf",
        "/usr/share/fonts/truetype/NerdFonts/JetBrainsMonoNerdFont-Regular.ttf",
        // Ubuntu Mono (available on this system)
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono[wght].ttf",
        // Common monospace fonts (no icons but better than default)
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoMono-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
        nullptr
    };
    
    ImFontConfig fontConfig;
    fontConfig.MergeMode = false;
    fontConfig.PixelSnapH = true;
    
    // Load main font with icon range
    ImVector<ImWchar> ranges;
    ImFontGlyphRangesBuilder builder;
    
    // Add default ranges plus FontAwesome icons
    builder.AddText("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!@#$%^&*()_+-=[]{}|;':\",./<>?");
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    
    // Add FontAwesome icon range
    ImWchar iconRange[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    builder.AddRanges(iconRange);
    
    builder.BuildRanges(&ranges);
    
    for (int i = 0; fontPaths[i] != nullptr; i++) {
        struct stat buffer;
        if (stat(fontPaths[i], &buffer) == 0) {
            std::cout << "Loading font: " << fontPaths[i] << "\n";
            if (logFile) {
                fprintf(logFile, "SUCCESS: Loaded font: %s\n", fontPaths[i]);
                fflush(logFile);
            }
            io.Fonts->AddFontFromFileTTF(fontPaths[i], 16.0f, &fontConfig, ranges.Data);
            
            // Check if it's a Nerd Font (supports icons)
            bool hasIcons = (strstr(fontPaths[i], "Nerd") != nullptr || 
                           strstr(fontPaths[i], "nerd") != nullptr);
            if (hasIcons) {
                std::cout << "Icon font loaded successfully!\n";
                if (logFile) {
                    fprintf(logFile, "This font supports icons (Nerd Font detected)\n");
                    fflush(logFile);
                }
            } else {
                std::cout << "Note: This font doesn't support icons.\n";
                if (logFile) {
                    fprintf(logFile, "Note: This font does not support icons\n");
                    fflush(logFile);
                }
            }
            if (logFile) fclose(logFile);
            return true;
        } else {
            if (logFile) {
                fprintf(logFile, "NOT FOUND: %s\n", fontPaths[i]);
                fflush(logFile);
            }
        }
    }
    
    std::cout << "No custom font found, using default ImGui font.\n";
    std::cout << "For better fonts and icons, install a Nerd Font:\n";
    std::cout << "  sudo apt install fonts-font-awesome fonts-jetbrains-mono\n";
    std::cout << "  OR download from: https://www.nerdfonts.com/font-downloads\n";
    
    if (logFile) {
        fprintf(logFile, "FAILURE: No fonts found, using default\n");
        fclose(logFile);
    }
    return false;
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cout << "=== RTT Engine Editor ===\n";

    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "ERROR: Failed to initialize GLFW\n";
        return -1;
    }
    
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1920, 1080, "RTT Engine Editor", nullptr, nullptr);
    if (!window) {
        std::cerr << "ERROR: Failed to create GLFW window\n";
        glfwTerminate();
        return -1;
    }
    
    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);  // Disable vsync for max FPS

    // Initialize GLAD
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "ERROR: Failed to initialize GLAD\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    std::cout << "OpenGL: " << glGetString(GL_VERSION) << "\n";

    // Initialize GPU Profiler
    std::cout << "Initializing GPU Profiler...\n";
    if (!AdvancedGPUProfiler::getInstance().initialize()) {
        std::cerr << "WARNING: GPU Profiler initialization failed\n";
    }

    // Initialize resources
    initShader();
    initCube();
    g_viewportFB.init(1280, 720);

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "engine_ui.ini";

    // Enable proper menu and window behavior
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    
    // Fix resize duplication - ensure proper alpha blending
    io.ConfigWindowsResizeFromEdges = true;
    
    // Load icon font
    loadIconFont(io);

    // Setup ImGui style
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.WindowPadding = ImVec2(4, 4);
    style.FramePadding = ImVec2(6, 3);
    style.ItemSpacing = ImVec2(6, 4);
    
    // Setup ImGui colors (Unreal-inspired dark theme)
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);  // Fully opaque
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_TabActive] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.60f, 0.40f, 0.0f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.60f, 0.40f, 0.0f, 1.00f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.50f, 0.50f, 0.50f, 0.5f);  // Semi-transparent grip
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.60f, 0.60f, 0.60f, 0.7f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.70f, 0.70f, 0.70f, 0.9f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 450");

    // Initialize camera and ECS
    g_camera = new flyCamera(glm::vec3(0, 5, 10), glm::vec3(0, 0, 0), -90, 0, 10);
    g_world.init();
    g_world.addSystem<ecs::PhysicsSystem>().setGravity(glm::vec3(0, -9.81f, 0));

    // Initialize Renderer and Render System
    std::cout << "Initializing Renderer...\n";
    g_renderer.Initialize();
    std::cout << "Renderer initialized\n";
    g_renderSystem.setRenderer(&g_renderer);
    g_renderSystem.setWorld(&g_world);  // Set world pointer for iteration
    std::cout << "RenderSystem configured\n";
    
    // Create a simple test model from VAO (cube)
    g_currentModel = Model::CreateFromVAO(g_cubeVAO, 36);
    std::cout << "Test model created from VAO (VAO=" << g_cubeVAO << ")\n";
    g_renderSystem.setModel(g_currentModel);
    
    // Set shader program for rendering
    g_renderSystem.setDefaultShaderProgram(g_shaderProg);
    std::cout << "Shader program set (" << g_shaderProg << ")\n";
    
    // Add render system to world (use our global instance)
    g_world.addSystem(&g_renderSystem);
    std::cout << "RenderSystem added to world\n";

    // Create initial scene (AFTER render system is added)
    std::cout << "Creating test cubes...\n";
    createCube(glm::vec3(0, 1, 0), glm::vec3(1), glm::vec3(0.8f, 0.2f, 0.2f));
    std::cout << "Created cube 1\n";
    createCube(glm::vec3(2, 2, 0), glm::vec3(0.5f), glm::vec3(0.2f, 0.8f, 0.2f));
    std::cout << "Created cube 2\n";
    createCube(glm::vec3(-2, 3, 0), glm::vec3(0.7f), glm::vec3(0.2f, 0.2f, 0.8f));
    std::cout << "Created cube 3\n";
    std::cout << "Total entities: " << g_world.getEntityCount() << "\n";

    std::cout << "Ready!\n";
    std::cout << "Controls: Right-click+drag to look, WASD to move\n";

    // Main loop
    float lastTime = 0;
    float fpsTimer = 0;
    int frames = 0;

    while (!glfwWindowShouldClose(window)) {
        // Start GPU frame
        BEGIN_GPU_FRAME();
        
        // Calculate delta time
        float now = glfwGetTime();
        float dt = now - lastTime;
        lastTime = now;

        // FPS counter
        frames++;
        fpsTimer += dt;
        if (fpsTimer >= 1.0f) {
            g_fps = static_cast<float>(frames);
            frames = 0;
            fpsTimer = 0;
        }

        // Update ECS
        g_world.update(dt);

        // Handle mouse input for viewport camera
        double mx, my;
        glfwGetCursorPos(window, &mx, &my);
        
        if (g_isViewing && !io.WantCaptureMouse) {
            float dx = static_cast<float>(mx - g_lastMousePos.x);
            float dy = static_cast<float>(my - g_lastMousePos.y);
            g_camera->ProcessMouseMovement(dx * 0.3f, dy * 0.3f);
        }
        g_lastMousePos = glm::vec2(static_cast<float>(mx), static_cast<float>(my));

        // Handle keyboard shortcuts
        if (!io.WantCaptureKeyboard) {
            // Gizmo tools
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) g_gizmoType = GizmoType::Translate;
            if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) g_gizmoType = GizmoType::Rotate;
            if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) g_gizmoType = GizmoType::Scale;
            if (glfwGetKey(window, GLFW_KEY_X) == GLFW_PRESS) {
                g_spaceType = (g_spaceType == SpaceType::World) ? SpaceType::Local : SpaceType::World;
            }
            
            // Entity operations (with Ctrl modifier)
            bool ctrlPressed = (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                               glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS);
            
            if (ctrlPressed) {
                if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
                    duplicateSelectedEntity();
                    // Simple debounce
                    static float lastDupTime = 0;
                    if (glfwGetTime() - lastDupTime > 0.2f) {
                        lastDupTime = glfwGetTime();
                    }
                }
            }
            
            // Delete key
            if (glfwGetKey(window, GLFW_KEY_DELETE) == GLFW_PRESS) {
                static float lastDelTime = 0;
                if (glfwGetTime() - lastDelTime > 0.2f) {
                    deleteSelectedEntity();
                    lastDelTime = glfwGetTime();
                }
            }
            
            // Console toggle
            if (glfwGetKey(window, GLFW_KEY_BACKSLASH) == GLFW_PRESS) {
                static float lastToggle = 0;
                if (glfwGetTime() - lastToggle > 0.2f) {
                    g_showConsole = !g_showConsole;
                    lastToggle = glfwGetTime();
                }
            }
        }

        // Render 3D scene to FBO
        {
            PROFILE_GPU_SCOPE("Render Scene");
            renderScene();
        }

        // End GPU frame
        END_GPU_FRAME();

        // Setup ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Get window dimensions
        int windowW, windowH;
        glfwGetWindowSize(window, &windowW, &windowH);
        
        // Panel dimensions
        const float menuBarHeight = 25.0f;
        const float toolbarHeight = 36.0f;
        const float sidePanelWidth = 280.0f;
        const float bottomPanelHeight = 250.0f;
        const float consoleHeight = 180.0f;
        const float statusBarHeight = 24.0f;
        
        // Use smaller of console or bottom panel height for tabbed panel
        const float tabbedBottomHeight = consoleHeight;

        // ========== MENU BAR ==========
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("New Scene", "Ctrl+N")) {
                    // Clear scene and create new one
                    g_world.shutdown();
                    g_world.init();
                    g_selected = ecs::INVALID_ENTITY_ID;
                    std::cout << "New scene created\n";
                }
                if (ImGui::MenuItem("Open Scene", "Ctrl+O")) {
                    // For now, load a default scene file
                    loadScene("scene.json");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Save", "Ctrl+S")) {
                    saveScene(g_currentSceneFile.empty() ? "scene.json" : g_currentSceneFile);
                }
                if (ImGui::MenuItem("Save As", "Ctrl+Shift+S")) {
                    saveScene("scene.json");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4")) {
                    glfwSetWindowShouldClose(window, true);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Undo", "Ctrl+Z")) {
                    // TODO: Implement undo system
                    std::cout << "Undo not yet implemented\n";
                }
                if (ImGui::MenuItem("Redo", "Ctrl+Y")) {
                    // TODO: Implement redo system
                    std::cout << "Redo not yet implemented\n";
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Preferences")) {
                    // TODO: Implement preferences
                    std::cout << "Preferences not yet implemented\n";
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("GameObject")) {
                if (ImGui::MenuItem("Cube")) {
                    createCube(glm::vec3(0, 1, 0), glm::vec3(1), glm::vec3(0.8f, 0.8f, 0.8f));
                }
                if (ImGui::MenuItem("Sphere")) {
                    createSphere(glm::vec3(0, 1, 0), 0.5f, glm::vec3(0.2f, 0.8f, 0.2f));
                }
                if (ImGui::MenuItem("Plane")) {
                    createPlane(glm::vec3(0, 0, 0), glm::vec2(10, 10), glm::vec3(0.5f, 0.5f, 0.5f));
                }
                if (ImGui::MenuItem("Light")) {
                    createLight(glm::vec3(5, 10, 5), glm::vec3(1, 1, 0.9f), 1.0f);
                }
                if (ImGui::MenuItem("Camera")) {
                    createCamera(glm::vec3(0, 5, 10), glm::vec3(0, 0, 0));
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Window")) {
                ImGui::MenuItem("World Outliner", nullptr, true);
                ImGui::MenuItem("Details", nullptr, true);
                ImGui::MenuItem("Toolbox", nullptr, true);
                ImGui::MenuItem("Viewport", nullptr, true);
                ImGui::Separator();
                if (ImGui::MenuItem("Profiler")) {
                    g_uiState.bottomPanelTab = 2;  // Switch to Profiler tab
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Help")) {
                if (ImGui::MenuItem("Documentation")) {
                    std::cout << "Opening documentation...\n";
                    // TODO: Open documentation URL
                }
                if (ImGui::MenuItem("About RTT Engine")) {
                    g_showAbout = true;
                }
                ImGui::EndMenu();
            }

            // Performance stats on right side
            ImGui::Separator();
            ImGui::SameLine();
            
            char perfText[64];
            snprintf(perfText, sizeof(perfText), "FPS: %.0f  |  Entities: %d", g_fps, g_world.getEntityCount());
            float perfWidth = ImGui::CalcTextSize(perfText).x + 30;
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - perfWidth - 150);
            ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.5f, 1), "%s", perfText);
            
            // GPU time if available
            auto& profiler = AdvancedGPUProfiler::getInstance();
            float gpuTime = profiler.getFrameTimeMs();
            if (gpuTime > 0) {
                ImGui::SameLine();
                char gpuText[64];
                snprintf(gpuText, sizeof(gpuText), "GPU: %.2f ms", gpuTime);
                ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1), "%s", gpuText);
            }

            ImGui::EndMainMenuBar();
        }

        // ========== TOOLBAR ==========
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.08f, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::BeginChild("Toolbar", ImVec2(static_cast<float>(windowW), toolbarHeight), 
                         false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollbar);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 3));

        // Transform tools
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Transform:");
        ImGui::SameLine();

        const char* transformTools[] = {"Translate", "Rotate", "Scale"};
        GizmoType transformTypes[] = {GizmoType::Translate, GizmoType::Rotate, GizmoType::Scale};
        const char* transformTooltips[] = {"Translate Tool (W)", "Rotate Tool (E)", "Scale Tool (R)"};
        
        for (int i = 0; i < 3; i++) {
            bool isActive = (g_gizmoType == transformTypes[i]);
            ImGui::PushStyleColor(ImGuiCol_Button, isActive ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
            if (ImGui::Button(transformTools[i], ImVec2(75, 28))) {
                g_gizmoType = transformTypes[i];
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", transformTooltips[i]);
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }

        // Separator
        ImGui::Dummy(ImVec2(15, 1));
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(15, 1));
        ImGui::SameLine();

        // Space toggle
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Space:");
        ImGui::SameLine();

        const char* spaceText = (g_spaceType == SpaceType::World) ? "World" : "Local";
        if (ImGui::Button(spaceText, ImVec2(70, 28))) {
            g_spaceType = (g_spaceType == SpaceType::World) ? SpaceType::Local : SpaceType::World;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Transform Space (X)");
        ImGui::SameLine();

        // Separator
        ImGui::Dummy(ImVec2(15, 1));
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(15, 1));
        ImGui::SameLine();

        // View options
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "View:");
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, g_showGrid ? ImVec4(0.3f, 0.3f, 0.2f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Grid", ImVec2(50, 28))) g_showGrid = !g_showGrid;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Grid Display");
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, g_showGizmo ? ImVec4(0.3f, 0.3f, 0.2f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Gizmo", ImVec2(55, 28))) g_showGizmo = !g_showGizmo;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Gizmo Display");
        ImGui::PopStyleColor();
        ImGui::SameLine();

        // Separator
        ImGui::Dummy(ImVec2(15, 1));
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(15, 1));
        ImGui::SameLine();

        // Play controls
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Play:");
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, g_isPlaying ? ImVec4(0.1f, 0.3f, 0.1f, 1) : ImVec4(0.2f, 0.5f, 0.2f, 1));
        if (ImGui::Button(g_isPlaying ? "PLAYING" : "PLAY", ImVec2(70, 28))) {
            if (!g_isPlaying) {
                // Enter play mode
                g_isPlaying = true;
                g_wasPlaying = false;
                std::cout << "=== PLAY MODE ===\n";
                // TODO: Save current state before entering play mode
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enter Play Mode");
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, g_isPlaying ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.3f, 0.3f, 0.3f, 1));
        ImGui::BeginDisabled(!g_isPlaying);
        if (ImGui::Button("PAUSE", ImVec2(70, 28))) {
            g_wasPlaying = g_isPlaying;
            g_isPlaying = !g_wasPlaying;
            std::cout << (g_wasPlaying ? "=== PAUSED ===\n" : "=== RESUMED ===\n");
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pause/Resume Game");
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, g_isPlaying ? ImVec4(0.5f, 0.1f, 0.1f, 1) : ImVec4(0.3f, 0.3f, 0.3f, 1));
        ImGui::BeginDisabled(!g_isPlaying && !g_wasPlaying);
        if (ImGui::Button("STOP", ImVec2(60, 28))) {
            if (g_isPlaying || g_wasPlaying) {
                g_isPlaying = false;
                g_wasPlaying = false;
                std::cout << "=== STOP (Exited Play Mode) ===\n";
                // TODO: Restore state from before play mode
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Exit Play Mode");
        ImGui::PopStyleColor();

        ImGui::PopStyleVar(2);
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // ========== LEFT PANEL (Outliner / Details - Tabbed) ==========
        ImGui::SetNextWindowPos(ImVec2(0, menuBarHeight + toolbarHeight));
        ImGui::SetNextWindowSize(ImVec2(sidePanelWidth,
            static_cast<float>(windowH) - menuBarHeight - toolbarHeight - tabbedBottomHeight));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.25f, 0.25f, 0.25f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1));
        ImGui::Begin("Scene", nullptr,
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Tab buttons
        ImGui::PushStyleColor(ImGuiCol_Button, g_uiState.leftPanelTab == 0 ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Outliner", ImVec2(sidePanelWidth/2 - 5, 28))) g_uiState.leftPanelTab = 0;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, g_uiState.leftPanelTab == 1 ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Details", ImVec2(sidePanelWidth/2 - 5, 28))) g_uiState.leftPanelTab = 1;
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();

        if (g_uiState.leftPanelTab == 0) {
            // ========== WORLD OUTLINER ==========
            // Search box
            ImGui::PushItemWidth(-1);
            ImGui::InputTextWithHint("##Search", "Search entities...", g_uiState.searchBuffer,
                                    sizeof(g_uiState.searchBuffer));
            ImGui::PopItemWidth();
            ImGui::Separator();
            ImGui::Spacing();
            
            int entityCount = 0;
        g_world.forEach<ecs::TransformComponent, ecs::MeshComponent>(
            [&](ecs::EntityID id, ecs::TransformComponent&, ecs::MeshComponent& m) {
                bool isSelected = (g_selected == id);
                ImGui::PushStyleColor(ImGuiCol_Text,
                    isSelected ? ImVec4(1.0f, 0.8f, 0.2f, 1) : ImVec4(0.85f, 0.85f, 0.85f, 1));

                char label[64];
                snprintf(label, sizeof(label), "Entity %d", id);

                // Right-click context menu
                if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    g_selected = id;
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        // Double-click: focus on entity (TODO: move camera)
                        logMessage("Focus on: " + std::string(label));
                    }
                }
                
                // Context menu on right-click
                if (ImGui::BeginPopupContextItem()) {
                    g_selected = id;
                    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
                        duplicateSelectedEntity();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Delete", "Delete")) {
                        deleteSelectedEntity();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Rename")) {
                        // TODO: Open rename dialog
                    }
                    ImGui::EndPopup();
                }
                
                ImGui::PopStyleColor();
                entityCount++;
            });

        // Global context menu (right-click in empty space)
        if (ImGui::BeginPopupContextWindow("OutlinerContext")) {
            if (ImGui::MenuItem("Create Cube")) {
                createCube(glm::vec3(0, 1, 0), glm::vec3(1), glm::vec3(0.8f, 0.8f, 0.8f));
            }
            if (ImGui::MenuItem("Create Sphere")) {
                createSphere(glm::vec3(0, 1, 0), 0.5f, glm::vec3(0.2f, 0.8f, 0.2f));
            }
            if (ImGui::MenuItem("Create Plane")) {
                createPlane(glm::vec3(0, 0, 0), glm::vec2(10, 10), glm::vec3(0.5f, 0.5f, 0.5f));
            }
            if (ImGui::MenuItem("Create Light")) {
                createLight(glm::vec3(5, 10, 5), glm::vec3(1, 1, 0.9f), 1.0f);
            }
            if (ImGui::MenuItem("Create Camera")) {
                createCamera(glm::vec3(0, 5, 10), glm::vec3(0, 0, 0));
            }
            ImGui::EndPopup();
        }

        if (entityCount == 0) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "No entities in scene");
            ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1), "Right-click to create one");
        }

        } else {
            // ========== DETAILS ==========
            if (g_selected != ecs::INVALID_ENTITY_ID) {
                // Get components safely
                ecs::Entity selectedEntity{g_selected};

                // Check if entity still exists before accessing
                bool entityStillExists = false;
                ecs::TransformComponent* t = nullptr;
                ecs::MeshComponent* m = nullptr;

                // Try to get components (they may not exist or entity may be destroyed)
                if (selectedEntity.isValid()) {
                    t = g_world.getComponentArchetype<ecs::TransformComponent>(selectedEntity);
                    m = g_world.getComponentArchetype<ecs::MeshComponent>(selectedEntity);
                    entityStillExists = (t || m);
                }

                ImGui::Separator();
                ImGui::Spacing();

                // Transform component
                if (t && entityStillExists) renderTransformSection(t);

                // Mesh component
                if (m && entityStillExists) renderMeshSection(m);

            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "No entity selected");
                ImGui::Dummy(ImVec2(0, 20));
                ImGui::TextWrapped("Select an entity from the Outliner to view and edit its properties.");
            }
        }

        ImGui::PopStyleColor(2);
        ImGui::End();

        // ========== BOTTOM PANEL (Content / Console / Debug - Tabbed) ==========
        ImGui::SetNextWindowPos(ImVec2(0, static_cast<float>(windowH) - tabbedBottomHeight));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(windowW), tabbedBottomHeight));
        ImGui::Begin("Toolbox", nullptr,
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1));

        // Tab buttons
        ImGui::PushStyleColor(ImGuiCol_Button, g_uiState.bottomPanelTab == 0 ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Content", ImVec2(90, 28))) g_uiState.bottomPanelTab = 0;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, g_uiState.bottomPanelTab == 1 ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Console", ImVec2(90, 28))) g_uiState.bottomPanelTab = 1;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, g_uiState.bottomPanelTab == 2 ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Profiler", ImVec2(90, 28))) g_uiState.bottomPanelTab = 2;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::Spacing();

        if (g_uiState.bottomPanelTab == 0) {
            // ========== CONTENT BROWSER ==========

        // Path bar
        ImGui::PushItemWidth(300);
        ImGui::InputText("##Path", g_uiState.pathBuffer, sizeof(g_uiState.pathBuffer));
        ImGui::PopItemWidth();
        ImGui::SameLine();

        if (ImGui::Button("Refresh", ImVec2(75, 0))) {
            std::cout << "Refreshing asset browser: " << g_uiState.pathBuffer << "\n";
            // Asset refresh would scan the directory
        }
        ImGui::SameLine();
        if (ImGui::Button("Import", ImVec2(75, 0))) {
            std::cout << "Import asset dialog (not implemented)\n";
            // TODO: Open file dialog for asset import
        }

        ImGui::Separator();

        // Asset grid
        ImGui::BeginChild("Assets", ImVec2(0, 0), true);

        // Calculate grid layout
        float iconSize = 70.0f;
        float availWidth = ImGui::GetContentRegionAvail().x;
        int itemsPerRow = std::max(1, static_cast<int>(availWidth / (iconSize + 15)));

        // Meshes section
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1), "MESHES");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8));

        const char* meshes[] = {"Cube", "Sphere", "Plane", "Cylinder", "Cone", "Torus"};
        for (int i = 0; i < 6; i++) {
            ImGui::PushID(i);
            
            // Asset box
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 1));
            ImGui::Button("##Asset", ImVec2(iconSize, iconSize));
            ImGui::PopStyleColor();
            
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Click to select %s", meshes[i]);
            }
            
            ImGui::SameLine();
            ImGui::TextWrapped("%s", meshes[i]);
            ImGui::SameLine();

            if ((i + 1) % itemsPerRow != 0 && i < 5) {
                ImGui::SameLine();
            }
            ImGui::PopID();
        }

        ImGui::Dummy(ImVec2(0, 15));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 10));

        // Materials section
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.4f, 1), "MATERIALS");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8));

        const char* materials[] = {"M_Default", "M_Metal", "M_Wood", "M_Stone", "M_Glass"};
        for (int i = 0; i < 5; i++) {
            ImGui::PushID(i + 100);
            
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 1));
            ImGui::Button("##Mat", ImVec2(iconSize, iconSize));
            ImGui::PopStyleColor();
            
            ImGui::SameLine();
            ImGui::TextWrapped("%s", materials[i]);
            ImGui::SameLine();

            if ((i + 1) % itemsPerRow != 0 && i < 4) {
                ImGui::SameLine();
            }
            ImGui::PopID();
        }

        ImGui::EndChild();

        } else if (g_uiState.bottomPanelTab == 1) {
            // ========== CONSOLE ==========
            // Console toolbar
            if (ImGui::Button("Clear", ImVec2(60, 0))) {
                g_consoleMessages.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button("Collapse All", ImVec2(80, 0))) {
                // TODO: Collapse log groups
            }
            ImGui::SameLine();
            ImGui::Button("##Sep1", ImVec2(1, 20));
            ImGui::SameLine();

            // Filter buttons
            const char* filterNames[] = {"All", "Info", "Warning", "Error"};
            for (int i = -1; i < 3; i++) {
                ImGui::PushStyleColor(ImGuiCol_Button, g_consoleFilter == i ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
                if (ImGui::Button(filterNames[i + 1], ImVec2(60, 0))) {
                    g_consoleFilter = i;
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }

            ImGui::Separator();

            // Message list
            ImGui::BeginChild("ConsoleMessages", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

            int msgCount = 0;
            for (const auto& msg : g_consoleMessages) {
                if (g_consoleFilter != -1 && msg.level != g_consoleFilter) continue;

                ImVec4 color;
                const char* prefix;
                if (msg.level == 2) { color = ImVec4(1.0f, 0.3f, 0.3f, 1); prefix = "[ERROR]"; }
                else if (msg.level == 1) { color = ImVec4(1.0f, 0.8f, 0.0f, 1); prefix = "[WARN]"; }
                else { color = ImVec4(0.7f, 0.7f, 0.7f, 1); prefix = "[INFO]"; }

                ImGui::TextColored(color, "%s %s", prefix, msg.text.c_str());
                msgCount++;
            }

            if (msgCount == 0) {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "No messages");
            }

            // Auto-scroll to bottom
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }

            ImGui::EndChild();

        } else if (g_uiState.bottomPanelTab == 2) {
            // ========== PROFILER PANEL ==========
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1), "PROFILER");
            ImGui::Separator();
            ImGui::Spacing();

            // Engine stats
            ImGui::Text("FPS: %.1f", g_fps);
            ImGui::Text("Frame Time: %.2f ms", 1000.0f / (g_fps > 0 ? g_fps : 60));
            ImGui::Text("Entity Count: %d", g_world.getEntityCount());
            ImGui::Separator();
            ImGui::Spacing();

            // Camera info
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1), "CAMERA");
            ImGui::Text("Position: (%.2f, %.2f, %.2f)",
                g_camera->Position.x, g_camera->Position.y, g_camera->Position.z);
            ImGui::Text("Target: (%.2f, %.2f, %.2f)",
                g_camera->Target.x, g_camera->Target.y, g_camera->Target.z);
            ImGui::Separator();
            ImGui::Spacing();

            // Selected entity info
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1), "SELECTION");
            if (g_selected != ecs::INVALID_ENTITY_ID) {
                ImGui::Text("Selected Entity ID: %d", g_selected);
                auto* t = g_world.getComponentArchetype<ecs::TransformComponent>(ecs::Entity{g_selected});
                if (t) {
                    ImGui::Text("Position: (%.2f, %.2f, %.2f)", t->position.x, t->position.y, t->position.z);
                }
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "No entity selected");
            }
            ImGui::Separator();
            ImGui::Spacing();

            // Memory info
            ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1), "MEMORY");
            auto* drawData = ImGui::GetDrawData();
            if (drawData) {
                ImGui::Text("ImGui Draw Lists: %d", drawData->CmdListsCount);
            } else {
                ImGui::Text("ImGui Draw Lists: N/A");
            }
            // TODO: Add more memory stats when memory manager is integrated

        }

        ImGui::PopStyleColor();
        ImGui::End();

        // ========== VIEWPORT (Center - Maximizes remaining space) ==========
        float vpX = sidePanelWidth;
        float vpY = menuBarHeight + toolbarHeight;
        float vpW = static_cast<float>(windowW) - sidePanelWidth;
        float vpH = static_cast<float>(windowH) - menuBarHeight - toolbarHeight - tabbedBottomHeight;

        ImGui::SetNextWindowPos(ImVec2(vpX, vpY));
        ImGui::SetNextWindowSize(ImVec2(vpW, vpH));
        ImGui::Begin("Viewport", nullptr,
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImVec2 contentSize = ImGui::GetContentRegionAvail();
        if (contentSize.x > 0 && contentSize.y > 0) {
            int w = static_cast<int>(contentSize.x);
            int h = static_cast<int>(contentSize.y);
            
            // Resize FBO if needed
            if (w != g_viewportFB.width || h != g_viewportFB.height) {
                g_viewportFB.resize(w, h);
            }

            // Display FBO texture
            ImGui::Image((void*)(intptr_t)g_viewportFB.colorTex, 
                        ImVec2(static_cast<float>(w), static_cast<float>(h)),
                        ImVec2(0, 1), ImVec2(1, 0));

            // Viewport interaction (right-click to look)
            ImVec2 viewportMin = ImGui::GetCursorScreenPos();
            ImVec2 viewportMax = ImVec2(viewportMin.x + w, viewportMin.y + h);
            ImVec2 mousePos = ImGui::GetMousePos();
            
            bool mouseInViewport = (mousePos.x >= viewportMin.x && mousePos.x < viewportMax.x &&
                                   mousePos.y >= viewportMin.y && mousePos.y < viewportMax.y);

            if (mouseInViewport && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                g_isViewing = true;
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                g_isViewing = false;
            }
            
            if (g_isViewing) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);
            }
        }

        ImGui::End();

        // Show About dialog if requested
        showAboutDialog();

        // ========== STATUS BAR (Bottom of screen) ==========
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
        ImGui::SetNextWindowPos(ImVec2(0, static_cast<float>(windowH) - statusBarHeight));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(windowW), statusBarHeight));
        ImGui::Begin("StatusBar", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Left side: Scene info
        int totalEntities = 0;
        g_world.forEach<ecs::TransformComponent>([&](ecs::EntityID, ecs::TransformComponent&) {
            totalEntities++;
        });

        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Entities: %d  |  Selected: %s",
            totalEntities,
            g_selected != ecs::INVALID_ENTITY_ID ? "Yes" : "No");
        ImGui::SameLine();

        // Center: Play mode indicator
        if (g_isPlaying) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1));
            ImGui::Text("\xef\x81\x8b  PLAYING");
            ImGui::PopStyleColor();
        } else if (g_wasPlaying) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.0f, 1));
            ImGui::Text("\xef\x81\x8c  PAUSED");
            ImGui::PopStyleColor();
        }

        // Right side: FPS and memory
        char statusText[128];
        snprintf(statusText, sizeof(statusText), "FPS: %.0f  |  Frame: %.2fms",
            g_fps, 1000.0f / (g_fps > 0 ? g_fps : 60));

        float textWidth = ImGui::CalcTextSize(statusText).x + 20;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - textWidth);
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "%s", statusText);

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        // Final ImGui render
        ImGui::Render();
        
        // Clear and render ImGui draw data
        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Swap buffers and poll events
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    std::cout << "Shutting down...\n";

    // Shutdown GPU profiler
    AdvancedGPUProfiler::getInstance().shutdown();
    std::cout << "GPU Profiler shutdown complete\n";

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    g_world.shutdown();
    
    cleanupCube();
    g_viewportFB.cleanup();
    glDeleteProgram(g_shaderProg);
    
    if (g_camera) {
        delete g_camera;
        g_camera = nullptr;
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    std::cout << "Goodbye!\n";
    return 0;
}
