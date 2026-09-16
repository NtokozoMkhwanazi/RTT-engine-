#include "ui.h"
#include <imgui_internal.h>   // ImRect for viewport-hover tracking
#include "PlayModeController.h"
#include "phosphor_imgui.h"
#include "phosphor_icons_codepoints.h"
#include "editor_theme.h"
#include "fa_imgui.h"
#include "ui_config.h"
#include "IconsFontAwesome6.h"
#include "editor_state.h"
#include "entity_manager.h"
#include "console.h"
#include "profiler.h"
#include "scene_manager.h"
#include "model_loader.h"
#include "undo_redo.h"
#include "world_manager.h"
#include "render_pipeline.h"
#include "ecs/components/Components.h"
#include "cameraSystem/flyCamera.h"
#include "gizmo_renderer.h"
#include "grid_renderer.h"
#include "shader_manager.h"
#include "rhi/RHI.h"
#include "geo_config_panel.h"
#include "ui_helpers.h"
#include <GLFW/glfw3.h>
#include <iostream>
#include <algorithm>
#include <vector>
#include <cstring>
#include <cstdio>
#include <filesystem>

namespace UI {

// Fixed editor layout constants (Unreal/Unity-style docked layout)
// MENU_BAR_HEIGHT tracks the main menu bar height with the JetBrains Mono
// UI font (15px + frame padding + border).
constexpr float MENU_BAR_HEIGHT     = 27.0f;
constexpr float TOOLBAR_HEIGHT      = 38.0f;
constexpr float LEFT_PANEL_WIDTH    = 280.0f;
constexpr float RIGHT_PANEL_WIDTH   = 300.0f;
constexpr float BOTTOM_PANEL_HEIGHT = 180.0f;
constexpr float STATUS_BAR_HEIGHT   = 24.0f;

static ImVec2 GetDisplaySize() { return ImGui::GetIO().DisplaySize; }

// Screen-space rect of the viewport panel + its window id, captured every
// frame by RenderViewport. UI::IsViewport3DHovered() uses them so the engine
// can hand the camera ownership of the mouse + keyboard while the cursor is
// over the viewport - but only when nothing else (menu, modal, other panel)
// is stacked on top of it.
static ImRect g_viewport3DRect;
static ImGuiID g_viewportWindowID = 0;

// Rect of the FBO image inside the viewport (below the header), in window
// coordinates. Captured every frame by RenderViewport; the engine reads it to
// map mouse input for gizmo dragging + picking. Zero until the first frame.
static ImRect g_viewportContentRect;

bool GetViewportContentRect(float& x, float& y, float& w, float& h) {
    x = g_viewportContentRect.Min.x;
    y = g_viewportContentRect.Min.y;
    w = g_viewportContentRect.Max.x - g_viewportContentRect.Min.x;
    h = g_viewportContentRect.Max.y - g_viewportContentRect.Min.y;
    return w > 1.0f && h > 1.0f;
}

bool IsViewport3DHovered() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx == nullptr) return false;
    const ImVec2 mp = ImGui::GetIO().MousePos;
    if (!g_viewport3DRect.Contains(mp)) return false;
    // Nothing else is on top: the hovered window (or one of its ancestors)
    // must be the viewport itself. This keeps menus, modals and docked
    // panels that overlap the viewport in charge of their own input.
    ImGuiWindow* hw = ctx->HoveredWindow;
    while (hw) {
        if (hw->ID == g_viewportWindowID) return true;
        hw = hw->ParentWindow;
    }
    return false;
}

// ============================================================================
// Menu bar
// ============================================================================
void RenderMenuBar(Editor::Editor& editor, const std::string& currentSceneFile) {
    if (!ImGui::BeginMainMenuBar()) return;

    // App identity at the far left, then the standard menus.
    const auto& theme = EditorTheme::Get();
    ImGui::TextColored(theme.accent, "RTT");
    ImGui::SameLine();
    ImGui::TextColored(theme.textDim, "ENGINE");
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem(ICON_FA_FILE_CIRCLE_PLUS " New Scene", "Ctrl+N")) {
                editor.world().shutdown();
                editor.world().init();
                UndoRedo::Clear();
                editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
                EditorConsole::Log("New scene created");
            }
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN " Open Scene...", "Ctrl+O")) {
                editor.uiState.showOpenScene = true;
            }
            if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK " Save", "Ctrl+S")) {
                SceneManager::SaveScene(editor.uiState.sceneFile, editor.world());
            }
            if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK " Save As...", "Ctrl+Shift+S")) {
                editor.uiState.showSaveAs = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_FILE_IMPORT " Import Model...")) {
                editor.uiState.showModelLoader = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_RIGHT_FROM_BRACKET " Exit", "Alt+F4")) {
                editor.setShouldClose(true);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            bool hasSel = (editor.selectedEntity() != ecs::INVALID_ENTITY_ID);
            if (ImGui::MenuItem(ICON_FA_ARROW_ROTATE_LEFT " Undo", "Ctrl+Z", false, UndoRedo::CanUndo())) {
                UndoRedo::Undo();
                editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
            }
            if (ImGui::MenuItem(ICON_FA_ARROW_ROTATE_RIGHT " Redo", "Ctrl+Y", false, UndoRedo::CanRedo())) {
                UndoRedo::Redo();
                editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_COPY " Duplicate", "Ctrl+D", false, hasSel)) {
                ecs::EntityID dup = EntityManager::DuplicateEntity(editor.selectedEntity());
                if (dup != ecs::INVALID_ENTITY_ID) editor.setSelectedEntity(dup);
            }
            if (ImGui::MenuItem(ICON_FA_TRASH " Delete", "Del", false, hasSel)) {
                EntityManager::DeleteEntity(editor.selectedEntity());
                editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_OBJECT_GROUP " Select All", "Ctrl+A", false, false)) {}
            ImGui::EndMenu();
        }

        // ---- Add: spawn engine entities (primitives, lights, cameras) ----
        if (ImGui::BeginMenu("Add")) {
            auto SpawnAndSelect = [&](ecs::Entity e) {
                if (e.isValid()) {
                    editor.setSelectedEntity(e.id);
                    editor.entityCache().dirty = true;
                }
            };
            if (ImGui::BeginMenu(ICON_FA_CUBES " Primitives")) {
                if (ImGui::MenuItem("Cube"))      SpawnAndSelect(EntityManager::CreatePrimitive(ecs::MeshType::Cube));
                if (ImGui::MenuItem("Sphere"))    SpawnAndSelect(EntityManager::CreatePrimitive(ecs::MeshType::Sphere));
                if (ImGui::MenuItem("Plane"))     SpawnAndSelect(EntityManager::CreatePrimitive(ecs::MeshType::Plane));
                if (ImGui::MenuItem("Cylinder"))  SpawnAndSelect(EntityManager::CreatePrimitive(ecs::MeshType::Cylinder));
                if (ImGui::MenuItem("Cone"))      SpawnAndSelect(EntityManager::CreatePrimitive(ecs::MeshType::Cone));
                if (ImGui::MenuItem("Torus"))     SpawnAndSelect(EntityManager::CreatePrimitive(ecs::MeshType::Torus));
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_LIGHTBULB " Point Light"))   SpawnAndSelect(EntityManager::CreateLight(glm::vec3(0, 4, 0)));
            if (ImGui::MenuItem(ICON_FA_CAMERA " Camera"))        SpawnAndSelect(EntityManager::CreateCamera(glm::vec3(0, 5, 10)));
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_FILE_IMPORT " Import Model...")) {
                editor.uiState.showModelLoader = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            bool showGrid = editor.showGrid();
            bool showGizmo = editor.showGizmo();
            if (ImGui::MenuItem(ICON_FA_TABLE_CELLS " Show Grid", nullptr, &showGrid)) editor.setShowGrid(showGrid);
            if (ImGui::MenuItem(ICON_FA_WAND_MAGIC_SPARKLES " Show Gizmo", nullptr, &showGizmo)) editor.setShowGizmo(showGizmo);
            ImGui::Separator();
            ImGui::MenuItem(ICON_FA_LIST " Outliner", nullptr, &editor.uiState.showOutliner);
            ImGui::MenuItem(ICON_FA_SLIDERS " Details", nullptr, &editor.uiState.showDetails);
            ImGui::MenuItem(ICON_FA_FOLDER " Content Browser", nullptr, &editor.uiState.showContentBrowser);
            ImGui::MenuItem(ICON_FA_TERMINAL " Console", nullptr, &editor.uiState.showConsole);
            ImGui::MenuItem(ICON_FA_GAUGE " Profiler", nullptr, &editor.uiState.showProfiler);
            ImGui::MenuItem(ICON_FA_SATELLITE " Geo Tracking", nullptr, &editor.uiState.showGameMode);
            ImGui::Separator();
            if (ImGui::BeginMenu(ICON_FA_MICROCHIP " Graphics")) {
                const RHI::GraphicsAPI active = RHI::activeGraphicsApi();
                const bool useVk = (active == RHI::GraphicsAPI::Vulkan);
                if (ImGui::MenuItem("OpenGL (low / optimized)", nullptr, !useVk)) {
                    RHI::setGraphicsApi(RHI::GraphicsAPI::OpenGL);
                }
                if (ImGui::MenuItem("Vulkan (high quality)", nullptr, useVk)) {
                    RHI::setGraphicsApi(RHI::GraphicsAPI::Vulkan);
                }
                ImGui::Separator();
                ImGui::TextDisabled("Backend for the next launch\n(restart required)");
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Play")) {
            bool playing = editor.isPlaying();
            bool paused = editor.wasPlaying() && !playing;
            if (ImGui::MenuItem(ICON_FA_PLAY " Play", "F5", playing)) editor.setPlaying(!playing);
            if (ImGui::MenuItem(ICON_FA_PAUSE " Pause", "F6", paused)) editor.setWasPlaying(playing ? !editor.wasPlaying() : false);
            if (ImGui::MenuItem(ICON_FA_STOP " Stop", "Esc", false, playing || paused)) {
                editor.setPlaying(false);
                editor.setWasPlaying(false);
            }
            ImGui::Separator();
            static float gameSpeed = 1.0f;
            if (ImGui::SliderFloat("Game Speed", &gameSpeed, 0.1f, 3.0f, "%.1fx")) {
                editor.setGameSpeed(gameSpeed);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Camera")) {
            const char* modes[] = { ICON_FA_COMPASS " Free", ICON_FA_CAMERA " Follow",
                                    ICON_FA_CIRCLE_DOT " Orbit", ICON_FA_ANGLE_DOWN " Top-Down",
                                    ICON_FA_USER " First-Person" };
            const char* keys[] = {"0", "1", "2", "3", "4"};
            for (int i = 0; i < 5; ++i) {
                if (ImGui::MenuItem(modes[i], keys[i], editor.uiState.playCameraMode == i)) {
                    editor.uiState.playCameraMode = i;
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled("0 Free: WASD + right-drag look\n"
                                "1-4 around bot: right-drag + scroll\n"
                                "Keys 0-4 switch modes");
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Geo")) {
            bool showGeoPanel = editor.uiState.showGameMode;
            if (ImGui::MenuItem(ICON_FA_SATELLITE " Geo Tracking Panel", "F8", &showGeoPanel)) {
                editor.uiState.showGameMode = showGeoPanel;
                // Jump the left panel to the Geo tab - index 3 in the fixed
                // tab bar {Outliner, Layers, World, Geo}.
                if (showGeoPanel) editor.scenePanelConfig.activeTabIndex = 3;
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("GPS Mode")) {
                // Drive the GeoAPI tracker - the same one the tracking tab and the
                // status bar read from, so all three stay in sync. The Mode enum
                // starts at DISABLED=0, so the simulated modes are values 1..4.
                auto& geoApi = editor.geospatialSystem().getGeoAPI();
                const char* modes[] = { ICON_FA_LOCATION_DOT " Simulated Static", ICON_FA_PERSON_WALKING " Simulated Walk",
                                        ICON_FA_CAR " Simulated Vehicle", ICON_FA_PLANE " Simulated Aircraft" };
                int current = (int)editor.geoPanelState.gpsModeIndex - 1;
                for (int i = 0; i < 4; ++i) {
                    if (ImGui::MenuItem(modes[i], nullptr, current == i)) {
                        editor.geoPanelState.gpsModeIndex = i + 1;
                        geoApi.setGPSMode((GPSTracker::Mode)(i + 1));
                        EditorConsole::Log(std::string("GPS mode: ") + modes[i]);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::TextDisabled("Origin: %.5f, %.5f, %.1f m",
                                editor.geoPanelState.originLat, editor.geoPanelState.originLon,
                                editor.geoPanelState.originAlt);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Graphics")) {
            // Render-backend selection. The ACTIVE backend is whatever this
            // session launched with; switching writes graphics_api.cfg and
            // takes effect on the next launch (a live hot-swap of two graphics
            // contexts mid-frame is not attempted - see rhi/RHI.h).
            ImGui::TextDisabled("Active: %s", RHI::graphicsApiName(RHI::activeGraphicsApi()));
            ImGui::Separator();
            bool curGL = RHI::activeGraphicsApi() == RHI::GraphicsAPI::OpenGL;
            if (ImGui::MenuItem(ICON_FA_DISPLAY " OpenGL (low/optimized)", nullptr, curGL)) {
                if (!curGL && RHI::setGraphicsApi(RHI::GraphicsAPI::OpenGL)) {
                    EditorConsole::Log("Graphics: OpenGL (applies on next launch)");
                }
            }
            bool curVk = RHI::activeGraphicsApi() == RHI::GraphicsAPI::Vulkan;
            if (ImGui::MenuItem(ICON_FA_BOLT " Vulkan (high quality)", nullptr, curVk)) {
                if (!curVk && RHI::setGraphicsApi(RHI::GraphicsAPI::Vulkan)) {
                    EditorConsole::Log("Graphics: Vulkan (applies on next launch)");
                } else if (!curVk) {
                    EditorConsole::Log("Graphics: Vulkan unavailable on this machine");
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled("Switch takes effect on restart");
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem(ICON_FA_CIRCLE_INFO " About")) {
                editor.setShowAbout(true);
            }
            ImGui::EndMenu();
        }

    ImGui::EndMainMenuBar();
}

void RenderPreferencesDialog(bool& showPreferences, Editor::Editor& editor) {
    if (!showPreferences) return;
    ImGui::OpenPopup("Preferences");
    if (ImGui::BeginPopupModal("Preferences", &showPreferences, ImGuiWindowFlags_AlwaysAutoResize)) {
        EditorTheme::SectionTitle("Editor Preferences");
        ImGui::Separator();

        // Theme switcher - applies immediately (next frame) and persists to
        // ui_panel.cfg so the choice survives restarts.
        const char* themeNames[] = { "Dark", "Light" };
        int themeIdx = (int)EditorTheme::GetThemeMode();
        if (ImGui::Combo("Theme", &themeIdx, themeNames, IM_ARRAYSIZE(themeNames))) {
            EditorTheme::SetThemeMode(themeIdx == 0 ? EditorTheme::ThemeMode::Dark
                                                    : EditorTheme::ThemeMode::Light);
            UIConfig::gThemeMode = themeIdx;
            UIConfig::SaveConfig(editor.uiState.showOutliner, editor.uiState.showDetails);
            EditorConsole::Log(themeIdx == 1 ? "Theme: Light (saved)" : "Theme: Dark (saved)");
        }
        ImGui::Separator();

        bool showGrid = editor.showGrid();
        bool showGizmo = editor.showGizmo();
        if (UI::CheckboxSafe("Show Grid", &showGrid)) editor.setShowGrid(showGrid);
        if (UI::CheckboxSafe("Show Gizmo", &showGizmo)) editor.setShowGizmo(showGizmo);
        ImGui::Separator();
        if (UI::ButtonSafe("Close", ImVec2(120, 0))) {
            showPreferences = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

// ============================================================================
// Scene file dialogs (Save As / Open) - simple filename modal, Unreal-style
// ============================================================================
void RenderSceneDialogs(Editor::Editor& editor) {
    // Save As...
    if (editor.uiState.showSaveAs) {
        ImGui::OpenPopup("Save Scene As");
    }
    if (ImGui::BeginPopupModal("Save Scene As", &editor.uiState.showSaveAs,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("File name (.json):");
        UI::InputTextSafe("##SaveAsName", editor.uiState.sceneFile, IM_ARRAYSIZE(editor.uiState.sceneFile));
        ImGui::Dummy(ImVec2(0, 6));
        if (UI::ButtonSafe("Save", ImVec2(120, 0))) {
            std::string file = editor.uiState.sceneFile;
            if (file.find(".json") == std::string::npos) file += ".json";
            if (SceneManager::SaveScene(file, editor.world())) {
                EditorConsole::Log("Scene saved: " + file);
            }
            snprintf(editor.uiState.sceneFile, sizeof(editor.uiState.sceneFile), "%s", file.c_str());
            editor.uiState.showSaveAs = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (UI::ButtonSafe("Cancel", ImVec2(120, 0))) {
            editor.uiState.showSaveAs = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Open Scene...
    if (editor.uiState.showOpenScene) {
        ImGui::OpenPopup("Open Scene");
    }
    if (ImGui::BeginPopupModal("Open Scene", &editor.uiState.showOpenScene,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Scene file (.json):");
        UI::InputTextSafe("##OpenName", editor.uiState.sceneFile, IM_ARRAYSIZE(editor.uiState.sceneFile));
        ImGui::Dummy(ImVec2(0, 6));
        if (UI::ButtonSafe("Open", ImVec2(120, 0))) {
            if (SceneManager::LoadScene(editor.uiState.sceneFile, editor.world())) {
                EditorConsole::Log("Scene loaded: " + std::string(editor.uiState.sceneFile));
                editor.entityCache().dirty = true;
            }
            editor.uiState.showOpenScene = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (UI::ButtonSafe("Cancel", ImVec2(120, 0))) {
            editor.uiState.showOpenScene = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Import Model dialog (lives in model_loader.cpp).
    UI::RenderModelLoader(editor.uiState.showModelLoader);
    UI::RenderPreferencesDialog(editor.uiState.showPreferences, editor);
}

// ============================================================================
// Toolbar - Unreal Engine style top toolbar (fixed)
// ============================================================================
void RenderToolbar(Editor::Editor& editor) {
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    UI::BeginFixedPanel("Toolbar", ImVec2(0, MENU_BAR_HEIGHT), ImVec2(displaySize.x, TOOLBAR_HEIGHT));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));

    const auto& theme = EditorTheme::Get();
    const ImVec2 tbSize(24, 24);

    // ----- File / Edit quick actions (left group) -----
    if (FaImGui::IconButton(ICON_FA_FLOPPY_DISK, false, "Save Scene", tbSize)) {
        SceneManager::SaveScene("scene.json", editor.world());
    }

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // ----- Transform tools (Move / Rotate / Scale + World/Local space) -----
    bool tActive = (editor.gizmoType() == GizmoRenderer::GizmoType::Translate);
    if (FaImGui::IconButton(ICON_FA_UP_DOWN_LEFT_RIGHT, tActive, "Translate (W)", tbSize)) {
        editor.setGizmoType(GizmoRenderer::GizmoType::Translate);
        GizmoRenderer::SetGizmoType(GizmoRenderer::GizmoType::Translate);
    }
    ImGui::SameLine();

    bool rActive = (editor.gizmoType() == GizmoRenderer::GizmoType::Rotate);
    if (FaImGui::IconButton(ICON_FA_ROTATE, rActive, "Rotate (E)", tbSize)) {
        editor.setGizmoType(GizmoRenderer::GizmoType::Rotate);
        GizmoRenderer::SetGizmoType(GizmoRenderer::GizmoType::Rotate);
    }
    ImGui::SameLine();

    bool sActive = (editor.gizmoType() == GizmoRenderer::GizmoType::Scale);
    if (FaImGui::IconButton(ICON_FA_MAXIMIZE, sActive, "Scale (R)", tbSize)) {
        editor.setGizmoType(GizmoRenderer::GizmoType::Scale);
        GizmoRenderer::SetGizmoType(GizmoRenderer::GizmoType::Scale);
    }
    ImGui::SameLine();

    bool worldSpace = (editor.spaceType() == GizmoRenderer::SpaceType::World);
    if (FaImGui::IconButton(ICON_FA_GLOBE, worldSpace, worldSpace ? "World Space" : "Local Space", tbSize)) {
        GizmoRenderer::SpaceType newSpace = worldSpace ? GizmoRenderer::SpaceType::Local : GizmoRenderer::SpaceType::World;
        editor.setSpaceType(newSpace);
        GizmoRenderer::SetSpaceType(newSpace);
    }

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // ----- Viewport toggles (wireframe / grid / snap) -----
    bool wf = editor.showWireframe() != 0;
    if (FaImGui::IconButton(ICON_FA_CUBE, wf, "Wireframe", tbSize)) {
        editor.setShowWireframe(wf ? 0 : 1);
        // Sync to render pipeline immediately (takes effect next frame)
        Render::RenderPipeline::getInstance().setWireframeMode(editor.showWireframe() != 0);
    }
    ImGui::SameLine();

    bool grid = editor.showGrid();
    if (FaImGui::IconButton(ICON_FA_TABLE_CELLS, grid, "Grid", tbSize)) {
        editor.setShowGrid(!grid);
    }
    ImGui::SameLine();

    bool snap = editor.uiState.snapEnabled;
    if (FaImGui::IconButton(ICON_FA_MAGNET, snap, "Snap to grid (World Settings)", tbSize)) {
        editor.uiState.snapEnabled = !snap;
    }

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // ----- Viewport camera modes (Free / Follow / Orbit / Top-Down / First-Person) -----
    const char* camIcons[5]  = { ICON_FA_COMPASS,    ICON_FA_CAMERA,  ICON_FA_CIRCLE_DOT,
                                 ICON_FA_ANGLE_DOWN, ICON_FA_USER };
    const char* camHints[5]  = { "Free camera (0): WASD + right-drag + scroll",
                                 "Follow bot (1)", "Orbit bot (2)",
                                 "Top-Down bot (3)", "First-Person bot (4)" };
    for (int i = 0; i < 5; ++i) {
        if (FaImGui::IconButton(camIcons[i], editor.uiState.playCameraMode == i, camHints[i], tbSize)) {
            editor.uiState.playCameraMode = i;
        }
        ImGui::SameLine();
    }
    ImGui::Separator();
    ImGui::SameLine();

    // ----- Geo tracking toggle (signature feature) -----
    bool geoPanel = editor.uiState.showGameMode;
    if (FaImGui::IconButton(ICON_FA_SATELLITE, geoPanel, "Geo Tracking (F8)", tbSize,
                            EditorTheme::ToU32(EditorTheme::WithAlpha(theme.accentGreen, 0.28f)))) {
        editor.uiState.showGameMode = !geoPanel;
        // Geo tab is index 3 in the fixed tab bar {Outliner, Layers, World, Geo}.
        if (editor.uiState.showGameMode) editor.scenePanelConfig.activeTabIndex = 3;
    }

    // ----- Play transport: pinned to the RIGHT edge of the toolbar -----
    // The left side holds tools/options; Play/Pause/Stop sit at the far right,
    // edge-aligned regardless of how wide the left groups are. Group = 3
    // buttons (24px each) + 2 gaps (4px) + a right margin.
    const float playGroupW = 3.0f * 24.0f + 2.0f * 4.0f + 8.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - playGroupW);

    bool playing = editor.isPlaying();
    if (FaImGui::IconButton(ICON_FA_PLAY, true, playing ? "Stop (Esc)" : "Play (F5)", tbSize,
                            EditorTheme::ToU32(playing ? theme.accentDanger : theme.accentGreen))) {
        editor.setPlaying(!playing);
    }
    ImGui::SameLine();

    bool paused = editor.wasPlaying() && !playing;
    if (FaImGui::IconButton(ICON_FA_PAUSE, paused, "Pause (F6)", tbSize,
                            EditorTheme::ToU32(paused ? theme.accentWarn : theme.frameBg))) {
        editor.setWasPlaying(playing ? !editor.wasPlaying() : false);
    }
    ImGui::SameLine();

    bool stopped = !playing && !paused;
    if (FaImGui::IconButton(ICON_FA_STOP, stopped, "Stop (Esc)", tbSize,
                            EditorTheme::ToU32(stopped ? theme.accentDim : theme.frameBg))) {
        editor.setPlaying(false);
        editor.setWasPlaying(false);
    }

    ImGui::PopStyleVar();
    UI::EndFixedPanel();
}

// ============================================================================
// Left panel - fixed Unreal-style tabbed sidebar
// ============================================================================
void RenderLeftPanel(Editor::Editor& editor) {
    auto& panelConfig = editor.scenePanelConfig;
    ImVec2 displaySize = GetDisplaySize();
    ImVec2 pos(0, MENU_BAR_HEIGHT + TOOLBAR_HEIGHT);
    ImVec2 size(LEFT_PANEL_WIDTH, displaySize.y - MENU_BAR_HEIGHT - TOOLBAR_HEIGHT - STATUS_BAR_HEIGHT);

    if (!UI::BeginFixedPanel("Scene Outliner", pos, size)) {
        UI::EndFixedPanel();
        return;
    }

    // Modern docked header strip + theme-matched tab bar.
    EditorTheme::PanelHeader("Scene", ICON_FA_LAYER_GROUP);
    ImGui::Dummy(ImVec2(0, 4));

    const char* tabs[] = { "Outliner", "Layers", "World", "Geo" };
    const int tabCount = IM_ARRAYSIZE(tabs);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float tabW = (avail.x - (tabCount - 1) * 4) / tabCount;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
    for (int i = 0; i < tabCount; ++i) {
        bool active = (panelConfig.activeTabIndex == i);
        if (EditorTheme::TabButton(tabs[i], active, ImVec2(tabW, 24))) panelConfig.activeTabIndex = i;
        if (i < tabCount - 1) ImGui::SameLine();
    }
    ImGui::PopStyleVar();

    ImGui::Separator();

    ecs::EntityID selected = editor.selectedEntity();
    switch (panelConfig.activeTabIndex) {
        case 0:
            RenderOutlinerPanel(selected, editor.entityCache(), editor.uiState.searchBuffer);
            // Sync the (by-reference updated) selection back into editor state
            // so the viewport gizmo, details panel and picking all agree with
            // the highlighted outliner row.
            editor.setSelectedEntity(selected);
            break;
        case 1: {
            // Layers: visibility + selectability toggles per scene layer.
            EditorTheme::SectionTitle("Layers");
            ImGui::Separator();
            static bool layerVisible[8] = {true, true, true, true, true, true, true, true};
            static bool layerLocked[8] = {false, false, false, false, false, false, false, false};
            static const char* layerNames[8] = {
                "Default", "Environment", "Characters", "Props",
                "Effects", "Geo Tracked", "UI", "Hidden"
            };
            for (int i = 0; i < 8; ++i) {
                ImGui::PushID(i);
                if (ImGui::Checkbox("##vis", &layerVisible[i])) {
                    // Visibility is advisory (render systems read entity flags);
                    // kept as editor state for future culling integration.
                }
                ImGui::SameLine();
                if (ImGui::Checkbox("##lock", &layerLocked[i])) {}
                ImGui::SameLine();
                ImGui::TextDisabled("%s", layerLocked[i] ? "\xEF\x84\x9E" : "");
                ImGui::SameLine();
                ImGui::Text("%s", layerNames[i]);
                ImGui::PopID();
            }
            ImGui::Separator();
            ImGui::TextDisabled("Layer visibility/locking is editor state\nfor future per-layer culling & selection.");
            break;
        }
        case 2:
            RenderWorldSettingsPanel(editor);
            break;
        case 3:
            // Geo tracking - the engine's signature feature: GPS fix, feeds,
            // trajectory prediction and storage, all live.
            UI::RenderGeoPanel(editor.geospatialSystem().getGeoAPI(), editor.geoPanelState);
            break;
        default:
            panelConfig.activeTabIndex = 0;
            break;
    }

    UI::EndFixedPanel();
}

// ============================================================================
// World settings panel (left panel tab)
// ============================================================================
void RenderWorldSettingsPanel(Editor::Editor& editor) {
    const auto& theme = EditorTheme::Get();
    EditorTheme::SectionTitle("World Settings");
    ImGui::Separator();

    bool showGrid = editor.showGrid();
    if (UI::CheckboxSafe("Show Grid", &showGrid)) editor.setShowGrid(showGrid);

    bool showGizmo = editor.showGizmo();
    if (UI::CheckboxSafe("Show Gizmo", &showGizmo)) editor.setShowGizmo(showGizmo);

    int wire = editor.showWireframe();
    bool wireframe = (wire != 0);
    if (UI::CheckboxSafe("Wireframe", &wireframe)) editor.setShowWireframe(wireframe ? 1 : 0);

    // Grid - live controls that re-init the GridRenderer when changed.
    ImGui::Separator();
    ImGui::TextColored(theme.textDim, "Grid");
    static float gridSpacing = 1.0f;
    static int gridSize = 20;
    bool gridChanged = false;
    gridChanged |= ImGui::DragFloat("Spacing", &gridSpacing, 0.1f, 0.1f, 20.0f);
    gridChanged |= ImGui::DragInt("Size", &gridSize, 1, 2, 200);
    if (gridChanged) {
        GridRenderer::Init(gridSize, gridSpacing);
    }

    // Terrain / rendering performance toggles (wired to the WorldManager).
    ImGui::Separator();
    ImGui::TextColored(theme.textDim, "Terrain");
    auto& wm = World::WorldManager::getInstance();
    bool lod = wm.isLODEnabled();
    if (UI::CheckboxSafe("LOD", &lod)) wm.setLODEnabled(lod);
    bool culling = wm.isCullingEnabled();
    if (UI::CheckboxSafe("Frustum Culling", &culling)) {
        wm.setCullingEnabled(culling);
        // Propagate to the RenderPipeline's Renderer so ECS scene meshes
        // (batched renderer) also stop/start frustum-rejecting draw calls.
        auto& rp = Render::RenderPipeline::getInstance();
        if (Renderer* r = rp.getRenderer()) r->SetFrustumCulling(culling);
    }
    float drawDist = wm.getDrawDistance();
    if (ImGui::DragFloat("Draw Distance", &drawDist, 10.0f, 50.0f, 2000.0f, "%.0f")) {
        wm.setDrawDistance(drawDist);
    }

    // Viewport prop culling / LOD / debug (Vulkan RHI scene path). The render
    // loop reads these live each frame.
    ImGui::Separator();
    ImGui::TextColored(theme.textDim, "Viewport Culling");
    auto& rp = Render::RenderPipeline::getInstance();
    if (ImGui::SliderInt("Max Visible Instances", &editor.uiState.worldSettings.maxVisibleInstances,
                      0, 2000, "%d")) {
        rp.setMaxVisibleInstances(editor.uiState.worldSettings.maxVisibleInstances);
        if (Renderer* r = rp.getRenderer()) {
            r->SetMaxVisibleInstances(editor.uiState.worldSettings.maxVisibleInstances);
        }
    }
    if (ImGui::Checkbox("Culling Debug", &editor.uiState.worldSettings.cullingDebug)) {
        rp.setCullingDebug(editor.uiState.worldSettings.cullingDebug);
        if (Renderer* r = rp.getRenderer()) {
            r->SetCullingDebug(editor.uiState.worldSettings.cullingDebug);
        }
    }
    if (editor.uiState.worldSettings.cullingDebug) {
        ImGui::Indent(12.0f);
        ImGui::TextColored(theme.textDim, "LOD bands (fraction of cull radius)");
        ImGui::SliderFloat("Near/Mid (LOD0/1)", &editor.uiState.worldSettings.lodBand1, 0.05f, 0.95f, "%.2f");
        ImGui::SliderFloat("Mid/Far (LOD1/2)", &editor.uiState.worldSettings.lodBand2, 0.05f, 0.95f, "%.2f");
        ImGui::SliderFloat("Far/Drop (LOD2+)", &editor.uiState.worldSettings.lodBand3, 0.05f, 0.99f, "%.2f");
        // Keep bands ordered so LOD0/1/2 always nest correctly.
        if (editor.uiState.worldSettings.lodBand1 > editor.uiState.worldSettings.lodBand2)
            editor.uiState.worldSettings.lodBand2 = editor.uiState.worldSettings.lodBand1;
        if (editor.uiState.worldSettings.lodBand2 > editor.uiState.worldSettings.lodBand3)
            editor.uiState.worldSettings.lodBand3 = editor.uiState.worldSettings.lodBand2;
        ImGui::Unindent(12.0f);
    }

    ImGui::Separator();
    ImGui::TextColored(theme.textDim, "Camera Mode");
    const char* camModes[] = { "Free", "Follow", "Orbit", "Top-Down", "First-Person" };
    if (ImGui::Combo("##PlayCamMode", &editor.uiState.playCameraMode, camModes, 5)) {
        // Handled by the engine loop in test.cpp via uiState.playCameraMode.
    }

    // Follow-camera tuning. Applied to the live follow cam by the engine loop
    // in test.cpp (Follow mode): distance is two-way synced so scroll zoom
    // also moves the slider; pitch is the resting look angle the camera eases
    // to (right-drag still temporarily tilts).
    ImGui::TextColored(theme.textDim, "Follow Camera");
    ImGui::SliderFloat("Distance", &editor.uiState.playCameraDistance, 0.5f, 9.0f, "%.1f");
    ImGui::SliderFloat("Pitch", &editor.uiState.playCameraPitch, -20.0f, 55.0f, "%.0f");

    // Gizmo snapping. The toggle is also on the viewport toolbar; the values
    // apply per transform tool (translate units / rotate degrees / scale
    // factor) and are read by the gizmo drag in test.cpp.
    ImGui::Separator();
    ImGui::TextColored(theme.textDim, "Gizmo Snapping");
    UI::CheckboxSafe("Enable Snap", &editor.uiState.snapEnabled);
    if (editor.uiState.snapEnabled) {
        ImGui::Indent(12.0f);
        ImGui::DragFloat("Translate", &editor.uiState.snapTranslate, 0.1f, 0.1f, 100.0f, "%.1f u");
        ImGui::DragFloat("Rotate", &editor.uiState.snapRotate, 1.0f, 1.0f, 90.0f, "%.0f deg");
        ImGui::DragFloat("Scale", &editor.uiState.snapScale, 0.05f, 0.05f, 2.0f, "%.2f x");
        ImGui::Unindent(12.0f);
    } else {
        ImGui::TextColored(theme.textDisabled, "Drags move freely; enable to snap.");
    }
}

// ============================================================================
// Right panel - fixed Unreal-style Details panel
// ============================================================================
void RenderRightPanel(Editor::Editor& editor,
                      const Editor::PlayModeController* play,
                      const char* camMode) {
    if (!editor.uiState.showDetails) return;

    ImVec2 displaySize = GetDisplaySize();
    ImVec2 pos(displaySize.x - RIGHT_PANEL_WIDTH, MENU_BAR_HEIGHT + TOOLBAR_HEIGHT);
    ImVec2 size(RIGHT_PANEL_WIDTH, displaySize.y - MENU_BAR_HEIGHT - TOOLBAR_HEIGHT - STATUS_BAR_HEIGHT);

    if (!UI::BeginFixedPanel("Details", pos, size)) {
        UI::EndFixedPanel();
        return;
    }

    const auto& theme = EditorTheme::Get();
    EditorTheme::PanelHeader("Details", ICON_FA_SLIDERS);

    // Play-mode debug lives HERE (inside the Details panel), not as an overlay
    // on the viewport - the 3D view must stay unobstructed so the character
    // is clearly visible. Rendered before the selection check so it shows even
    // when nothing is selected.
    if (play && editor.isPlaying() && play->isLoaded()) {
        RenderPlayModeDebug(*play, camMode);
    }

    ecs::EntityID selected = editor.selectedEntity();
    selected = EntityManager::ValidateOrClear(selected);

    if (selected == ecs::INVALID_ENTITY_ID) {
        if (!play || !editor.isPlaying() || !play->isLoaded()) {
            ImGui::TextColored(theme.textDisabled, "Select an actor to edit details");
        }
        UI::EndFixedPanel();
        return;
    }

    // Header: entity name (accent) + id (dim), then a hairline so the section
    // reads like a document title.
    const std::string selName = EntityManager::GetSafeEntityName(selected);
    ImGui::TextColored(theme.accentBright, "%s", selName.c_str());
    ImGui::SameLine();
    ImGui::TextColored(theme.textDisabled, "  id %u", selected);
    ImGui::Separator();

    ecs::World& world = editor.world();
    ecs::TransformComponent* t = world.getComponentArchetype<ecs::TransformComponent>(ecs::Entity{selected});
    ecs::ModelComponent* model = world.getComponentArchetype<ecs::ModelComponent>(ecs::Entity{selected});
    ecs::MeshComponent* m = world.getComponentArchetype<ecs::MeshComponent>(ecs::Entity{selected});
    ecs::GeospatialComponent* geo = world.getComponentArchetype<ecs::GeospatialComponent>(ecs::Entity{selected});

    RenderTransformSection(t);
    RenderModelSection(model);
    RenderMeshSection(m);

    if (geo && ImGui::CollapsingHeader("Geospatial", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("GeoDetail");
        ImGui::Text("Lat: %.8f", geo->latitude);
        ImGui::Text("Lon: %.8f", geo->longitude);
        ImGui::Text("Alt: %.2f m", geo->altitude);
        ImGui::PopID();
    }

    // Light component - fully editable (type, color, intensity, range).
    ecs::LightComponent* light = world.getComponentArchetype<ecs::LightComponent>(ecs::Entity{selected});
    if (light && ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("LightDetail");
        const char* lightTypes[] = { "Directional", "Point", "Spot", "Area" };
        int lt = glm::clamp((int)light->type, 0, 3);  // keep in sync with the enum
        if (ImGui::Combo("Type", &lt, lightTypes, 4)) light->type = (ecs::LightType)lt;
        ImVec4 lc = ImVec4(light->color.r, light->color.g, light->color.b, 1.0f);
        if (ImGui::ColorEdit3("Color", &lc.x)) light->color = glm::vec3(lc.x, lc.y, lc.z);
        UI::SliderFloatSafe("Intensity", &light->intensity, 0.0f, 10.0f);
        UI::SliderFloatSafe("Range", &light->range, 0.1f, 100.0f);
        UI::SliderFloatSafe("Temperature", &light->temperature, 1000.0f, 12000.0f, "%.0f K");
        UI::CheckboxSafe("Cast Shadows", &light->castShadows);
        ImGui::PopID();
    }

    // Camera component - show projection settings, edit FOV.
    ecs::CameraComponent* camComp = world.getComponentArchetype<ecs::CameraComponent>(ecs::Entity{selected});
    if (camComp && ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("CameraDetail");
        UI::SliderFloatSafe("FOV", &camComp->fov, 10.0f, 120.0f, "%.1f°");
        UI::SliderFloatSafe("Near", &camComp->nearPlane, 0.01f, 10.0f);
        UI::SliderFloatSafe("Far", &camComp->farPlane, 10.0f, 10000.0f, "%.0f");
        UI::CheckboxSafe("Orthographic", &camComp->isOrthographic);
        ImGui::Text("Aspect: %.3f", camComp->aspectRatio);
        ImGui::PopID();
    }

    // Component tags
    ImGui::Separator();
    ImGui::TextColored(theme.textDim, "Components");
    bool hasTrans = (t != nullptr);
    bool hasMesh = (m != nullptr);
    bool hasModel = (model != nullptr && model->isValid());
    bool hasCam = world.hasComponent<ecs::CameraComponent>(ecs::Entity{selected});
    bool hasLight = world.hasComponent<ecs::LightComponent>(ecs::Entity{selected});
    bool hasGeo = (geo != nullptr);

    // Component tags - two compact rows of three so nothing clips in the
    // 300px panel (the old hardcoded SameLine offsets ran past the edge).
    const ImVec4 tagOn = theme.accentGreen;
    const ImVec4 tagOff = theme.textDisabled;
    auto Tag = [&](const char* label, bool on) {
        ImGui::TextColored(on ? tagOn : tagOff, "%s", label);
        ImGui::SameLine(0.0f, 14.0f);
    };
    Tag("Transform", hasTrans);
    Tag("Mesh", hasMesh);
    Tag("Model", hasModel);
    ImGui::NewLine();
    Tag("Camera", hasCam);
    Tag("Light", hasLight);
    Tag("Geo", hasGeo);

    UI::EndFixedPanel();
}

void RenderModelSection(ecs::ModelComponent* model) {
    if (!model || !model->isValid()) return;
    if (ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Model");
        UI::CheckboxSafe("Visible", &model->visible);
        ImGui::PopID();
    }
}

// ============================================================================
// Outliner panel - renders inside the fixed left panel
// ============================================================================
void RenderOutlinerPanel(ecs::EntityID& selected, EntityCache& entityCache, const char* searchBuffer) {
    // Safety guard: no ImGui context -> nothing to render into.
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    const auto& theme = EditorTheme::Get();
    // Search input with safe wrapper
    UI::InputTextSafe("##Search", const_cast<char*>(searchBuffer), 128);
    ImGui::Separator();
    ImGui::TextColored(theme.textDim, "%zu entities", entityCache.size());
    ImGui::Separator();

    // Rebuild cache if dirty (not every frame)
    // Note: RebuildEntityCache needs the world, which should be passed from caller
    // For now, skip auto-rebuild here; caller should handle it
    if (entityCache.dirty) {
        // RebuildEntityCache will be called externally with proper world reference
    }

    // Render from cache - zero ECS queries. Each row is drawn manually so we
    // get per-type icon colors, entity names and right-aligned component badges
    // (the classic engine-outliner look).
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 1));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* phFont = PhosphorImGui::GetFont();
    ImFont* faFont = FaImGui::GetFont();

    for (const auto& entry : entityCache.entries) {
        // Validate entity still exists
        if (!EntityManager::EntityExists(entry.id)) {
            if (selected == entry.id) {
                selected = ecs::INVALID_ENTITY_ID;
            }
            continue;
        }

        ImGui::PushID((int)entry.id);

        // Full-width clickable row (invisible button defines the hit area).
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float rowH = ImGui::GetTextLineHeight() + 5.0f;
        const float rowW = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##row", ImVec2(rowW, rowH));
        const bool clicked = ImGui::IsItemClicked(0);
        const bool hovered = ImGui::IsItemHovered();
        if (clicked) selected = entry.id;

        const bool isSel = (selected == entry.id);
        const ImVec2 rowMax(rowMin.x + rowW, rowMin.y + rowH);

        // Per-type icon color.
        ImU32 iconCol;
        switch (entry.kind) {
            case EntityCache::Kind::Camera: iconCol = EditorTheme::ToU32(theme.accent); break;
            case EntityCache::Kind::Light:  iconCol = EditorTheme::ToU32(theme.accentWarn); break;
            case EntityCache::Kind::Geo:    iconCol = EditorTheme::ToU32(theme.accentGreen); break;
            case EntityCache::Kind::Model:  iconCol = EditorTheme::ToU32(theme.accent); break;
            default:                        iconCol = EditorTheme::ToU32(theme.textDim); break;
        }

        // Selection / hover background (drawn under the content). Rounded rows
        // with a small accent notch on the selected row.
        const float kRowRounding = 3.0f;
        if (isSel) {
            dl->AddRectFilled(rowMin, rowMax, EditorTheme::ToU32(theme.header), kRowRounding);
            dl->AddRectFilled(ImVec2(rowMin.x + 2.0f, rowMin.y + 3.0f),
                              ImVec2(rowMin.x + 4.0f, rowMax.y - 3.0f),
                              EditorTheme::ToU32(theme.accent), 1.0f);
        } else if (hovered) {
            dl->AddRectFilled(rowMin, rowMax,
                              EditorTheme::ToU32(EditorTheme::WithAlpha(theme.headerHovered, 0.45f)),
                              kRowRounding);
        }

        // Per-type Phosphor icon.
        const float iconSize = 13.0f;
        const ImVec2 iconPos(rowMin.x + 6.0f, rowMin.y + (rowH - iconSize) * 0.5f);
        if (phFont) {
            dl->AddText(phFont, iconSize, iconPos, iconCol, entry.iconCodepoint);
        } else {
            dl->AddText(iconPos, iconCol, entry.icon);
        }

        // Label: entity name in the foreground color, id dimmed after it.
        const ImU32 textCol = isSel ? EditorTheme::ToU32(theme.accentBright)
                                    : EditorTheme::ToU32(theme.text);
        const ImU32 idCol = isSel
            ? EditorTheme::ToU32(EditorTheme::WithAlpha(theme.accentBright, 0.6f))
            : EditorTheme::ToU32(theme.textDisabled);
        const float labelX = iconPos.x + 18.0f;
        if (entry.name[0]) {
            dl->AddText(ImVec2(labelX, rowMin.y + 2.0f), textCol, entry.name);
            char idBuf[16];
            snprintf(idBuf, sizeof(idBuf), "%u", entry.id);
            const float nameW = ImGui::CalcTextSize(entry.name).x;
            dl->AddText(ImVec2(labelX + nameW + 8.0f, rowMin.y + 2.0f), idCol, idBuf);
        } else {
            char idBuf[32];
            snprintf(idBuf, sizeof(idBuf), "%s %u", entry.icon, entry.id);
            dl->AddText(ImVec2(labelX, rowMin.y + 2.0f), textCol, idBuf);
        }

        // Right-aligned component badges (FontAwesome mini icons).
        if (faFont) {
            float bx = rowMax.x - 10.0f;
            const float bSize = 11.0f;
            const ImVec2 bPos(0, rowMin.y + 2.0f);
            auto Badge = [&](bool on, ImU32 col, const char* cp) {
                if (!on) return;
                bx -= 15.0f;
                dl->AddText(faFont, bSize, ImVec2(bx, bPos.y), col, cp);
            };
            Badge(entry.hasGeo,    EditorTheme::ToU32(theme.accentGreen), ICON_FA_LOCATION_DOT);
            Badge(entry.hasCamera, EditorTheme::ToU32(theme.accent),      ICON_FA_CAMERA);
            Badge(entry.hasLight,  EditorTheme::ToU32(theme.accentWarn),  ICON_FA_LIGHTBULB);
            Badge(entry.hasModel || entry.hasMesh, EditorTheme::ToU32(theme.textDim), ICON_FA_CUBE);
        }

        if (ImGui::BeginPopupContextItem(("ctx##" + std::to_string(entry.id)).c_str())) {
            if (ImGui::MenuItem("Duplicate")) {
                ecs::EntityID dup = EntityManager::DuplicateEntity(entry.id);
                if (dup != ecs::INVALID_ENTITY_ID) {
                    selected = dup;
                    entityCache.dirty = true;
                }
            }
            if (ImGui::MenuItem("Delete")) {
                EntityManager::DeleteEntity(entry.id);
                if (selected == entry.id) selected = ecs::INVALID_ENTITY_ID;
                entityCache.dirty = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();

    if (ImGui::BeginPopupContextWindow("##Create")) {
        ImGui::TextWrapped("Load models via Asset Browser");
        ImGui::EndPopup();
    }
}

// ============================================================================
// Details panel
// ============================================================================
void RenderDetailsPanel(ecs::World& world, ecs::EntityID& selected) {
    // Safety guard: no ImGui context -> nothing to render into.
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    // Validate entity exists before rendering
    selected = EntityManager::ValidateOrClear(selected);
    
    if (!UI::BeginPanel("Details")) {
        UI::EndPanel();
        return;
    }
    
    if (selected == ecs::INVALID_ENTITY_ID) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "No entity selected");
        UI::EndPanel();
        return;
    }

    const auto& theme = EditorTheme::Get();
    ImGui::Separator();
    ImGui::TextColored(theme.accentBright, "Selected: Entity %u", selected);
    ImGui::Separator();

    // Get components once, cache locally
    ecs::TransformComponent* t = world.getComponentArchetype<ecs::TransformComponent>(ecs::Entity{selected});
    ecs::ModelComponent* model = world.getComponentArchetype<ecs::ModelComponent>(ecs::Entity{selected});
    ecs::MeshComponent* m = world.getComponentArchetype<ecs::MeshComponent>(ecs::Entity{selected});

    // Check for geo component
    ecs::GeospatialComponent* geo = world.getComponentArchetype<ecs::GeospatialComponent>(ecs::Entity{selected});

    if (t && ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Transform");
        glm::vec3 p = t->position;
        if (UI::DragFloatClamped("Position X", &p.x, 0.1f, -10000.0f, 10000.0f)) t->position.x = p.x;
        if (UI::DragFloatClamped("Position Y", &p.y, 0.1f, -10000.0f, 10000.0f)) t->position.y = p.y;
        if (UI::DragFloatClamped("Position Z", &p.z, 0.1f, -10000.0f, 10000.0f)) t->position.z = p.z;

        glm::vec3 r = glm::degrees(glm::eulerAngles(t->rotation));
        if (UI::DragFloatClamped("Rotation X", &r.x, 1.0f, -180.0f, 180.0f)) {
            t->setEulerAngles(glm::radians(r));
        }
        if (UI::DragFloatClamped("Rotation Y", &r.y, 1.0f, -180.0f, 180.0f)) {
            t->setEulerAngles(glm::radians(r));
        }
        if (UI::DragFloatClamped("Rotation Z", &r.z, 1.0f, -180.0f, 180.0f)) {
            t->setEulerAngles(glm::radians(r));
        }

        glm::vec3 s = t->scale;
        if (UI::DragFloatClamped("Scale X", &s.x, 0.01f, 0.01f, 1000.0f)) t->scale.x = s.x;
        if (UI::DragFloatClamped("Scale Y", &s.y, 0.01f, 0.01f, 1000.0f)) t->scale.y = s.y;
        if (UI::DragFloatClamped("Scale Z", &s.z, 0.01f, 0.01f, 1000.0f)) t->scale.z = s.z;
        ImGui::PopID();
    }

    if (model && model->isValid() && ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen)) {
        UI::CheckboxSafe("Visible", &model->visible);
    }

    if (m && ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Mesh");
        ImVec4 c = ImVec4(m->color.r, m->color.g, m->color.b, 1.0f);
        if (ImGui::ColorEdit3("Albedo", &c.x)) m->color = glm::vec3(c.x, c.y, c.z);
        UI::CheckboxSafe("Visible", &m->visible);
        UI::SliderFloatSafe("Metallic", &m->metallic, 0.0f, 1.0f);
        UI::SliderFloatSafe("Roughness", &m->roughness, 0.0f, 1.0f);
        ImGui::PopID();
    }

    // Geospatial component display
    if (geo && ImGui::CollapsingHeader("Geospatial", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Geospatial");
        ImGui::Text("Lat: %.8f", geo->latitude);
        ImGui::Text("Lon: %.8f", geo->longitude);
        ImGui::Text("Alt: %.2f m", geo->altitude);
        ImGui::Text("Accuracy: ±%.2f m", geo->horizontalAccuracy);
        ImGui::Text("System: %s", geo->coordinateSystem.c_str());
        ImGui::PopID();
    }

    // Component list
    ImGui::Separator();
    ImGui::TextColored(theme.textDim, "Components");
    ImGui::Separator();

    bool hasTrans = (t != nullptr);
    bool hasMesh = (m != nullptr);
    bool hasModel = (model != nullptr && model->isValid());
    bool hasCam = world.hasComponent<ecs::CameraComponent>(ecs::Entity{selected});
    bool hasLight = world.hasComponent<ecs::LightComponent>(ecs::Entity{selected});
    bool hasGeo = (geo != nullptr);

    const ImVec4 tagOn = theme.accentGreen;
    const ImVec4 tagOff = theme.textDisabled;
    ImGui::TextColored(hasTrans ? tagOn : tagOff, "Transform");
    ImGui::SameLine(100);
    ImGui::TextColored(hasMesh ? tagOn : tagOff, "Mesh");
    ImGui::SameLine(160);
    ImGui::TextColored(hasModel ? tagOn : tagOff, "Model");
    ImGui::SameLine(220);
    ImGui::TextColored(hasCam ? tagOn : tagOff, "Camera");
    ImGui::SameLine(280);
    ImGui::TextColored(hasLight ? tagOn : tagOff, "Light");
    ImGui::SameLine(340);
    ImGui::TextColored(hasGeo ? tagOn : tagOff, "Geo");
    
    UI::EndPanel();
}

// ============================================================================
// Bottom panel - fixed Content Browser / Output Log / Profiler
// ============================================================================
void RenderBottomPanel(Editor::Editor& editor, float fps) {
    ImVec2 displaySize = GetDisplaySize();
    ImVec2 pos(LEFT_PANEL_WIDTH, displaySize.y - STATUS_BAR_HEIGHT - BOTTOM_PANEL_HEIGHT);
    ImVec2 size(displaySize.x - LEFT_PANEL_WIDTH - RIGHT_PANEL_WIDTH, BOTTOM_PANEL_HEIGHT);

    if (!UI::BeginFixedPanel("Content Browser / Output Log", pos, size)) {
        UI::EndFixedPanel();
        return;
    }

    // Modern docked header strip + theme-matched tab bar.
    EditorTheme::PanelHeader("Content & Output", ICON_FA_TERMINAL);
    ImGui::Dummy(ImVec2(0, 4));

    const char* tabs[] = { "Content Browser", "Output Log", "Profiler" };
    const int tabCount = IM_ARRAYSIZE(tabs);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float tabW = (avail.x - (tabCount - 1) * 4) / tabCount;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
    for (int i = 0; i < tabCount; ++i) {
        bool active = (editor.uiState.bottomPanelTab == i);
        if (EditorTheme::TabButton(tabs[i], active, ImVec2(tabW, 24))) editor.uiState.bottomPanelTab = i;
        if (i < tabCount - 1) ImGui::SameLine();
    }
    ImGui::PopStyleVar();

    ImGui::Separator();

    switch (editor.uiState.bottomPanelTab) {
        case 0:
            RenderContentBrowserPanel();
            break;
        case 1:
            UI::RenderConsolePanel();
            break;
        case 2:
            RenderProfilerPanel(editor, fps);
            break;
        default:
            editor.uiState.bottomPanelTab = 0;
            break;
    }

    UI::EndFixedPanel();
}

void RenderContentBrowserPanel() {
    static char pathBuffer[256] = "assets";
    UI::InputTextSafe("Path", pathBuffer, IM_ARRAYSIZE(pathBuffer));
    ImGui::SameLine();
    static bool scanned = false;
    static std::vector<std::string> modelFiles;
    if (UI::ButtonSafe("Scan")) {
        modelFiles.clear();
        try {
            namespace fs = std::filesystem;
            std::string root = pathBuffer;
            if (root.empty()) root = "assets";
            if (!fs::exists(root)) root = ".";
            for (auto& entry : fs::recursive_directory_iterator(root)) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                if (ext == ".fbx" || ext == ".gltf" || ext == ".glb" || ext == ".obj" || ext == ".dae") {
                    modelFiles.push_back(entry.path().string());
                }
            }
            std::sort(modelFiles.begin(), modelFiles.end());
            scanned = true;
        } catch (const std::exception& e) {
            EditorConsole::Log(std::string("Asset scan failed: ") + e.what());
        }
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.5f, 1), "%zu models", modelFiles.size());
    ImGui::Separator();

    if (!scanned) {
        ImGui::TextDisabled("Click 'Scan' to list models under the path.");
    }

    // Scrolling list; double-click or Load button spawns the model.
    ImGui::BeginChild("##AssetList", ImVec2(0, -28), true);
    static int selectedAsset = -1;
    for (size_t i = 0; i < modelFiles.size(); ++i) {
        bool sel = ((int)i == selectedAsset);
        if (ImGui::Selectable(modelFiles[i].c_str(), sel, ImGuiSelectableFlags_AllowDoubleClick)) {
            selectedAsset = (int)i;
            if (ImGui::IsMouseDoubleClicked(0)) {
                ecs::Entity e = EntityManager::CreateModel(modelFiles[i], glm::vec3(0, 0, 0));
                if (e.isValid()) {
                    g_editor.setSelectedEntity(e.id);
                    g_editor.entityCache().dirty = true;
                    EditorConsole::Log("Spawned " + modelFiles[i]);
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (UI::ButtonSafe("Spawn Selected", ImVec2(140, 0)) && selectedAsset >= 0 &&
        (size_t)selectedAsset < modelFiles.size()) {
        ecs::Entity e = EntityManager::CreateModel(modelFiles[selectedAsset], glm::vec3(0, 0, 0));
        if (e.isValid()) {
            g_editor.setSelectedEntity(e.id);
            g_editor.entityCache().dirty = true;
            EditorConsole::Log("Spawned " + modelFiles[selectedAsset]);
        }
    }
}

void RenderProfilerPanel(Editor::Editor& editor, float fps) {
    const auto& theme = EditorTheme::Get();
    auto snap = Profiler::Instance().snapshot();

    // Use the Profiler's frame timing if available, fall back to ImGui's FPS
    double frameMs = snap.frameTimeMs > 0.0
        ? snap.frameTimeMs
        : (1000.0 / (fps > 0 ? fps : 60));
    double displayFps = snap.fps > 0.0 ? snap.fps : fps;

    ImGui::Columns(2, "ProfilerStats", true);

    // --- Frame timing ---
    ImGui::TextColored(theme.textDim, "FPS");            ImGui::NextColumn();
    ImGui::TextColored(theme.accentGreen, "%.1f", displayFps); ImGui::NextColumn();

    ImGui::TextColored(theme.textDim, "Frame Time");     ImGui::NextColumn();
    ImGui::TextColored(theme.accent, "%.2f ms", frameMs); ImGui::NextColumn();

    ImGui::TextColored(theme.textDim, "Entities");       ImGui::NextColumn();
    ImGui::TextColored(theme.accentWarn, "%zu", editor.world().getEntityCount()); ImGui::NextColumn();

    // --- Rendering stats (visible/culled instances from culling debug) ---
    {
        float x, y, w, h;
        if (GetViewportContentRect(x, y, w, h)) {
            const Render::RenderPipeline& rp = Render::RenderPipeline::getInstance();
            const auto& stats = rp.getStats();
            ImGui::TextColored(theme.textDim, "Draw Calls");     ImGui::NextColumn();
            ImGui::TextColored(theme.accent, "%zu", stats.drawCalls); ImGui::NextColumn();

            if (stats.visibleInstances > 0 || stats.culledInstances > 0) {
                ImGui::TextColored(theme.textDim, "Visible Instances");  ImGui::NextColumn();
                ImGui::TextColored(theme.accentGreen, "%zu", stats.visibleInstances); ImGui::NextColumn();

                ImGui::TextColored(theme.textDim, "Culled Instances");   ImGui::NextColumn();
                ImGui::TextColored(theme.accentWarn, "%zu", stats.culledInstances); ImGui::NextColumn();
            }
        }
    }

    // --- CPU timing regions ---
    if (!snap.cpuRegions.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(theme.accent, "CPU Timings");
        ImGui::Separator();
        for (const auto& [name, stat] : snap.cpuRegions) {
            ImGui::TextColored(theme.textDim, "%s", name.c_str()); ImGui::NextColumn();
            ImGui::Text("%.3f ms (avg %.3f / %.3f)",
                stat.lastMs, stat.avgMs, stat.maxMs); ImGui::NextColumn();
        }
    }

    // --- Numeric stats from providers (KD-tree, MotionMatcher, Memory, GPU, SIMD) ---
    ImGui::Spacing();
    ImGui::TextColored(theme.accent, "Subsystems");
    ImGui::Separator();

    for (const auto& [key, val] : snap.numericStats) {
        // Skip duplicate frame timing we already show above
        if (key == "GPU.FrameTimeMs") continue;

        ImGui::TextColored(theme.textDim, "%s", key.c_str()); ImGui::NextColumn();

        // Format the value based on the key
        if (val.label.find("m/s") != std::string::npos) {
            ImGui::Text("%.2f %s", val.value, val.label.c_str());
        } else if (val.label.find("ms") != std::string::npos) {
            ImGui::Text("%.2f %s", val.value, val.label.c_str());
        } else if (val.label.find("fps") != std::string::npos) {
            ImGui::Text("%.1f %s", val.value, val.label.c_str());
        } else if (val.value >= 10000.0 &&
                   (key.find("Memory") != std::string::npos || key.find("Bytes") != std::string::npos)) {
            // Human-readable byte formatting
            double mb = val.value / (1024.0 * 1024.0);
            ImGui::Text("%.1f MB%s", mb, val.label.empty() ? "" : (" " + val.label).c_str());
        } else if (val.value >= 1000.0 &&
                   (key.find("Count") != std::string::npos)) {
            ImGui::Text("%.0f%s", val.value, val.label.empty() ? "" : (" " + val.label).c_str());
        } else {
            if (val.value == static_cast<int>(val.value) && std::abs(val.value) < 1e9)
                ImGui::Text("%.0f%s", val.value, val.label.empty() ? "" : (" " + val.label).c_str());
            else
                ImGui::Text("%.2f%s", val.value, val.label.empty() ? "" : (" " + val.label).c_str());
        }
        ImGui::NextColumn();
    }

    // --- GPU timing ---
    if (snap.numericStats.size() > 0) {
        bool hasGpu = false;
        for (const auto& [key, _] : snap.numericStats) {
            if (key.find("GPU.") == 0) { hasGpu = true; break; }
        }
        if (hasGpu) {
            ImGui::Spacing();
            ImGui::TextColored(theme.accent, "GPU");
            ImGui::Separator();
            for (const auto& [key, val] : snap.numericStats) {
                if (key.find("GPU.") != 0) continue;
                ImGui::TextColored(theme.textDim, "%s", key.c_str()); ImGui::NextColumn();
                ImGui::Text("%.2f %s", val.value, val.label.c_str()); ImGui::NextColumn();
            }
        }
    }

    ImGui::Columns(1);

    flyCamera* camera = editor.camera();
    if (camera) {
        ImGui::Text("Camera Position: (%.1f, %.1f, %.1f)", camera->Position.x, camera->Position.y, camera->Position.z);
    }

    // --- Profiling controls ---
    if (ImGui::TreeNode("Profiling Settings")) {
        bool enabled = Profiler::Instance().isEnabled();
        if (ImGui::Checkbox("Enabled", &enabled))
            Profiler::Instance().setEnabled(enabled);
        float interval = Profiler::Instance().getLogIntervalSeconds();
        if (ImGui::SliderFloat("Console Flush Interval", &interval, 1.0f, 30.0f, "%.0f s"))
            Profiler::Instance().setLogIntervalSeconds(interval);

        // Category checkboxes
        if (ImGui::TreeNode("Log Categories")) {
            for (int cat = 0; cat < 7; ++cat) {
                EditorConsole::LogCategory c = static_cast<EditorConsole::LogCategory>(cat);
                bool en = EditorConsole::IsCategoryEnabled(c);
                if (ImGui::Checkbox(EditorConsole::CategoryName(c), &en))
                    EditorConsole::SetCategoryEnabled(c, en);
            }
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }
}

void RenderConsolePanel() {
    const auto& theme = EditorTheme::Get();
    const auto& msgs = EditorConsole::GetMessages();

    // Level filter
    int filter = EditorConsole::GetFilter();
    const char* levels[] = {"All Info", "Warning+", "Error"};
    if (ImGui::Combo("Filter", &filter, levels, IM_ARRAYSIZE(levels)))
        EditorConsole::SetFilter(filter);

    // Show last 50 messages (was 20 — creators need more context now that
    // we also log profiling/memory/KD-tree stats)
    int n = std::min(50, (int)msgs.size());
    for (int i = std::max(0, (int)msgs.size() - n); i < (int)msgs.size(); i++) {
        const auto& m = msgs[i];
        ImVec4 c = (m.level == 0) ? theme.textDim :
                  (m.level == 1) ? theme.accentWarn :
                  theme.accentDanger;

        // Category badge (colored tag)
        ImVec4 catCol = (m.category == EditorConsole::LogCategory::Memory)     ? ImVec4(1.0f, 0.6f, 0.6f, 1.0f) :
                        (m.category == EditorConsole::LogCategory::Profiling) ? ImVec4(0.6f, 1.0f, 0.6f, 1.0f) :
                        (m.category == EditorConsole::LogCategory::Animation) ? ImVec4(0.8f, 0.8f, 1.0f, 1.0f) :
                        (m.category == EditorConsole::LogCategory::KDTree)     ? ImVec4(1.0f, 0.8f, 0.4f, 1.0f) :
                        (m.category == EditorConsole::LogCategory::SIMD)      ? ImVec4(0.6f, 1.0f, 1.0f, 1.0f) :
                        (m.category == EditorConsole::LogCategory::Rendering)  ? ImVec4(0.8f, 0.6f, 1.0f, 1.0f) :
                        theme.textDim;

        // [CATEGORY] message text
        ImGui::PushStyleColor(ImGuiCol_Text, catCol);
        char badge[32];
        std::snprintf(badge, sizeof(badge), "[%s]", EditorConsole::CategoryName(m.category));
        ImGui::Text("%s", badge);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        ImGui::Text("%s", m.text.c_str());
        ImGui::PopStyleColor();
    }
}

// ============================================================================
// Viewport - fixed center panel, always the largest area
// ============================================================================
void RenderViewport(Editor::Editor& editor, GLuint viewportTexture,
                 int windowW, int windowH, GLFWwindow* window, ImGuiIO* io,
                 const glm::mat4& projection, const glm::mat4& view,
                 const char* camModeName, const glm::vec3* cameraPos,
                 const glm::vec3* cameraTarget, bool swapchainBacked) {
    // Safety guard 1: no ImGui context -> nothing to render into.
    if (ImGui::GetCurrentContext() == nullptr || io == nullptr) {
        return;
    }

    const auto& theme = EditorTheme::Get();

    ImVec2 displaySize = GetDisplaySize();
    float vpX = LEFT_PANEL_WIDTH;
    float vpY = MENU_BAR_HEIGHT + TOOLBAR_HEIGHT;
    float vpW = displaySize.x - LEFT_PANEL_WIDTH - RIGHT_PANEL_WIDTH;
    if (vpW < 1.0f) vpW = 1.0f;
    float vpH = displaySize.y - MENU_BAR_HEIGHT - TOOLBAR_HEIGHT - BOTTOM_PANEL_HEIGHT - STATUS_BAR_HEIGHT;
    if (vpH < 1.0f) vpH = 1.0f;

    ImVec2 vpPos(vpX, vpY);
    ImVec2 vpSize(vpW, vpH);

    // Push a darker frame background so the FBO content is visually distinct from
    // the editor's UI panels. On the Vulkan backend the viewport is SWAPCHAIN-
    // backed (the RHI renders the scene into the swapchain, not an FBO texture):
    // push a fully transparent background so the live scene shows through.
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          swapchainBacked ? ImVec4(0.0f, 0.0f, 0.0f, 0.0f)
                                          : ImVec4(0.05f, 0.05f, 0.07f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImGuiWindowFlags viewportFlags =
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (!UI::BeginFixedPanel("Viewport", vpPos, vpSize, viewportFlags)) {
        UI::EndFixedPanel();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return;
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // Track the FULL viewport panel (header included) for hover detection.
    g_viewportWindowID = ImGui::GetCurrentWindow()->ID;
    ImVec2 winPos = ImGui::GetWindowPos();
    ImVec2 winSize = ImGui::GetWindowSize();
    g_viewport3DRect = ImRect(winPos, ImVec2(winPos.x + winSize.x, winPos.y + winSize.y));

    // ----- 3D viewport content area -----
    // Layout: a 28px header strip (title + Lit/Wire/Normals/Unlit mode
    // buttons) at the top, then the FBO image filling the rest BELOW it. The
    // image never overlaps the header, so the content rect exposed for
    // picking/gizmo math exactly matches the visible scene (no offset).
    ImVec2 contentAvail = ImGui::GetContentRegionAvail();
    const float headerH = 28.0f;
    ImVec2 imgSize(contentAvail.x, contentAvail.y - headerH);
    if (imgSize.x > 10 && imgSize.y > 10) {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // ----- Header bar (28px, drawn FIRST - the image starts below it) -----
        // Opaque strip so the title + mode buttons stay readable over the
        // bright sky of the FBO texture below.
        dl->AddRectFilled(
            ImVec2(winPos.x, winPos.y),
            ImVec2(winPos.x + winSize.x, winPos.y + headerH),
            EditorTheme::ToU32(theme.panelHeaderBg)
        );
        dl->AddRectFilled(
            ImVec2(winPos.x, winPos.y),
            ImVec2(winPos.x + 4.0f, winPos.y + headerH),
            EditorTheme::ToU32(theme.accent)
        );
        dl->AddLine(
            ImVec2(winPos.x, winPos.y + headerH),
            ImVec2(winPos.x + winSize.x, winPos.y + headerH),
            EditorTheme::ToU32(theme.border)
        );
        dl->AddText(ImVec2(winPos.x + 14.0f, winPos.y + 6.0f), EditorTheme::ToU32(theme.text), "Viewport");

        const char* modeLabels[] = { "Lit", "Wire", "Normals", "Unlit" };
        const int renderMode = editor.renderMode();
        const ImVec2& mousePos = ImGui::GetIO().MousePos;
        for (int i = 0; i < 4; i++) {
            ImVec2 btnSize(50, 20);
            ImVec2 btnPos(winPos.x + winSize.x - (4 - i) * (btnSize.x + 2) - 8, winPos.y + 4);
            bool isActive = (i == renderMode);
            dl->AddRectFilled(btnPos, ImVec2(btnPos.x + btnSize.x, btnPos.y + btnSize.y),
                              isActive ? EditorTheme::ToU32(theme.accent) : EditorTheme::ToU32(theme.frameBg));
            dl->AddRect(btnPos, ImVec2(btnPos.x + btnSize.x, btnPos.y + btnSize.y),
                        EditorTheme::ToU32(theme.border));
            dl->AddText(ImVec2(btnPos.x + 8, btnPos.y + 3),
                        isActive ? EditorTheme::ToU32(theme.text) : EditorTheme::ToU32(theme.textDim),
                            modeLabels[i]);

            if (mousePos.x >= btnPos.x && mousePos.x <= btnPos.x + btnSize.x &&
                mousePos.y >= btnPos.y && mousePos.y <= btnPos.y + btnSize.y &&
                ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) {
                editor.setRenderMode(i);
                // Wire mode: enable polygon wireframe rendering. This syncs
                // to the RenderPipeline which applies glPolygonMode in the
                // next frame's renderScene().
                Render::RenderPipeline::getInstance().setWireframeMode(i == 1);
            }
        }

        // The 3D canvas always starts below the header - even in swapchain-
        // backed mode the rect must be tracked (hover detection / camera
        // input map to it). WindowPadding is 0, so the canvas origin is the
        // window's content origin + headerH. (SetCursorPos is NOT used to
        // reach it here - it would assert when no ImGui item follows to grow
        // the window boundaries, which is the swapchain-backed case.)
        const ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
        const ImVec2 canvasMin(winPos.x + contentMin.x, winPos.y + contentMin.y + headerH);
        ImVec2 imgScreenMin = canvasMin;
        ImVec2 imgScreenMax(imgScreenMin.x + imgSize.x, imgScreenMin.y + imgSize.y);

        // Expose the canvas rect (window coords) for gizmo/picking math.
        g_viewportContentRect = ImRect(imgScreenMin, imgScreenMax);

        if (viewportTexture != 0) {
            // FBO image fills the panel BELOW the header - move the cursor
            // past the header so ImGui::Image lands exactly where the scene
            // is visible (the image item grows the window to fit).
            ImGui::SetCursorPos(ImVec2(0.0f, headerH));
            ImGui::Image((void*)(intptr_t)viewportTexture, imgSize, ImVec2(0, 1), ImVec2(1, 0));

            // Subtle accent frame around the 3D canvas so the viewport reads as
            // the editor's focal surface (drawn just inside the panel edge).
            dl->AddRect(imgScreenMin, imgScreenMax,
                        EditorTheme::ToU32(EditorTheme::WithAlpha(theme.accent, 0.45f)),
                        0.0f, ImDrawFlags_None, 1.0f);
        } else if (!swapchainBacked) {
            // Safety guard 3: missing viewport texture -> draw placeholder text
            // instead of calling ImGui::Image with a null texture handle.
            ImVec2 center(
                winPos.x + imgSize.x * 0.5f,
                winPos.y + headerH + imgSize.y * 0.5f
            );
            const char* msg = "Viewport unavailable";
            ImVec2 textSize = ImGui::CalcTextSize(msg);
            dl->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f),
                        IM_COL32(180, 180, 180, 255), msg);
        }
        // else: swapchain-backed - the RHI scene renders INTO the swapchain
        // behind the UI, so the panel stays transparent and the scene shows
        // through below the header strip.

        // ----- Viewport toolbar (Unreal-style, top-left of the canvas) -----
            // Snap / grid / wireframe / focus-selection as compact icon buttons.
            // Anchored to the image rect (which starts below the header), so
            // the buttons always render on top of the scene.
            {
                ImFont* fa = FaImGui::GetFont();
                const float btnSize = 26.0f;
                const float gap = 4.0f;
                float bx = imgScreenMin.x + 6.0f;
                const float by = imgScreenMin.y + 6.0f;
                const ImVec2& mouse = ImGui::GetIO().MousePos;

                auto ToolButton = [&](const char* cp, bool active, const char* tip) -> bool {
                    const ImRect r(ImVec2(bx, by), ImVec2(bx + btnSize, by + btnSize));
                    const bool hover = r.Contains(mouse);
                    const bool clicked = hover && ImGui::IsMouseClicked(0) &&
                                         ImGui::IsWindowHovered();
                    const ImU32 bg = active ? EditorTheme::ToU32(theme.accent)
                          : hover    ? EditorTheme::ToU32(EditorTheme::WithAlpha(theme.panelHeaderBg, 0.92f))
                          :            EditorTheme::ToU32(EditorTheme::WithAlpha(theme.panelBg, 0.72f));
                    dl->AddRectFilled(r.Min, r.Max, bg, 4.0f);
                    dl->AddRect(r.Min, r.Max,
                                EditorTheme::ToU32(EditorTheme::WithAlpha(theme.border, 0.6f)), 4.0f);
                    if (fa) {
                        const float iSize = 15.0f;
                        dl->AddText(fa, iSize,
                                    ImVec2(r.Min.x + (btnSize - iSize) * 0.5f,
                                           r.Min.y + (btnSize - iSize) * 0.5f),
                                    EditorTheme::ToU32(active ? theme.text : theme.textDim), cp);
                    }
                    if (hover) ImGui::SetTooltip("%s", tip);
                    bx += btnSize + gap;
                    return clicked;
                };

                if (ToolButton(ICON_FA_MAGNET, editor.uiState.snapEnabled, "Snap to grid (World Settings)")) {
                    editor.uiState.snapEnabled = !editor.uiState.snapEnabled;
                }
                if (ToolButton(ICON_FA_TABLE_CELLS, editor.showGrid(), "Toggle grid")) {
                    editor.setShowGrid(!editor.showGrid());
                }
                if (ToolButton(ICON_FA_CUBE, editor.showWireframe() != 0, "Toggle wireframe")) {
                    editor.setShowWireframe(editor.showWireframe() ? 0 : 1);
                }
                if (ToolButton(ICON_FA_CROSSHAIRS, false, "Focus selection (free camera)")) {
                    const ecs::EntityID sel = EntityManager::ValidateOrClear(editor.selectedEntity());
                    flyCamera* cam = editor.camera();
                    if (sel != ecs::INVALID_ENTITY_ID && cam) {
                        if (const auto* ft = editor.world().getComponentArchetype<ecs::TransformComponent>(
                                ecs::Entity{sel})) {
                            const float dist = 3.0f + 2.0f * glm::length(ft->scale);
                            cam->Target = ft->position;
                            cam->Position = ft->position + glm::vec3(0.0f, dist * 0.6f, dist);
                        }
                    }
                }
            }

            // ----- Overlay: bottom-left camera/tool info -----
            char camBuf[160];
            // Safety guard 4: camera may be null (editor not initialized yet).
            flyCamera* camera = editor.camera();
            GizmoRenderer::GizmoType gizmoType = editor.gizmoType();
            GizmoRenderer::SpaceType spaceType = editor.spaceType();
            bool showGrid = editor.showGrid();
            bool showGizmo = editor.showGizmo();
            // Show the ACTIVE view camera: the engine passes the current
            // camera position/target (fly cam in Free, follow cam in modes
            // 1-4). Fall back to the fly camera when not provided.
            if (cameraPos && cameraTarget) {
                float dist = glm::length(*cameraTarget - *cameraPos);
                snprintf(camBuf, sizeof(camBuf),
                         "Pos (%.1f, %.1f, %.1f)  Dist %.1f",
                         cameraPos->x, cameraPos->y, cameraPos->z, dist);
            } else if (camera) {
                glm::vec3 delta = camera->Target - camera->Position;
                float dist = glm::length(delta);
                snprintf(camBuf, sizeof(camBuf),
                         "Pos (%.1f, %.1f, %.1f)  Dist %.1f",
                         camera->Position.x, camera->Position.y, camera->Position.z, dist);
            } else {
                snprintf(camBuf, sizeof(camBuf), "Camera: <none>");
            }
            const char* giz = (gizmoType == GizmoRenderer::GizmoType::Translate) ? "Move" :
                           (gizmoType == GizmoRenderer::GizmoType::Rotate) ? "Rotate" : "Scale";
            const char* sp = (spaceType == GizmoRenderer::SpaceType::World) ? "World" : "Local";
            char toolBuf[64];
            snprintf(toolBuf, sizeof(toolBuf), "Tool: %s  Space: %s", giz, sp);

            // Position: bottom-left of FBO image
            const float overlayH = camModeName ? 56.0f : 36.0f;
            ImVec2 overlayBL(imgScreenMin.x + 8.0f, imgScreenMax.y - overlayH);
            dl->AddRectFilled(ImVec2(overlayBL.x - 4, overlayBL.y - 2),
                              ImVec2(overlayBL.x + 240, overlayBL.y + overlayH),
                              EditorTheme::ToU32(EditorTheme::WithAlpha(theme.bg, 0.82f)));
            dl->AddText(overlayBL, EditorTheme::ToU32(theme.text), camBuf);
            dl->AddText(ImVec2(overlayBL.x, overlayBL.y + 16), EditorTheme::ToU32(theme.textDim), toolBuf);
            if (camModeName) {
                dl->AddText(ImVec2(overlayBL.x, overlayBL.y + 32),
                            EditorTheme::ToU32(theme.accentBright), camModeName);
            }

            // ----- Overlay: top-right FPS / draw calls -----
            float fps = io->Framerate;
            char fpsBuf[32];
            snprintf(fpsBuf, sizeof(fpsBuf), "%.0f FPS", fps);
            char resBuf[48];
            snprintf(resBuf, sizeof(resBuf), "%dx%d", (int)imgSize.x, (int)imgSize.y);

            ImVec2 overlayTR(imgScreenMax.x - 110.0f, imgScreenMin.y + 6.0f);
            dl->AddRectFilled(ImVec2(overlayTR.x - 4, overlayTR.y - 2),
                              ImVec2(overlayTR.x + 110, overlayTR.y + 36),
                              EditorTheme::ToU32(EditorTheme::WithAlpha(theme.bg, 0.82f)));
            // Color FPS based on value
            ImU32 fpsCol = fps >= 60.0f ? EditorTheme::ToU32(theme.accentGreen) :
                           fps >= 30.0f ? EditorTheme::ToU32(theme.accentWarn) :
                                           EditorTheme::ToU32(theme.accentDanger);
            dl->AddText(overlayTR, fpsCol, fpsBuf);
            dl->AddText(ImVec2(overlayTR.x, overlayTR.y + 16), EditorTheme::ToU32(theme.textDim), resBuf);

            // ----- Center crosshair (subtle) -----
            ImVec2 crosshair(
                (imgScreenMin.x + imgScreenMax.x) * 0.5f,
                (imgScreenMin.y + imgScreenMax.y) * 0.5f
            );
            dl->AddLine(ImVec2(crosshair.x - 6, crosshair.y), ImVec2(crosshair.x + 6, crosshair.y),
                        IM_COL32(255, 255, 255, 100), 1.0f);
            dl->AddLine(ImVec2(crosshair.x, crosshair.y - 6), ImVec2(crosshair.x, crosshair.y + 6),
                        IM_COL32(255, 255, 255, 100), 1.0f);

            // ----- Grid (always at world origin) -----
            if (showGrid && camera) {
                GridRenderer::Draw(view, projection, ShaderManager::GetMainShaderProgram());
            }

            // ----- Keyboard shortcuts: W/E/R → gizmo type (when viewport hovered, not dragging) -----
            if (ImGui::IsWindowHovered() && !GizmoRenderer::IsDragging() &&
                gizmoType != GizmoRenderer::GizmoType::None) {
                // Only switch when not already on the target (avoids re-triggering)
                if (ImGui::IsKeyPressed(ImGuiKey_W, false) &&
                    gizmoType != GizmoRenderer::GizmoType::Translate) {
                    editor.setGizmoType(GizmoRenderer::GizmoType::Translate);
                    GizmoRenderer::SetGizmoType(GizmoRenderer::GizmoType::Translate);
                } else if (ImGui::IsKeyPressed(ImGuiKey_E, false) &&
                    gizmoType != GizmoRenderer::GizmoType::Rotate) {
                    editor.setGizmoType(GizmoRenderer::GizmoType::Rotate);
                    GizmoRenderer::SetGizmoType(GizmoRenderer::GizmoType::Rotate);
                } else if (ImGui::IsKeyPressed(ImGuiKey_R, false) &&
                    gizmoType != GizmoRenderer::GizmoType::Scale) {
                    editor.setGizmoType(GizmoRenderer::GizmoType::Scale);
                    GizmoRenderer::SetGizmoType(GizmoRenderer::GizmoType::Scale);
                }
            }

            // ----- Selection gizmo + interaction -----
            // Gizmo appears on the selected ECS entity, or — when no entity
            // is selected — on the loaded character (bot) for translate/rotate/scale.
            // Safety guard 5: validate the cached selection before dereferencing
            // its transform. If the selected entity no longer exists, clear it.
            ecs::EntityID selected = EntityManager::ValidateOrClear(editor.selectedEntity());
            AnimatedCharacter* ch = nullptr;
            // Fall back to the bot character when nothing in the ECS world is selected
            if (selected == ecs::INVALID_ENTITY_ID) {
                ch = editor.character();
                if (ch && !ch->ready()) ch = nullptr;  // don't use a half-loaded character
            }
            if (showGizmo && gizmoType != GizmoRenderer::GizmoType::None &&
                (selected != ecs::INVALID_ENTITY_ID || ch != nullptr) && camera) {
                ecs::World& w = editor.world();
                glm::vec3 pos(0.0f);
                glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
                const ecs::TransformComponent* t = nullptr;

                // --- Determine the gizmo target (ECS entity or character) ---
                if (ch && selected == ecs::INVALID_ENTITY_ID) {
                    pos = ch->position;
                    // Character heading is a Y-axis yaw — represent as local-space rot
                    if (spaceType == GizmoRenderer::SpaceType::Local) {
                        rot = glm::angleAxis(ch->heading, glm::vec3(0.0f, 1.0f, 0.0f));
                    }
                } else {
                    t = w.getComponent<ecs::TransformComponent>(ecs::Entity{selected});
                    if (t) {
                        pos = t->position;
                        rot = (spaceType == GizmoRenderer::SpaceType::World)
                                   ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
                                   : glm::quat(t->rotation);
                    }
                }

                // If we ended up with no valid target, skip gizmo rendering
                if (t || ch) {
                    float size = 1.0f;
                    GizmoRenderer::Draw(pos, size, rot, view, projection,
                                        ShaderManager::GetGizmoShaderProgram(),
                                        gizmoType);

                    // ---- Gizmo interaction: hit-test / drag / snap ----
                    // Handle picking + dragging the gizmo axes. The delta from the
                    // gizmo is applied directly to the target's transform, respecting
                    // snap settings.
                    const ImVec2& mouse = io->MousePos;

                    // Viewport image rect in window coords (captured above).
                    const bool mouseInViewport = g_viewportContentRect.Contains(mouse);

                    // Convert to viewport-local coords for ScreenToWorldRay
                    // (which divides by vpW/vpH).
                    const float vpX = (float)imgScreenMin.x;
                    const float vpY = (float)imgScreenMin.y;
                    const float vpW = (float)imgSize.x;
                    const float vpH = (float)imgSize.y;
                    const float mouseRelX = mouse.x - vpX;
                    const float mouseRelY = mouse.y - vpY;

                    // --- Mouse press: start drag if we hit an axis ---
                    if (mouseInViewport && ImGui::IsMouseClicked(0)) {
                        const GizmoRenderer::GizmoAxis hit = GizmoRenderer::HitTest(
                            pos, 1.0f, rot, view, projection,
                            (int)vpX, (int)vpY, (int)vpW, (int)vpH,
                            mouse.x, mouse.y);
                        if (hit != GizmoRenderer::GizmoAxis::None) {
                            GizmoRenderer::BeginDrag(hit, pos, rot,
                                                     mouseRelX, mouseRelY);
                        }
                    }

                    // --- While dragging: accumulate delta + apply with snap ---
                    if (GizmoRenderer::IsDragging()) {
                        if (ch && selected == ecs::INVALID_ENTITY_ID) {
                            // --- Character (bot) transform ---
                            if (gizmoType == GizmoRenderer::GizmoType::Translate) {
                                glm::vec3 delta = GizmoRenderer::UpdateDrag(
                                    mouseRelX, mouseRelY, view, projection, (int)vpW, (int)vpH);
                                if (editor.uiState.snapEnabled && editor.uiState.snapTranslate > 0.0f) {
                                    delta = glm::round(delta / editor.uiState.snapTranslate) *
                                            editor.uiState.snapTranslate;
                                }
                                ch->position += delta;
                            } else if (gizmoType == GizmoRenderer::GizmoType::Rotate) {
                                float ang = GizmoRenderer::UpdateDragRotate(
                                    mouseRelX, mouseRelY, view, projection, (int)vpW, (int)vpH);
                                if (editor.uiState.snapEnabled && editor.uiState.snapRotate > 0.0f) {
                                    ang = glm::round(ang / editor.uiState.snapRotate) *
                                            editor.uiState.snapRotate;
                                }
                                // Character rotates around Y — add to heading (radians)
                                ch->heading += glm::radians(ang);
                            } else if (gizmoType == GizmoRenderer::GizmoType::Scale) {
                                float factor = GizmoRenderer::UpdateDragScale(
                                    mouseRelX, mouseRelY, view, projection, (int)vpW, (int)vpH);
                                if (editor.uiState.snapEnabled && editor.uiState.snapScale > 0.0f) {
                                    factor = glm::round(factor / editor.uiState.snapScale) *
                                            editor.uiState.snapScale;
                                }
                                ch->scale = std::max(0.01f, ch->scale * factor);
                            }
                        } else {
                            // --- ECS entity transform ---
                            ecs::TransformComponent* tc =
                                w.getComponent<ecs::TransformComponent>(ecs::Entity{selected});
                            if (tc) {
                                if (gizmoType == GizmoRenderer::GizmoType::Translate) {
                                    glm::vec3 delta = GizmoRenderer::UpdateDrag(
                                        mouseRelX, mouseRelY, view, projection, (int)vpW, (int)vpH);
                                    if (editor.uiState.snapEnabled && editor.uiState.snapTranslate > 0.0f) {
                                        delta = glm::round(delta / editor.uiState.snapTranslate) *
                                                editor.uiState.snapTranslate;
                                    }
                                    tc->position += delta;
                                } else if (gizmoType == GizmoRenderer::GizmoType::Rotate) {
                                    float ang = GizmoRenderer::UpdateDragRotate(
                                        mouseRelX, mouseRelY, view, projection, (int)vpW, (int)vpH);
                                    if (editor.uiState.snapEnabled && editor.uiState.snapRotate > 0.0f) {
                                        ang = glm::round(ang / editor.uiState.snapRotate) *
                                                editor.uiState.snapRotate;
                                    }
                                    tc->rotation = glm::angleAxis(glm::radians(ang),
                                                GizmoRenderer::GetDragAxisWorld()) * tc->rotation;
                                } else if (gizmoType == GizmoRenderer::GizmoType::Scale) {
                                    float factor = GizmoRenderer::UpdateDragScale(
                                        mouseRelX, mouseRelY, view, projection, (int)vpW, (int)vpH);
                                    if (editor.uiState.snapEnabled && editor.uiState.snapScale > 0.0f) {
                                        factor = glm::round(factor / editor.uiState.snapScale) *
                                                editor.uiState.snapScale;
                                    }
                                    tc->scale *= factor;
                                }
                            }
                        }

                        // --- Mouse release: end drag ---
                        if (ImGui::IsMouseReleased(0)) {
                            GizmoRenderer::EndDrag();
                        }
                    }
                }
            }

        (void)window;
    }
    UI::EndFixedPanel();
}

// ============================================================================
// Status bar - fixed bottom strip
// ============================================================================
void RenderStatusBar(size_t entityCount, ecs::EntityID selected, float fps,
                   bool isPlaying, bool wasPlaying, int windowW, int windowH) {
    (void)windowW;
    (void)windowH;

    // Safety guard: no ImGui context -> nothing to render into.
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    ImVec2 displaySize = GetDisplaySize();
    ImVec2 pos(0, displaySize.y - STATUS_BAR_HEIGHT);
    ImVec2 size(displaySize.x, STATUS_BAR_HEIGHT);

    const auto& theme = EditorTheme::Get();
    // BeginFixedPanel creates a real window, so the window background (not the
    // child background) is what paints the status strip.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::ToU32(theme.statusBarBg));

    if (!UI::BeginFixedPanel("StatusBar", pos, size)) {
        UI::EndFixedPanel();
        ImGui::PopStyleColor();
        return;
    }

    ImGui::TextColored(theme.accentGreen, "%.0f FPS", fps);
    ImGui::SameLine();
    ImGui::TextColored(theme.textDim, " | %zu entities", entityCount);

    // Geo status (if geo API available)
    ImGui::SameLine();
#ifndef DISABLE_GEOSPATIAL
    UI::RenderGeoStatusBar(g_editor.geospatialSystem().getGeoAPI());
#else
    // geo status bar disabled
#endif

    // Right side: selected entity (by name) + a colored play-state indicator.
    // Everything on one line - the old code started a second row that got
    // clipped by the 24px strip.
    ImGui::SameLine(ImGui::GetWindowWidth() - 240);
    if (selected != ecs::INVALID_ENTITY_ID) {
        const std::string selName = EntityManager::GetSafeEntityName(selected);
        ImGui::TextColored(theme.accent, "%s", selName.c_str());
    } else {
        ImGui::TextColored(theme.textDisabled, "No selection");
    }

    ImGui::SameLine();
    if (isPlaying)      ImGui::TextColored(theme.accentDanger, ICON_FA_CIRCLE_PLAY " Playing");
    else if (wasPlaying) ImGui::TextColored(theme.accentWarn,  ICON_FA_CIRCLE_PAUSE " Paused");
    else                 ImGui::TextColored(theme.textDisabled, ICON_FA_CIRCLE_STOP " Stopped");

    UI::EndFixedPanel();
    ImGui::PopStyleColor();
}

void RenderAboutDialog(bool& showAbout) {
    if (!showAbout) return;
    ImGui::OpenPopup("About");
    if (ImGui::BeginPopupModal("About", &showAbout, ImGuiWindowFlags_AlwaysAutoResize)) {
        EditorTheme::SectionTitle("RTT Engine Editor");
        ImGui::Separator();
        ImGui::Text("Version: 1.0.0");
        ImGui::Text("A professional 3D game engine editor");
        ImGui::Dummy(ImVec2(0, 10));
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            showAbout = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

// One drag row for a transform axis. The X/Y/Z labels reuse the viewport
// gizmo axis colors (X=red, Y=green, Z=blue) so the Details panel and the
// 3D gizmo read as one coherent language.
static void TransformAxisRow(const char* axis, float& value, float speed,
                             float minV, float maxV, ImU32 axisColor) {
    ImGui::PushStyleColor(ImGuiCol_Text, axisColor);
    ImGui::Text("%s", axis);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat("##v", &value, speed, minV, maxV);
}

void RenderTransformSection(ecs::TransformComponent* t) {
    if (!t) return;

    const auto& theme = EditorTheme::Get();
    // Gizmo axis colors (X/Y/Z).
    const ImU32 colX = EditorTheme::ToU32(ImVec4(0.85f, 0.30f, 0.28f, 1.0f));
    const ImU32 colY = EditorTheme::ToU32(ImVec4(0.30f, 0.80f, 0.34f, 1.0f));
    const ImU32 colZ = EditorTheme::ToU32(ImVec4(0.35f, 0.55f, 0.95f, 1.0f));

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Transform");

        ImGui::TextColored(theme.textDim, "Location");
        glm::vec3 p = t->position;
        TransformAxisRow("X", p.x, 0.1f, -100000.0f, 100000.0f, colX);
        TransformAxisRow("Y", p.y, 0.1f, -100000.0f, 100000.0f, colY);
        TransformAxisRow("Z", p.z, 0.1f, -100000.0f, 100000.0f, colZ);
        if (p != t->position) t->position = p;

        ImGui::TextColored(theme.textDim, "Rotation");
        glm::vec3 r = glm::degrees(glm::eulerAngles(t->rotation));
        const glm::vec3 rBefore = r;
        TransformAxisRow("X", r.x, 0.5f, -360.0f, 360.0f, colX);
        TransformAxisRow("Y", r.y, 0.5f, -360.0f, 360.0f, colY);
        TransformAxisRow("Z", r.z, 0.5f, -360.0f, 360.0f, colZ);
        if (r != rBefore) t->setEulerAngles(glm::radians(r));

        ImGui::TextColored(theme.textDim, "Scale");
        glm::vec3 s = t->scale;
        const glm::vec3 sBefore = s;
        TransformAxisRow("X", s.x, 0.01f, 0.001f, 100000.0f, colX);
        TransformAxisRow("Y", s.y, 0.01f, 0.001f, 100000.0f, colY);
        TransformAxisRow("Z", s.z, 0.01f, 0.001f, 100000.0f, colZ);
        if (s != sBefore) t->scale = s;

        ImGui::PopID();
    }
}

void RenderMeshSection(ecs::MeshComponent* m) {
    if (!m) return;
    if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Mesh");
        ImVec4 c = ImVec4(m->color.r, m->color.g, m->color.b, 1.0f);
        if (ImGui::ColorEdit3("Albedo", &c.x)) m->color = glm::vec3(c.x, c.y, c.z);
        ImGui::Checkbox("Visible", &m->visible);
        ImGui::SliderFloat("Metallic", &m->metallic, 0.0f, 1.0f);
        ImGui::SliderFloat("Roughness", &m->roughness, 0.0f, 1.0f);
        ImGui::PopID();
    }
}

// ============================================================================
// Play-mode debug section - rendered INSIDE the Details panel (no floating
// viewport window, so the 3D view stays clear and the character visible).
// ============================================================================
void RenderPlayModeDebug(const Editor::PlayModeController& ctrl, const char* camMode) {
    // Safety guard: no ImGui context -> nothing to render into.
    if (ImGui::GetCurrentContext() == nullptr) return;
    if (!ctrl.isLoaded()) return;

    const AnimatedCharacter& cc = ctrl.character();
    const auto& theme = EditorTheme::Get();

    if (ImGui::CollapsingHeader("Play Mode", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("PlayModeDebug");

        const float speed = cc.currentSpeed();
        const float speedPct = glm::clamp(speed / 6.0f, 0.0f, 1.0f);

        ImGui::Text("State:  %s", AnimationStateToString(cc.state()).c_str());
        ImGui::Text("Speed:  %.2f m/s", speed);
        ImGui::ProgressBar(speedPct, ImVec2(-1.0f, 0.0f), "");
        ImGui::Text("Grounded: %s", cc.grounded ? "yes" : "NO");
        ImGui::Text("Crouch:  %s", cc.crouchState() ? "yes" : "no");
        ImGui::Text("Active clip: %s", cc.activeClipName().c_str());
        ImGui::Text("Animator time: %.2fs", cc.animatorTime());
        if (camMode) {
            ImGui::Separator();
            ImGui::TextColored(theme.accentBright, "Camera: %s", camMode);
            ImGui::TextDisabled("1-4 switch · right-drag orbit · scroll zoom");
        }

        ImGui::Separator();
        if (cc.isMotionMatchingActive()) {
            const AnimatedCharacter::PoseDiag diag = cc.debugPoseDiag();
            ImGui::TextColored(theme.accentWarn, "Motion Matching");
            ImGui::Text("Matcher clip: %s", diag.matcherClip.c_str());
            ImGui::Text("Dominant: %s (%.0f%%)", diag.dominantRawName.c_str(), diag.dominantWeight * 100.0f);
            ImGui::Text("Dominant time: %.2fs", diag.dominantTime);
            ImGui::TextColored(
                diag.dominantMatchesMatcher ? theme.accentGreen : theme.accentDanger,
                "Pose sync: %s", diag.dominantMatchesMatcher ? "MATCHED" : "MISMATCH");
            ImGui::Text("Foot IK: L=%s R=%s",
                        diag.leftLocked ? "LOCK" : "free",
                        diag.rightLocked ? "LOCK" : "free");

            // --- Extended IK diagnostics ---
            if (ImGui::TreeNode("Leg Diagnostics")) {
                // Left leg
                ImVec4 leftCol  = (diag.leftKneeDeg  > 170.0f) ? theme.accentGreen :
                                   (diag.leftKneeDeg  > 140.0f) ? theme.accentWarn :
                                                                  theme.accentDanger;
                ImVec4 rightCol = (diag.rightKneeDeg > 170.0f) ? theme.accentGreen :
                                   (diag.rightKneeDeg > 140.0f) ? theme.accentWarn :
                                                                  theme.accentDanger;
                ImGui::TextColored(leftCol,  "L knee: %.1f°  (lock %.0f%%)", diag.leftKneeDeg,  diag.leftLockWeight  * 100.0f);
                ImGui::SameLine(200);
                ImGui::TextColored(rightCol, "R knee: %.1f°  (lock %.0f%%)", diag.rightKneeDeg, diag.rightLockWeight * 100.0f);
                ImGui::Text("L ankle off: %.1f cm   R ankle off: %.1f cm",
                            diag.leftAnkleOffY_m  * 100.0f,
                            diag.rightAnkleOffY_m * 100.0f);
                ImGui::Text("L reach: %.3f m   R reach: %.3f m",
                            diag.leftLegReach, diag.rightLegReach);
                ImGui::Text("Pelvis drop: %.3f m", diag.pelvisDropY);
                ImGui::TreePop();
            }
                // --- Log diagnostics to stdout for headless capture ---
            printf("[KneeDiag] L=%.0f°(%.0f%%) R=%.0f°(%.0f%%) ankleL=%.3f ankleR=%.3f "
                   "pelvisDrop=%.3f reachL=%.3f reachR=%.3f moving=%d clip=%s\n",
                   diag.leftKneeDeg, diag.leftLockWeight * 100.0f,
                   diag.rightKneeDeg, diag.rightLockWeight * 100.0f,
                   diag.leftAnkleOffY_m, diag.rightAnkleOffY_m,
                   diag.pelvisDropY, diag.leftLegReach, diag.rightLegReach,
                   diag.isMoving ? 1 : 0,
                   diag.matcherClip.c_str());
            fflush(stdout);
        } else {
            ImGui::TextColored(theme.textDim, "Motion Matching: off");
        }

        ImGui::PopID();
    }
}

} // namespace UI
