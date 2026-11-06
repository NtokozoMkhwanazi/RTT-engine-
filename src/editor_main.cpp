// ============================================================================
// src/editor_main.cpp
//
// Editor application entry point (bin/editor_app).
//
// The original body of this file was lost to a bad automated overwrite. This
// version was reconstructed from the surviving 2026-11-01 release binary
// (bin/release/editor_app, not stripped), whose main() still records the exact
// call sequence the editor booted with:
//
//   signal(SIGSEGV/SIGABRT/SIGFPE) -> crash.log
//   RHI::graphicsApiConfigPath() -> graphics_api.cfg -> RHI::parseGraphicsApi
//   RHI::createRHI() -> initialize() (window + GL context + VSync)
//   Editor::ImGuiContext::initialize() -> fonts/theme/UIConfig
//   Editor::EditorApplication::getInstance().initialize()
//   InitEditor() -> setUiFrameHook(shared editor UI) -> run()
//   shutdown -> CleanupEditor -> Terrain::ClearTextureCache -> ImGui teardown
//
// The Vulkan swapchain path (BuildVulkanScene + a bespoke render loop) is NOT
// part of this reconstruction: the editor's RenderPipeline is GL-only, so a
// Vulkan request falls back to OpenGL with an explicit warning instead of
// silently drawing nothing.
// ============================================================================

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <csignal>
#include <execinfo.h>
#include <cxxabi.h>

#include <glm/glm.hpp>

#include "rhi/RHI.h"
#include "world/Terrain.h"          // Terrain::ClearTextureCache (teardown order)
#include "editor/config.h"          // Config::getRenderConfig()
#include "editor/editor_state.h"    // g_editor, InitEditor/CleanupEditor
#include "editor/editor_application.h"
#include "editor/imgui_context.h"
#include "editor/editor_theme.h"
#include "editor/phosphor_imgui.h"
#include "editor/fa_imgui.h"
#include "editor/ui_config.h"
#include "editor/ui.h"
#include "editor/PlayModeController.h"
#include "editor/render_pipeline.h"
#include "editor/scene_manager.h"
#include "editor/profiler.h"        // PROFILE_CPU_SCOPE

#include <imgui.h>
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// ============================================================================
// Crash handler (demangled backtrace -> crash.log), same contract as the
// engine entry point (test.cpp) so both apps dump readable crashes.
// ============================================================================
static void crashHandler(int signal) {
    FILE* f = fopen("crash.log", "w");
    if (f) {
        fprintf(f, "=== CRASH LOG ===\nSignal: %d\n", signal);
        void* array[64];
        int size = backtrace(array, 64);
        fprintf(f, "\nBacktrace (%d frames):\n", size);
        backtrace_symbols_fd(array, size, fileno(f));

        char** symbols = backtrace_symbols(array, size);
        if (symbols) {
            fprintf(f, "\nDemangled frames:\n");
            for (int i = 0; i < size; ++i) {
                char* sym = symbols[i];
                if (!sym) continue;
                const char* addr_start = strrchr(sym, '[');
                unsigned long addr = addr_start ? strtoul(addr_start + 1, nullptr, 16) : 0;
                char* open_paren = strchr(sym, '(');
                char* plus = open_paren ? strchr(open_paren, '+') : nullptr;
                if (!open_paren || !plus) {
                    fprintf(f, "#%d 0x%lx %s\n", i, addr, sym);
                    continue;
                }
                *plus = '\0';
                size_t len = 0;
                int status = 0;
                char* demangled = abi::__cxa_demangle(open_paren + 1, nullptr, &len, &status);
                fprintf(f, "#%d 0x%lx %s\n", i, addr,
                        (status == 0 && demangled) ? demangled : open_paren + 1);
                free(demangled);
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
// Shared editor UI frame (menu bar + toolbar + panels + viewport + status bar)
//
// Owns no Begin/End of its own: every panel renderer below balances its own
// ImGui window, which is what keeps the "Missing end()" IM_ASSERT from firing.
// Passed as the EditorApplication UI frame hook, so it runs after the scene has
// been rendered into the viewport FBO and before the buffer swap.
// ============================================================================
static void RenderSharedEditorUI(Editor::Editor& editor,
                                 Editor::PlayModeController* play,
                                 const char* camMode,
                                 unsigned int viewportTexture,
                                 int windowW, int windowH,
                                 GLFWwindow* window, ImGuiIO& io,
                                 const glm::mat4& projection,
                                 const glm::mat4& view,
                                 const glm::vec3* cameraPos,
                                 const glm::vec3* cameraTarget,
                                 bool swapchainBacked) {
    // Keep the docked layout legal before any panel opens (shrinks the right /
    // bottom panels instead of letting the viewport overflow its region).
    UI::EnforceGlobalLayoutConstraints(editor);

    if (editor.entityCache().dirty) {
        UI::RebuildEntityCache(editor.entityCache(), editor.world());
        editor.entityCache().dirty = false;
    }

    UI::RenderMenuBar(editor, SceneManager::GetCurrentSceneFile());
    UI::RenderSceneDialogs(editor);
    UI::RenderToolbar(editor);
    UI::RenderLeftPanel(editor);
    UI::RenderRightPanel(editor, play, camMode);
    UI::RenderBottomPanel(editor, editor.fps());
    UI::RenderViewport(editor, viewportTexture, windowW, windowH, window, &io,
                       projection, view, camMode, cameraPos, cameraTarget,
                       swapchainBacked);
    UI::RenderStatusBar(editor, editor.entityCache().entries.size(),
                        editor.selectedEntity(), editor.fps(),
                        editor.isPlaying(), editor.wasPlaying(),
                        windowW, windowH, cameraPos, cameraTarget, camMode);
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char** argv) {
    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
    signal(SIGFPE,  crashHandler);

    // ---- Backend selection: graphics_api.cfg, then CLI overrides -----------
    RHI::GraphicsAPI api = RHI::GraphicsAPI::OpenGL;
    {
        std::ifstream cfg(RHI::graphicsApiConfigPath());
        std::string token;
        if (cfg && (cfg >> token)) {
            RHI::GraphicsAPI parsed = api;
            if (RHI::parseGraphicsApi(token, parsed)) {
                api = parsed;
                std::cout << "[EditorMain] graphics_api.cfg -> " << token << std::endl;
            } else {
                std::cerr << "[EditorMain] Unknown graphics API '" << token
                          << "' in " << RHI::graphicsApiConfigPath()
                          << " - using " << RHI::graphicsApiName(api) << std::endl;
            }
        }
    }

    bool noVsync = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-vsync") {
            noVsync = true;
        } else if (a == "--graphics" && i + 1 < argc) {
            RHI::GraphicsAPI parsed = api;
            if (RHI::parseGraphicsApi(argv[++i], parsed)) api = parsed;
        }
    }
    RHI::setSkipVSync(noVsync);
    if (noVsync) std::cout << "[EditorMain] VSync disabled (--no-vsync).\n";

#if !defined(RTT_EDITOR_VULKAN_SWAPCHAIN)
    if (api == RHI::GraphicsAPI::Vulkan) {
        std::cerr << "[EditorMain] The Vulkan editor path (swapchain scene + "
                     "BuildVulkanScene) is not part of this restored entry "
                     "point - falling back to OpenGL.\n";
        api = RHI::GraphicsAPI::OpenGL;
    }
#endif

    // ---- Window + graphics context ----------------------------------------
    std::unique_ptr<RHI::IRHI> rhi = RHI::createRHI(api);
    if (!rhi) {
        std::cerr << "[EditorMain] Failed to create " << RHI::graphicsApiName(api)
                  << " backend (unavailable on this machine)\n";
        return 1;
    }

    auto& renderConfig = Config::getRenderConfig();
    const int winW = renderConfig.defaultWindowWidth;
    const int winH = renderConfig.defaultWindowHeight;
    if (!rhi->initialize(winW, winH, "RTT Engine - Editor")) {
        std::cerr << "[EditorMain] Failed to create window / initialize "
                  << RHI::graphicsApiName(api) << std::endl;
        return 1;
    }
    std::cout << "[EditorMain] Graphics backend: " << RHI::graphicsApiName(api) << std::endl;

    GLFWwindow* window = rhi->window();
    if (!window) {
        std::cerr << "[EditorMain] Backend reported no GLFW window\n";
        return 1;
    }

    // Gamma-correct compositing + multisample resolve, same as the engine entry
    // point (the editor HUD is drawn on top of the tone-mapped scene).
    glEnable(GL_FRAMEBUFFER_SRGB);
    glEnable(GL_MULTISAMPLE);

    // ---- ImGui context + theme/fonts --------------------------------------
    Editor::ImGuiContext imguiCtx;
    if (!imguiCtx.initialize(window, "#version 430")) {
        std::cerr << "[EditorMain] Failed to initialize ImGui\n";
        rhi->shutdown();
        return 1;
    }

    ImGuiIO& io = ImGui::GetIO();
    EditorTheme::LoadEditorFonts(io, 15.0f);
    PhosphorImGui::Load(io, 14.0f);
    FaImGui::Load(io, 15.0f);

    bool outlinerVisible = g_editor.uiState.showOutliner;
    bool detailsVisible  = g_editor.uiState.showDetails;
    UIConfig::LoadConfig(&outlinerVisible, &detailsVisible);
    g_editor.uiState.showOutliner = outlinerVisible;
    g_editor.uiState.showDetails  = detailsVisible;

    EditorTheme::SetThemeMode(EditorTheme::ThemeMode::Dark);
    EditorTheme::ApplyTheme();
    EditorTheme::SetUIScale(1.0f);

    // ---- Editor application ------------------------------------------------
    Editor::EditorApplication& app = Editor::EditorApplication::getInstance();
    if (!app.initialize(winW, winH)) {
        std::cerr << "[EditorMain] Failed to initialize editor application\n";
        imguiCtx.shutdown();
        rhi->shutdown();
        return 1;
    }

    InitEditor();
    g_editor.uiState.showOutliner = outlinerVisible;
    g_editor.uiState.showDetails  = detailsVisible;

    static const char* kPlayCameraModeNames[] = { "Free", "Third-person", "First-person" };
    auto cameraModeName = []() -> const char* {
        int idx = g_editor.uiState.playCameraMode;
        if (idx < 0 || idx > 2) idx = 0;
        return kPlayCameraModeNames[idx];
    };

    int lastWindowW = winW;
    int lastWindowH = winH;

    app.setUiFrameHook([&](float /*dt*/) {
        PROFILE_CPU_SCOPE("Frame.UI");

        // The hook owns the ImGui frame AND the final buffer swap: render()
        // calls it instead of swapping so the HUD lands on top of the scene.
        imguiCtx.beginFrame();

        glfwGetWindowSize(window, &lastWindowW, &lastWindowH);
        if (lastWindowW <= 0) lastWindowW = 1;
        if (lastWindowH <= 0) lastWindowH = 1;

        const unsigned int viewportTexture = app.getRenderPipeline().getFramebufferTexture();
        const glm::vec3 cameraPos    = app.lastCameraPosition();
        const glm::vec3 cameraTarget = app.lastCameraTarget();

        RenderSharedEditorUI(g_editor, &app.getPlayController(), cameraModeName(),
                             viewportTexture, lastWindowW, lastWindowH, window,
                             io, app.lastProjectionMatrix(), app.lastViewMatrix(),
                             &cameraPos, &cameraTarget, /*swapchainBacked=*/false);

        imguiCtx.endFrame();
        glfwSwapBuffers(window);
    });

    app.run();

    // ---- Teardown (deterministic order: GL resources before the context) ---
    app.setUiFrameHook(nullptr);
    app.shutdown();
    CleanupEditor();
    Terrain::ClearTextureCache();
    imguiCtx.shutdown();
    if (rhi) rhi->shutdown();
    rhi.reset();

    std::cout << "[EditorMain] Shutdown complete.\n";
    return 0;
}
