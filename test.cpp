/**
 * ============================================================================
 * RTT ENGINE EDITOR - Modular Version
 * ============================================================================
 * Main entry point that uses modular editor components:
 * - editor/editor_state.h    - Global editor state
 * - editor/ui.h              - ImGui UI rendering
 * - editor/mesh_builder.h    - Procedural mesh generation
 * - editor/shader_manager.h  - Shader management
 * - editor/grid_renderer.h   - Grid rendering
 * - editor/gizmo_renderer.h  - Transform gizmos
 * - editor/entity_manager.h  - Entity operations
 * - editor/scene_manager.h   - Scene save/load
 * - editor/console.h         - Console/logging system
 * ============================================================================
 */

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <iostream>
#include <string>
#include <sys/stat.h>

// Editor modules
#include "editor/editor_state.h"
#include "editor/ui.h"
#include "editor/mesh_builder.h"
#include "editor/shader_manager.h"
#include "editor/grid_renderer.h"
#include "editor/gizmo_renderer.h"
#include "editor/entity_manager.h"
#include "editor/scene_manager.h"
#include "editor/console.h"

#include "renderer/GPUProfilerAdvanced.h"

// ============================================================================
// Font Loading
// ============================================================================
static bool loadIconFont(ImGuiIO& io) {
    const char* fontPaths[] = {
        "/home/run-time-terror/.local/share/fonts/FiraCodeNerdFont-Regular.ttf",
        "/home/run-time-terror/.local/share/fonts/FiraCodeNerdFontMono-Regular.ttf",
        "/usr/share/fonts/truetype/JetBrainsMono/JetBrainsMonoNerdFont-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        nullptr
    };

    ImFontConfig fontConfig;
    fontConfig.MergeMode = false;
    fontConfig.PixelSnapH = true;

    ImVector<ImWchar> ranges;
    ImFontGlyphRangesBuilder builder;
    builder.AddText("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!@#$%^&*()_+-=[]{}|;':\",./<>?");
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    ImWchar iconRange[] = { 0xf000, 0xf8ff, 0 };
    builder.AddRanges(iconRange);
    builder.BuildRanges(&ranges);

    for (int i = 0; fontPaths[i] != nullptr; i++) {
        struct stat buffer;
        if (stat(fontPaths[i], &buffer) == 0) {
            std::cout << "Loading font: " << fontPaths[i] << "\n";
            io.Fonts->AddFontFromFileTTF(fontPaths[i], 16.0f, &fontConfig, ranges.Data);
            return true;
        }
    }

    std::cout << "Using default ImGui font (no custom font found)\n";
    return false;
}

// ============================================================================
// Render Scene to FBO
// ============================================================================
void renderScene() {
    PROFILE_GPU_SCOPE("Render Scene");

    g_editor.frameCount++;
    float currentTime = glfwGetTime();

    if (g_editor.frameCount % 60 == 0 && g_editor.debugConfig.verbose) {
        float dt = currentTime - g_editor.lastRenderTime;
        if (dt > 0) {
            std::cout << "[DEBUG] Frame " << g_editor.frameCount
                      << " | Entities: " << g_editor.world.getEntityCount()
                      << " | FPS: " << (1.0f/dt) << "\n";
        }
        g_editor.lastRenderTime = currentTime;
    }

    // Bind viewport framebuffer
    g_editor.viewportFB.bind();

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    if (g_editor.showWireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    } else {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Set renderer viewport and camera matrices
    g_editor.renderer.SetViewport(0, 0, g_editor.viewportFB.width, g_editor.viewportFB.height);

    glm::mat4 view = glm::lookAt(g_editor.camera->Position, g_editor.camera->Target, glm::vec3(0,1,0));
    glm::mat4 proj = glm::perspective(glm::radians(60.0f),
                                       (float)g_editor.viewportFB.width / g_editor.viewportFB.height,
                                       0.1f, 1000.0f);
    g_editor.renderer.SetCameraMatrices(view, proj);
    g_editor.renderer.SetLightParameters(glm::vec3(5.0f, 5.0f, 5.0f), g_editor.camera->Position);

    // Render all ECS entities through RenderSystem
    g_editor.renderSystem.render();

    // Render grid
    GridRenderer::Draw(view, proj, ShaderManager::GetMainShaderProgram());

    // Render gizmo for selected entity
    if (g_editor.selectedEntity != ecs::INVALID_ENTITY_ID) {
        ecs::Entity selectedEntity{g_editor.selectedEntity};
        auto* t = g_editor.world.getComponentArchetype<ecs::TransformComponent>(selectedEntity);
        if (t) {
            float gizmoSize = glm::max(1.0f, glm::distance(g_editor.camera->Position, t->position) * 0.15f);
            GizmoRenderer::Draw(t->position, gizmoSize, t->rotation, view, proj,
                               ShaderManager::GetGizmoShaderProgram(), g_editor.gizmoType);
        }
    }

    // Reset polygon mode
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // Restore default framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
}

// ============================================================================
// Input Handling
// ============================================================================
void handleInput(GLFWwindow* window, float dt, ImGuiIO& io) {
    if (!io.WantCaptureKeyboard) {
        // Gizmo tools
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS && g_editor.gizmoType != GizmoRenderer::GizmoType::Translate) {
            static float lastW = 0;
            if (glfwGetTime() - lastW > 0.2f) {
                g_editor.gizmoType = GizmoRenderer::GizmoType::Translate;
                lastW = glfwGetTime();
            }
        }
        if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS && g_editor.gizmoType != GizmoRenderer::GizmoType::Rotate) {
            static float lastE = 0;
            if (glfwGetTime() - lastE > 0.2f) {
                g_editor.gizmoType = GizmoRenderer::GizmoType::Rotate;
                lastE = glfwGetTime();
            }
        }
        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS && g_editor.gizmoType != GizmoRenderer::GizmoType::Scale) {
            static float lastR = 0;
            if (glfwGetTime() - lastR > 0.2f) {
                g_editor.gizmoType = GizmoRenderer::GizmoType::Scale;
                lastR = glfwGetTime();
            }
        }
        if (glfwGetKey(window, GLFW_KEY_X) == GLFW_PRESS) {
            static float lastX = 0;
            if (glfwGetTime() - lastX > 0.2f) {
                g_editor.spaceType = (g_editor.spaceType == GizmoRenderer::SpaceType::World) ?
                                     GizmoRenderer::SpaceType::Local : GizmoRenderer::SpaceType::World;
                lastX = glfwGetTime();
            }
        }

        // Entity operations
        bool ctrlPressed = (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                           glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS);

        if (ctrlPressed && glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
            EntityManager::DuplicateEntity(g_editor.selectedEntity);
            static float lastDupTime = 0;
            if (glfwGetTime() - lastDupTime > 0.2f) {
                lastDupTime = glfwGetTime();
            }
        }

        if (glfwGetKey(window, GLFW_KEY_DELETE) == GLFW_PRESS) {
            static float lastDelTime = 0;
            if (glfwGetTime() - lastDelTime > 0.2f) {
                EntityManager::DeleteEntity(g_editor.selectedEntity);
                lastDelTime = glfwGetTime();
            }
        }

        if (glfwGetKey(window, GLFW_KEY_BACKSLASH) == GLFW_PRESS) {
            static float lastToggle = 0;
            if (glfwGetTime() - lastToggle > 0.2f) {
                // Toggle console (handled in UI)
                lastToggle = glfwGetTime();
            }
        }
    }
}

void handleCameraInput(GLFWwindow* window, float dt, ImGuiIO& io,
                       double mx, double my, bool mouseInViewport) {
    // Track right-click hold for orbit camera
    bool rightClickInViewport = mouseInViewport && ImGui::IsMouseDown(ImGuiMouseButton_Right);
    bool rightClickReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Right);

    if (rightClickInViewport && !io.WantCaptureMouse) {
        g_editor.isViewing = true;
    }
    if (rightClickReleased) {
        g_editor.isViewing = false;
    }

    // Orbit camera with right-click drag
    if (g_editor.isViewing && !io.WantCaptureMouse) {
        float dx = static_cast<float>(mx - g_editor.lastMousePos.x);
        float dy = static_cast<float>(my - g_editor.lastMousePos.y);
        g_editor.camera->ProcessMouseMovement(dx * 0.3f, dy * 0.3f);
    }

    // WASD camera movement
    if (g_editor.isViewing && !io.WantCaptureKeyboard && !io.WantCaptureMouse) {
        float moveSpeed = 5.0f * dt;
        glm::vec3 forward = glm::normalize(g_editor.camera->Target - g_editor.camera->Position);
        glm::vec3 right = glm::normalize(glm::cross(forward, g_editor.camera->WorldUp));

        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
            g_editor.camera->Position += forward * moveSpeed;
            g_editor.camera->Target += forward * moveSpeed;
        }
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
            g_editor.camera->Position -= forward * moveSpeed;
            g_editor.camera->Target -= forward * moveSpeed;
        }
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
            g_editor.camera->Position -= right * moveSpeed;
            g_editor.camera->Target -= right * moveSpeed;
        }
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
            g_editor.camera->Position += right * moveSpeed;
            g_editor.camera->Target += right * moveSpeed;
        }
        if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) {
            g_editor.camera->Position.y -= moveSpeed;
            g_editor.camera->Target.y -= moveSpeed;
        }
        if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) {
            g_editor.camera->Position.y += moveSpeed;
            g_editor.camera->Target.y += moveSpeed;
        }
    }

    // Scroll zoom
    if (mouseInViewport && !io.WantCaptureMouse) {
        float scrollY = io.MouseWheel;
        if (scrollY != 0.0f) {
            g_editor.camera->ProcessMouseScroll(scrollY * 2.0f);
        }
    }

    // Update last mouse position
    g_editor.lastMousePos = glm::vec2(static_cast<float>(mx), static_cast<float>(my));
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cout << "=== RTT Engine Editor (Modular Version) ===\n";

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
    glfwSwapInterval(0);  // Disable vsync

    // Initialize GLAD
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "ERROR: Failed to initialize GLAD\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    std::cout << "OpenGL: " << glGetString(GL_VERSION) << "\n";

    // Initialize GPU Profiler
    if (!AdvancedGPUProfiler::getInstance().initialize()) {
        std::cerr << "WARNING: GPU Profiler initialization failed\n";
    }

    // Initialize editor state (creates all resources)
    InitEditor();

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "engine_ui.ini";
    io.ConfigWindowsMoveFromTitleBarOnly = true;
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

    // Dark theme colors
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
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

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 450");

    // Create initial test scene
    EntityManager::CreateCube(glm::vec3(0, 1, 0), glm::vec3(1), glm::vec3(0.8f, 0.2f, 0.2f));
    EntityManager::CreateCube(glm::vec3(2, 2, 0), glm::vec3(0.5f), glm::vec3(0.2f, 0.8f, 0.2f));
    EntityManager::CreateCube(glm::vec3(-2, 3, 0), glm::vec3(0.7f), glm::vec3(0.2f, 0.2f, 0.8f));

    std::cout << "Total entities: " << g_editor.world.getEntityCount() << "\n";
    std::cout << "Ready!\n";
    std::cout << "Controls: Right-click+drag to look, WASD to move\n";

    // Main loop
    float lastTime = 0;
    float fpsTimer = 0;
    int frames = 0;

    while (!glfwWindowShouldClose(window) && !g_editor.shouldClose) {
        BEGIN_GPU_FRAME();

        // Calculate delta time
        float now = glfwGetTime();
        float dt = now - lastTime;
        lastTime = now;

        // FPS counter
        frames++;
        fpsTimer += dt;
        if (fpsTimer >= 1.0f) {
            g_editor.fps = static_cast<float>(frames);
            frames = 0;
            fpsTimer = 0;
        }

        // Update ECS
        g_editor.world.update(dt);

        // Get window dimensions and mouse position
        int windowW, windowH;
        glfwGetWindowSize(window, &windowW, &windowH);
        double mx, my;
        glfwGetCursorPos(window, &mx, &my);

        // Handle keyboard shortcuts
        handleInput(window, dt, io);

        // Render 3D scene to FBO
        {
            PROFILE_GPU_SCOPE("Render Scene");
            renderScene();
        }

        END_GPU_FRAME();

        // Setup ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // ========== RENDER UI PANELS ==========
        
        // Menu bar
        UI::RenderMenuBar(g_editor.showAbout, g_editor.world, g_editor.selectedEntity,
                         g_editor.isPlaying, g_editor.wasPlaying,
                         SceneManager::GetCurrentSceneFile(), g_editor.shouldClose);

        // Left panel (Outliner/Details/Geo)
        UI::RenderLeftPanel(g_editor.uiState.leftPanelTab, g_editor.world,
                           g_editor.selectedEntity, g_editor.uiState.searchBuffer);

        // Bottom panel (Content/Console/Profiler)
        UI::RenderBottomPanel(g_editor.uiState.bottomPanelTab, g_editor.world,
                             g_editor.selectedEntity, g_editor.fps, g_editor.camera);

        // Viewport
        bool mouseInViewport = (mx >= g_editor.uiState.leftPanelTab * 280.0f && 
                                mx < windowW && my >= 61.0f && 
                                my < windowH - 180.0f);
        UI::RenderViewport(g_editor.selectedEntity, g_editor.camera, g_editor.isViewing,
                          g_editor.lastMousePos, g_editor.viewportFB.colorTex,
                          windowW, windowH, g_editor.gizmoType, g_editor.spaceType,
                          g_editor.showWireframe, g_editor.showGrid, g_editor.showGizmo,
                          window, io);

        // Handle camera input after viewport is rendered
        handleCameraInput(window, dt, io, mx, my, mouseInViewport);

        // Status bar
        UI::RenderStatusBar(g_editor.world, g_editor.selectedEntity, g_editor.fps,
                           g_editor.isPlaying, g_editor.wasPlaying, windowW);

        // About dialog
        UI::RenderAboutDialog(g_editor.showAbout);

        // Final ImGui render
        ImGui::Render();

        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    CleanupEditor();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    std::cout << "Goodbye!\n";
    return 0;
}
