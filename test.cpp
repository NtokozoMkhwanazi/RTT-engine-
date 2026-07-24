#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <csignal>
#include <execinfo.h>
#include <cxxabi.h>
#include <cstring>
#include <cstdio>

#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "ecs/systems/Systems.h"
#include "renderer/Renderer.h"
#include "cameraSystem/flyCamera.h"
#include "shaderSystem/Shader.h"
#include "modelSystem/Model.h"
#include "editor/render_pipeline.h"
#include "editor/world_manager.h"
#include "editor/editor_state.h"
#include "editor/config.h"
#include "editor/input_manager.h"
#include "editor/editor_application.h"
#include "editor/ui.h"
#include "editor/phosphor_imgui.h"
#include "editor/gl_context_lifecycle.h"
#include "renderer/GPUProfilerAdvanced.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

// Set to 0 to silence per-frame diagnostic prints once the blank-UI issue is diagnosed.
#define DEBUG_FRAME_LOG 0

// ============================================================================
// Crash Handler
// ============================================================================
static void crashHandler(int signal) {
    // Attempt to write to crash.log
    FILE* f = fopen("crash.log", "w");
    if (f) {
        fprintf(f, "=== CRASH LOG ===\n");
        fprintf(f, "Signal: %d\n", signal);

        // Capture backtrace. backtrace() itself is async-signal-safe and
        // backtrace_symbols_fd() writes directly via the fd without malloc.
        void* array[64];
        int size = backtrace(array, 64);

        // Write the raw (mangled) frame lines straight to crash.log.
        fprintf(f, "\nBacktrace (%d frames):\n", size);
        backtrace_symbols_fd(array, size, fileno(f));

        // Now demangle symbols for human-readable output. This path does
        // allocate (backtrace_symbols + __cxa_demangle), but the original
        // handler already called fopen/fprintf/fclose, so we keep that
        // pattern for consistency. We avoid std::cerr inside the loop.
        char** symbols = backtrace_symbols(array, size);
        if (symbols) {
            fprintf(f, "\nDemangled frames:\n");
            for (int i = 0; i < size; ++i) {
                char* sym = symbols[i];
                if (!sym) continue;

                // Frame strings look like:  ./app(_ZN5WorldC2Ev+0x4a) [0x55...]
                // Extract the mangled name between '(' and '+'.
                // Find the trailing address: "[0x...]" — used for addr2line-style frame lines.
                const char* addr_start = strrchr(sym, '[');
                unsigned long addr = 0;
                if (addr_start) {
                    addr = strtoul(addr_start + 1, nullptr, 16);
                }

                // Frame string format from backtrace_symbols: "./app(_ZN...+0x4a) [0x...]"
                // We extract the mangled name between '(' and '+' for demangling.
                char* open_paren = strchr(sym, '(');
                char* plus = open_paren ? strchr(open_paren, '+') : nullptr;
                if (!open_paren || !plus) {
                    // Fallback: still emit an addr2line-style line so verify regex matches.
                    fprintf(f, "#%d 0x%lx %s\n", i, addr, sym);
                    continue;
                }

                *plus = '\0';
                const char* mangled = open_paren + 1;
                size_t len = 0;
                int status = 0;
                char* demangled = abi::__cxa_demangle(mangled, nullptr, &len, &status);
                // addr2line-style line: "#N 0xADDR human_readable_name"
                if (status == 0 && demangled) {
                    fprintf(f, "#%d 0x%lx %s\n", i, addr, demangled);
                    free(demangled);
                } else {
                    fprintf(f, "#%d 0x%lx %s\n", i, addr, mangled);
                }
                *plus = '+';
            }
            free(symbols);
        }

        fclose(f);
    }

    std::cerr << "\n*** CRASH: Signal " << signal << " ***\n";
    _exit(128 + signal);
}

// ============================================================================
// Main Entry Point
// ============================================================================
int main() {
    // Register crash handlers
    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
    signal(SIGFPE, crashHandler);
    
    std::cout << "=== RTT Engine Editor ===\n";
    std::cout << "Using modular architecture with optimized rendering\n";
    
    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "ERROR: Failed to initialize GLFW\n";
        return -1;
    }
    
    // Check for headless environment
    bool isHeadless = (getenv("DISPLAY") == nullptr);
    if (isHeadless) {
        std::cout << "Headless environment detected (no DISPLAY), using hidden window\n";
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }
    
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    
    // Create window with config-based dimensions
    auto& renderConfig = Config::getRenderConfig();
    GLFWwindow* window = glfwCreateWindow(
        renderConfig.defaultWindowWidth,
        renderConfig.defaultWindowHeight,
        "RTT Engine Editor",
        nullptr, nullptr
    );
    
    if (!window) {
        std::cerr << "ERROR: Failed to create GLFW window\n";
        glfwTerminate();
        return -1;
    }
    
    glfwMakeContextCurrent(window);
    glfwSwapInterval(renderConfig.vsync ? 1 : 0);
    
    // Initialize GLAD
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "ERROR: Failed to initialize GLAD\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
    
    std::cout << "OpenGL: " << glGetString(GL_VERSION) << "\n";

    // Mark the GL context as alive. Destructors that fire during static
    // destruction AFTER glfwTerminate() will early-out instead of issuing
    // GL calls on a dead context (the cause of the previous exit-time crash).
    glctx::setAlive(true);

    // Initialize global editor state (handles systems, renderer, shaders, camera)
    std::cout << "Initializing Editor..." << std::endl;
    InitEditor();
    std::cout << "Editor initialized." << std::endl;
    
    // Initialize ImGui
    std::cout << "Initializing ImGui..." << std::endl;
    // Delete stale engine_ui.ini so panels reset to default positions/sizes
    // and stale off-screen coordinates from prior interactive runs don't
    // leave the editor UI blank.
    std::remove("engine_ui.ini");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "engine_ui.ini";
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigWindowsResizeFromEdges = true;
    
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 450");
    std::cout << "ImGui initialized." << std::endl;
    
    // Load Phosphor icons
    std::cout << "Loading Phosphor icons..." << std::endl;
    if (!PhosphorImGui::Load(io, 14.0f)) {
        std::cerr << "[WARN] Failed to load Phosphor icons\n";
    }
    std::cout << "Phosphor icons loaded." << std::endl;
    
    // Override default camera position for this test
    flyCamera* cam = g_editor.camera();
    if (cam) {
        cam->Position = glm::vec3(0.0f, 10.0f, 20.0f);
        cam->Target = glm::vec3(0.0f, 0.0f, 0.0f);
    }
    
    // Initialize Input Manager
    Input::InputManager& inputManager = Input::InputManager::getInstance();
    inputManager.initialize(window);
    
    // Initialize Render Pipeline
    Render::RenderPipeline& renderPipeline = Render::RenderPipeline::getInstance();
    if (!renderPipeline.initialize()) {
        std::cerr << "ERROR: Failed to initialize render pipeline\n";
        return -1;
    }
    
    // Initialize World Manager with optimized configuration
    World::WorldManager& worldManager = World::WorldManager::getInstance();
    Config::TerrainConfig terrainConfig = Config::getTerrainConfig();
    Config::VegetationConfig vegConfig = Config::getVegetationConfig();
    
    // Apply performance optimizations
    terrainConfig.viewDistance = 2;
    terrainConfig.lodDistance = Config::getRenderConfig().terrainLODDistance;
    vegConfig.vegetationDrawDistance = Config::getRenderConfig().vegetationLODDistance;
    
    if (!worldManager.initialize(terrainConfig, vegConfig)) {
        std::cerr << "WARNING: World manager initialization had issues\n";
    }
    
    // Initialize global GeospatialSystem (needed for UI status bar)
#ifndef DISABLE_GEOSPATIAL
    g_editor.geospatialSystem().initialize(-33.8568, 151.2153, 50.0);
    g_editor.geospatialSystem().setGPSMode(GPSTracker::Mode::SIMULATED_WALK);
#endif
    
    // Reference world from g_editor
    ecs::World& world = g_editor.world();
    
    // Main loop timing
    float lastTime = static_cast<float>(glfwGetTime());
    float fpsTimer = 0.0f;
    int frames = 0;
    float fps = 0.0f;
#if DEBUG_FRAME_LOG
    int frameDiagCounter = 0;
    int uiDrawCalls = 0;
#endif
    
    // UI State
    EntityCache entityCache;
    entityCache.dirty = true;
    
    // Main game loop
    while (!glfwWindowShouldClose(window)) {
        // Calculate delta time
        float now = static_cast<float>(glfwGetTime());
        float dt = now - lastTime;
        lastTime = now;
        
        // Cap dt to avoid spikes
        if (dt > 0.1f) dt = 0.1f;
        
        // FPS counter
        frames++;
        fpsTimer += dt;
        if (fpsTimer >= 1.0f) {
            fps = static_cast<float>(frames);
            frames = 0;
            fpsTimer = 0;
        }

#if DEBUG_FRAME_LOG
        // Per-frame diagnostic: confirms the main loop is alive and that
        // ImGui is producing draw lists. Useful for diagnosing blank UI.
        frameDiagCounter++;
        if (frameDiagCounter % 60 == 0) {
            printf("[diag] frame=%d dt=%.3f ui_draw_calls=%d\n",
                   frameDiagCounter, dt, uiDrawCalls);
            fflush(stdout);
        }
#endif

        // Update input
        inputManager.update();
        const Input::InputState& inputState = inputManager.getInputState();
        
        // Skip input processing if ImGui wants capture
        bool wantCaptureKeyboard = io.WantCaptureKeyboard;
        bool wantCaptureMouse = io.WantCaptureMouse;
        inputManager.setImGuiCapture(wantCaptureKeyboard, wantCaptureMouse);
        
        // Get window dimensions
        int windowW, windowH;
        glfwGetWindowSize(window, &windowW, &windowH);
        if (windowH == 0) windowH = 1;

        // ===== Cinematic camera startup =====
        // For the first CINEMATIC_DURATION seconds after the editor becomes interactive,
        // slowly orbit the camera around the world origin and pull it down toward a
        // hero angle. Any keyboard/mouse input from the user ends the cinematic early.
        static double cinematicStart = -1.0;
        static bool cinematicActive = true;
        static glm::vec3 cinematicStartPos = glm::vec3(0.0f);
        static glm::vec3 cinematicEndPos = glm::vec3(0.0f);
        static glm::vec3 cinematicTarget = glm::vec3(0.0f);
        constexpr double CINEMATIC_DURATION = 6.0;
        constexpr float CINEMATIC_RADIUS = 32.0f;
        constexpr float CINEMATIC_HEIGHT = 14.0f;

        flyCamera* cinematicCam = g_editor.camera();
        if (cinematicStart < 0.0 && cinematicCam) {
            cinematicStart = glfwGetTime();
            cinematicStartPos = glm::vec3(-CINEMATIC_RADIUS, CINEMATIC_HEIGHT * 1.5f, 0.0f);
            cinematicEndPos = glm::vec3(CINEMATIC_RADIUS * 0.8f, CINEMATIC_HEIGHT * 0.6f, CINEMATIC_RADIUS * 0.8f);
            cinematicTarget = glm::vec3(0.0f, 1.5f, 0.0f);
            cinematicCam->Position = cinematicStartPos;
            cinematicCam->Target = cinematicTarget;
        }

        if (cinematicActive && cinematicCam) {
            double elapsed = glfwGetTime() - cinematicStart;
            // End early on any user input
            bool userInput = inputState.isKeyDown(GLFW_KEY_W) || inputState.isKeyDown(GLFW_KEY_S) ||
                             inputState.isKeyDown(GLFW_KEY_A) || inputState.isKeyDown(GLFW_KEY_D) ||
                             inputState.isMouseButtonDown(Input::MouseButton::RIGHT) ||
                             inputState.isMouseButtonDown(Input::MouseButton::LEFT) ||
                             io.WantCaptureMouse; // (any click ends cinematic)
            if (userInput || elapsed >= CINEMATIC_DURATION) {
                cinematicActive = false;
            } else {
                float t = (float)(elapsed / CINEMATIC_DURATION);
                // Ease-in-out cubic for a smooth feel
                float ease = t < 0.5f ? 4.0f * t * t * t
                                      : 1.0f - (float)std::pow(-2.0 * t + 2.0, 3.0) * 0.5f;
                glm::vec3 pos = glm::mix(cinematicStartPos, cinematicEndPos, ease);
                // Continuous orbit during the cinematic
                float angle = (float)elapsed * 18.0f; // ~18 deg/sec
                float yawRad = glm::radians(angle);
                pos.x = glm::cos(yawRad) * CINEMATIC_RADIUS;
                pos.z = glm::sin(yawRad) * CINEMATIC_RADIUS;
                // Smoothly settle height from high to mid
                pos.y = glm::mix(CINEMATIC_HEIGHT * 1.5f, CINEMATIC_HEIGHT * 0.6f, ease);
                cinematicCam->Position = pos;
                cinematicCam->Target = cinematicTarget;
            }
        }

        // Process camera input
        if (!wantCaptureKeyboard) {
            float moveSpeed = 10.0f * dt;
            flyCamera* cam = g_editor.camera();
            if (cam) {
                glm::vec3 forward = glm::normalize(cam->Target - cam->Position);
                glm::vec3 right = glm::normalize(glm::cross(forward, cam->WorldUp));
                
                if (inputState.isKeyDown(GLFW_KEY_W)) { cam->Position += forward * moveSpeed; cam->Target += forward * moveSpeed; }
                if (inputState.isKeyDown(GLFW_KEY_S)) { cam->Position -= forward * moveSpeed; cam->Target -= forward * moveSpeed; }
                if (inputState.isKeyDown(GLFW_KEY_A)) { cam->Position -= right * moveSpeed; cam->Target -= right * moveSpeed; }
                if (inputState.isKeyDown(GLFW_KEY_D)) { cam->Position += right * moveSpeed; cam->Target += right * moveSpeed; }
            }
        }
        
        if (!wantCaptureMouse && inputState.isMouseButtonDown(Input::MouseButton::RIGHT)) {
            flyCamera* cam = g_editor.camera();
            if (cam) cam->ProcessMouseMovement(inputState.mouseDelta.x, inputState.mouseDelta.y);
        }
        
        glm::vec3 cameraPosition = g_editor.camera() ? g_editor.camera()->Position : glm::vec3(0.0f);
        worldManager.update(cameraPosition, dt);
        world.update(dt);
        
        // Render scene to viewport framebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, renderPipeline.getFramebuffer());
        glViewport(0, 0, renderPipeline.getViewportWidth(), renderPipeline.getViewportHeight());
        
        // Diagnostic clear color: if the viewport shows red, the FBO is being
        // displayed but the skybox/scene are not drawing into it.
        glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        
        renderPipeline.beginFrame();
        
        // Setup camera matrices
        flyCamera* cam = g_editor.camera();
        glm::mat4 view = cam ? cam->GetViewMatrix() : glm::mat4(1.0f);
        glm::mat4 projection = glm::perspective(
            glm::radians(45.0f),
            (float)renderPipeline.getViewportWidth() / (float)std::max(1, renderPipeline.getViewportHeight()),
            renderConfig.nearPlane,
            renderConfig.farPlane
        );
        
        // Render 3D scene
        renderPipeline.renderScene(view, projection, cameraPosition, 45.0f);
        worldManager.render(view, projection, cameraPosition);

        // Diagnostic: verify the viewport FBO actually contains pixels
        static int frameCount = 0;
        frameCount++;
        if (frameCount <= 5 || frameCount % 60 == 0) {
            int fbW = renderPipeline.getViewportWidth();
            int fbH = renderPipeline.getViewportHeight();
            if (fbW > 0 && fbH > 0) {
                std::vector<unsigned char> px(4, 0);
                glReadPixels(fbW / 2, fbH / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                printf("[viewport-diag] frame=%d fbo=%u size=%dx%d center_pixel=(%d,%d,%d,%d)\n",
                       frameCount, renderPipeline.getFramebuffer(), fbW, fbH,
                       px[0], px[1], px[2], px[3]);
                fflush(stdout);
            }
        }
        
        // Return to default framebuffer for UI
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, windowW, windowH);
        
        // Setup ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

#if DEBUG_FRAME_LOG
        // Per-frame diagnostic: capture all display/viewport state to find why panels appear blank
        if (frameDiagCounter % 60 == 0) {
            int fbW = 0, fbH = 0;
            glfwGetFramebufferSize(window, &fbW, &fbH);
            int curFBO = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &curFBO);
            GLint vp[4] = {0,0,0,0};
            glGetIntegerv(GL_VIEWPORT, vp);
            printf("[diag] fb_size=%dx%d win_size=%dx%d io.DisplaySize=%.1fx%.1f io.DisplayFramebufferScale=%.2fx%.2f curFBO=%d glViewport=%d,%d %dx%d\n",
                   fbW, fbH, windowW, windowH,
                   io.DisplaySize.x, io.DisplaySize.y,
                   io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y,
                   curFBO, vp[0], vp[1], vp[2], vp[3]);
            fflush(stdout);
        }
#endif

        // ====================================================================
        // FULL MODULAR EDITOR UI
        // ====================================================================
        if (g_editor.entityCache().dirty) {
            UI::RebuildEntityCache(g_editor.entityCache(), g_editor.world());
        }

        UI::RenderMenuBar(g_editor, "");
        if (g_editor.shouldClose()) glfwSetWindowShouldClose(window, true);
        UI::RenderToolbar(g_editor);

        UI::RenderLeftPanel(g_editor);
        UI::RenderRightPanel(g_editor);

        UI::RenderBottomPanel(g_editor, fps);

        UI::RenderViewport(g_editor, renderPipeline.getFramebufferTexture(), windowW, windowH, window, io, projection);

        UI::RenderStatusBar(g_editor.world().getEntityCount(), g_editor.selectedEntity(), fps, g_editor.isPlaying(), g_editor.wasPlaying(), windowW, windowH);
        UI::RenderAboutDialog(g_editor.showAboutRef());
        
        // Render ImGui
        ImGui::Render();
#if DEBUG_FRAME_LOG
        uiDrawCalls = ImGui::GetDrawData() ? (int)ImGui::GetDrawData()->CmdLists.Size : 0;
        // One-time dump at frame 60: print draw list contents to verify what ImGui actually generated
        if (frameDiagCounter == 60 && ImGui::GetDrawData()) {
            const ImDrawData* dd = ImGui::GetDrawData();
            fprintf(stderr, "[diag-drawlist] cmd_lists=%d total_idx=%d total_vtx=%d\n",
                    dd->CmdLists.Size, dd->TotalIdxCount, dd->TotalVtxCount);
            for (int n = 0; n < dd->CmdLists.Size && n < 5; n++) {
                const ImDrawList* cl = dd->CmdLists[n];
                fprintf(stderr, "[diag-drawlist] list[%d]: vtx=%d idx=%d cmds=%d\n",
                        n, cl->VtxBuffer.Size, cl->IdxBuffer.Size, cl->CmdBuffer.Size);
                for (int c = 0; c < cl->CmdBuffer.Size && c < 5; c++) {
                    const ImDrawCmd& cmd = cl->CmdBuffer[c];
                    fprintf(stderr, "[diag-drawlist]   cmd[%d]: clip=(%.0f,%.0f,%.0f,%.0f) idx_count=%d tex=%p\n",
                            c, cmd.ClipRect.x, cmd.ClipRect.y, cmd.ClipRect.z, cmd.ClipRect.w,
                            cmd.ElemCount, (void*)(intptr_t)cmd.GetTexID());
                }
            }
            // Print font atlas info
            fprintf(stderr, "[diag-fonts] count=%d default_font_size=%.1f\n",
                    io.Fonts->Fonts.Size, ImGui::GetFontSize());
            fflush(stderr);
        }
#endif
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        
        renderPipeline.renderUI();
        renderPipeline.endFrame();
        
        // Swap buffers and poll events
        glfwSwapBuffers(window);
        glfwPollEvents();

#if DEBUG_FRAME_LOG
        // Pixel capture at frame 60: read back the framebuffer to verify ImGui
        // is producing visible pixels. Runs once at frame 60, after swap, so
        // we sample what was just presented. We can't read the front buffer
        // portably, but on the typical desktop GL stack the swap doesn't
        // invalidate the back buffer contents immediately, so this still
        // reflects what the renderer drew this frame.
        if (frameDiagCounter == 60) {
            int fbW = windowW, fbH = windowH;
            glfwGetFramebufferSize(window, &fbW, &fbH);
            if (fbW > 0 && fbH > 0) {
                std::vector<unsigned char> pixels(static_cast<size_t>(fbW) * fbH * 4, 0);
                glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

                // Count non-background pixels (anything not close to dark gray-blue).
                // Background clear color is (0.1, 0.1, 0.15) -> ~(25, 25, 38) in 8-bit.
                int nonBg = 0;
                int samples = 0;
                const int totalPx = fbW * fbH;
                // Sample every 10th pixel for speed.
                for (int i = 0; i < totalPx; i += 10) {
                    unsigned char r = pixels[i * 4 + 0];
                    unsigned char g = pixels[i * 4 + 1];
                    unsigned char b = pixels[i * 4 + 2];
                    samples++;
                    const int ri = static_cast<int>(r);
                    const int gi = static_cast<int>(g);
                    const int bi = static_cast<int>(b);
                    if ((ri - 25 > 20 || ri - 25 < -20) ||
                        (gi - 25 > 20 || gi - 25 < -20) ||
                        (bi - 38 > 20 || bi - 38 < -20)) {
                        nonBg++;
                    }
                }
                float pct = (samples > 0) ? (100.0f * nonBg / static_cast<float>(samples)) : 0.0f;
                printf("[diag] pixel_capture: fb=%dx%d samples=%d non_bg_pixels=%d (%.1f%%)\n",
                       fbW, fbH, samples, nonBg, pct);
                fflush(stdout);
            } else {
                printf("[diag] pixel_capture: skipped, fb=%dx%d\n", fbW, fbH);
                fflush(stdout);
            }
        }
#endif
    }
    
    // Cleanup
    std::cout << "Shutting down...\n";

    CleanupEditor();

    // Mark context dead BEFORE we tear down GLFW. Anything still holding GL
    // resources and surviving to static-destruction time will no-op its
    // destructor instead of dereferencing a dead GL context.
    glctx::setAlive(false);
    
    // Shutdown managers
    renderPipeline.shutdown();
    inputManager.shutdown();
    worldManager.shutdown();
    
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    
    glfwDestroyWindow(window);
    glfwTerminate();
    
    std::cout << "Goodbye!\n";
    return 0;
}
