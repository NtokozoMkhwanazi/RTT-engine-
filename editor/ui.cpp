#include "ui.h"
#include "phosphor_imgui.h"
#include "phosphor_icons_codepoints.h"
#include "editor_state.h"
#include "entity_manager.h"
#include "console.h"
#include "scene_manager.h"
#include "model_loader.h"
#include "ecs/components/Components.h"
#include "cameraSystem/flyCamera.h"
#include "gizmo_renderer.h"
#include "grid_renderer.h"
#include "shader_manager.h"
#include "geo_config_panel.h"
#include "ui_helpers.h"
#include <GLFW/glfw3.h>
#include <iostream>
#include <algorithm>
#include <vector>
#include <cstring>

namespace UI {

// Fixed editor layout constants (Unreal/Unity-style docked layout)
constexpr float MENU_BAR_HEIGHT     = 25.0f;
constexpr float TOOLBAR_HEIGHT      = 38.0f;
constexpr float LEFT_PANEL_WIDTH    = 280.0f;
constexpr float RIGHT_PANEL_WIDTH   = 300.0f;
constexpr float BOTTOM_PANEL_HEIGHT = 180.0f;
constexpr float STATUS_BAR_HEIGHT   = 24.0f;

static ImVec2 GetDisplaySize() { return ImGui::GetIO().DisplaySize; }

// ============================================================================
// Menu bar
// ============================================================================
void RenderMenuBar(Editor::Editor& editor, const std::string& currentSceneFile) {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Scene", "Ctrl+N")) {
                editor.world().shutdown();
                editor.world().init();
                editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
                EditorConsole::Log("New scene created");
            }
            if (ImGui::MenuItem("Open Scene...", "Ctrl+O")) {
                if (SceneManager::LoadScene("scene.json", editor.world())) {
                    EditorConsole::Log("Scene loaded");
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S")) {
                SceneManager::SaveScene(currentSceneFile.empty() ? "scene.json" : currentSceneFile, editor.world());
            }
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                editor.setShouldClose(true);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            bool hasSel = (editor.selectedEntity() != ecs::INVALID_ENTITY_ID);
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, false)) {}
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, false)) {}
            ImGui::Separator();
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSel)) {
                EntityManager::DuplicateEntity(editor.selectedEntity());
            }
            if (ImGui::MenuItem("Delete", "Del", false, hasSel)) {
                EntityManager::DeleteEntity(editor.selectedEntity());
                editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            bool showGrid = editor.showGrid();
            bool showGizmo = editor.showGizmo();
            if (ImGui::MenuItem("Show Grid", nullptr, &showGrid)) editor.setShowGrid(showGrid);
            if (ImGui::MenuItem("Show Gizmo", nullptr, &showGizmo)) editor.setShowGizmo(showGizmo);
            ImGui::Separator();
            ImGui::MenuItem("Outliner", nullptr, &editor.uiState.showOutliner);
            ImGui::MenuItem("Details", nullptr, &editor.uiState.showDetails);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) {
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
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1), "Editor Preferences");
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
// Toolbar - Unreal Engine style top toolbar (fixed)
// ============================================================================
void RenderToolbar(Editor::Editor& editor) {
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    UI::BeginFixedPanel("Toolbar", ImVec2(0, MENU_BAR_HEIGHT), ImVec2(displaySize.x, TOOLBAR_HEIGHT));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));

    auto ToolbarButton = [&](PhosphorIcons::Icon icon, bool active, const char* tooltip, ImU32 activeCol = IM_COL32(60, 60, 60, 255)) {
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
        PhosphorImGui::ToolbarButton(icon, active, tooltip);
        if (active) ImGui::PopStyleColor();
    };

    // ----- File / Edit quick actions -----
    ToolbarButton(PhosphorIcons::Save, false, "Save Scene");
    if (ImGui::IsItemClicked()) SceneManager::SaveScene("scene.json", editor.world());
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(4, 0));
    ImGui::SameLine();

    // ----- Transform tools -----
    bool tActive = (editor.gizmoType() == GizmoRenderer::GizmoType::Translate);
    ToolbarButton(PhosphorIcons::ArrowsOut, tActive, "Translate (W)");
    if (ImGui::IsItemClicked()) editor.setGizmoType(GizmoRenderer::GizmoType::Translate);
    ImGui::SameLine();

    bool rActive = (editor.gizmoType() == GizmoRenderer::GizmoType::Rotate);
    ToolbarButton(PhosphorIcons::ArrowCounterClockwise, rActive, "Rotate (E)");
    if (ImGui::IsItemClicked()) editor.setGizmoType(GizmoRenderer::GizmoType::Rotate);
    ImGui::SameLine();

    bool sActive = (editor.gizmoType() == GizmoRenderer::GizmoType::Scale);
    ToolbarButton(PhosphorIcons::ArrowsIn, sActive, "Scale (R)");
    if (ImGui::IsItemClicked()) editor.setGizmoType(GizmoRenderer::GizmoType::Scale);
    ImGui::SameLine();

    ImGui::Separator();
    ImGui::SameLine();

    // ----- Coordinate space -----
    bool worldSpace = (editor.spaceType() == GizmoRenderer::SpaceType::World);
    ToolbarButton(PhosphorIcons::GridFour, worldSpace, worldSpace ? "World Space" : "Local Space");
    if (ImGui::IsItemClicked()) editor.setSpaceType(worldSpace ? GizmoRenderer::SpaceType::Local : GizmoRenderer::SpaceType::World);
    ImGui::SameLine();

    ImGui::Separator();
    ImGui::SameLine();

    // ----- Viewport options -----
    bool wf = editor.showWireframe() != 0;
    ToolbarButton(PhosphorIcons::Cube, wf, "Wireframe");
    if (ImGui::IsItemClicked()) editor.setShowWireframe(wf ? 0 : 1);
    ImGui::SameLine();

    bool grid = editor.showGrid();
    ToolbarButton(PhosphorIcons::GridFour, grid, "Grid");
    if (ImGui::IsItemClicked()) editor.setShowGrid(!grid);
    ImGui::SameLine();

    // ----- Play controls (right side) -----
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120);
    bool playing = editor.isPlaying();
    ImGui::PushStyleColor(ImGuiCol_Button, playing ? IM_COL32(200, 60, 60, 255) : IM_COL32(60, 140, 60, 255));
    PhosphorImGui::ToolbarButton(PhosphorIcons::Play, false, playing ? "Stop (Esc)" : "Play (F5)");
    if (ImGui::IsItemClicked()) editor.setPlaying(!playing);
    ImGui::PopStyleColor();
    ImGui::SameLine();

    bool paused = editor.wasPlaying() && !playing;
    ImGui::PushStyleColor(ImGuiCol_Button, paused ? IM_COL32(220, 180, 60, 255) : IM_COL32(80, 80, 80, 255));
    PhosphorImGui::ToolbarButton(PhosphorIcons::Pause, false, "Pause");
    if (ImGui::IsItemClicked()) editor.setWasPlaying(playing ? !editor.wasPlaying() : false);
    ImGui::PopStyleColor();
    ImGui::SameLine();

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

    const char* tabs[] = { "Outliner", "Layers", "World" };
    const int tabCount = IM_ARRAYSIZE(tabs);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float tabW = (avail.x - (tabCount - 1) * 4) / tabCount;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
    for (int i = 0; i < tabCount; ++i) {
        bool active = (panelConfig.activeTabIndex == i);
        ImGui::PushStyleColor(ImGuiCol_Button, active ? IM_COL32(60, 60, 60, 255) : IM_COL32(40, 40, 40, 255));
        if (ImGui::Button(tabs[i], ImVec2(tabW, 24))) panelConfig.activeTabIndex = i;
        ImGui::PopStyleColor();
        if (i < tabCount - 1) ImGui::SameLine();
    }
    ImGui::PopStyleVar();

    ImGui::Separator();

    ecs::EntityID selected = editor.selectedEntity();
    switch (panelConfig.activeTabIndex) {
        case 0:
            RenderOutlinerPanel(selected, editor.entityCache(), editor.uiState.searchBuffer);
            break;
        case 1:
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "Layers (placeholder)");
            break;
        case 2:
            RenderWorldSettingsPanel(editor);
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
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1), "World Settings");
    ImGui::Separator();

    bool showGrid = editor.showGrid();
    if (UI::CheckboxSafe("Show Grid", &showGrid)) editor.setShowGrid(showGrid);

    bool showGizmo = editor.showGizmo();
    if (UI::CheckboxSafe("Show Gizmo", &showGizmo)) editor.setShowGizmo(showGizmo);

    int wire = editor.showWireframe();
    bool wireframe = (wire != 0);
    if (UI::CheckboxSafe("Wireframe", &wireframe)) editor.setShowWireframe(wireframe ? 1 : 0);

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Grid");
    static float gridSpacing = 1.0f;
    static int gridSize = 20;
    ImGui::DragFloat("Spacing", &gridSpacing, 0.1f, 0.1f, 10.0f);
    ImGui::DragInt("Size", &gridSize, 1, 2, 100);
}

// ============================================================================
// Right panel - fixed Unreal-style Details panel
// ============================================================================
void RenderRightPanel(Editor::Editor& editor) {
    if (!editor.uiState.showDetails) return;

    ImVec2 displaySize = GetDisplaySize();
    ImVec2 pos(displaySize.x - RIGHT_PANEL_WIDTH, MENU_BAR_HEIGHT + TOOLBAR_HEIGHT);
    ImVec2 size(RIGHT_PANEL_WIDTH, displaySize.y - MENU_BAR_HEIGHT - TOOLBAR_HEIGHT - STATUS_BAR_HEIGHT);

    if (!UI::BeginFixedPanel("Details", pos, size)) {
        UI::EndFixedPanel();
        return;
    }

    ecs::EntityID selected = editor.selectedEntity();
    selected = EntityManager::ValidateOrClear(selected);

    if (selected == ecs::INVALID_ENTITY_ID) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "Select an actor to edit details");
        UI::EndFixedPanel();
        return;
    }

    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1), "Actor %u", selected);
    ImGui::Separator();

    // Search/filter box
    static char detailsFilter[64] = "";
    UI::InputTextSafe("Search Details", detailsFilter, IM_ARRAYSIZE(detailsFilter));
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

    // Component tags
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Components");
    bool hasTrans = (t != nullptr);
    bool hasMesh = (m != nullptr);
    bool hasModel = (model != nullptr && model->isValid());
    bool hasCam = world.hasComponent<ecs::CameraComponent>(ecs::Entity{selected});
    bool hasLight = world.hasComponent<ecs::LightComponent>(ecs::Entity{selected});
    bool hasGeo = (geo != nullptr);

    ImGui::TextColored(hasTrans ? ImVec4(0.4f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "Transform");
    ImGui::SameLine(90);
    ImGui::TextColored(hasMesh ? ImVec4(0.4f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "Mesh");
    ImGui::SameLine(150);
    ImGui::TextColored(hasModel ? ImVec4(0.4f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "Model");
    ImGui::SameLine(210);
    ImGui::TextColored(hasCam ? ImVec4(0.4f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "Camera");
    ImGui::SameLine(270);
    ImGui::TextColored(hasLight ? ImVec4(0.4f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "Light");
    ImGui::SameLine(330);
    ImGui::TextColored(hasGeo ? ImVec4(0.4f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "Geo");

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
    // Search input with safe wrapper
    UI::InputTextSafe("##Search", const_cast<char*>(searchBuffer), 128);
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.5f, 1), "%zu entities", entityCache.size());
    ImGui::Separator();

    // Rebuild cache if dirty (not every frame)
    // Note: RebuildEntityCache needs the world, which should be passed from caller
    // For now, skip auto-rebuild here; caller should handle it
    if (entityCache.dirty) {
        // RebuildEntityCache will be called externally with proper world reference
    }

    // Render from cache - zero ECS queries
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 1));
    for (const auto& entry : entityCache.entries) {
        // Validate entity still exists
        if (!EntityManager::EntityExists(entry.id)) {
            if (selected == entry.id) {
                selected = ecs::INVALID_ENTITY_ID;
            }
            continue;
        }
        
        bool isSel = (selected == entry.id);

        // Use Phosphor icon for entity type
        PhosphorImGui::PushIconFont();
        const char* icon = entry.icon;
        if (PhosphorImGui::SmallIconButton(PhosphorIcons::Cube, "")) {
            // Click on icon - could expand/collapse in future
        }
        PhosphorImGui::PopIconFont();
        ImGui::SameLine();

        // Format: "Icon ID"
        char label[32];
        snprintf(label, sizeof(label), "%s %u", entry.icon, entry.id);

        if (isSel) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(70, 130, 180, 255));
        if (entry.hasGeo) {
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(40, 60, 40, 255));
        }

        if (UI::SelectableSafe(label, isSel, ImGuiSelectableFlags_SpanAllColumns)) {
            selected = entry.id;
        }

        if (entry.hasGeo) ImGui::PopStyleColor();
        if (isSel) ImGui::PopStyleColor();

        if (ImGui::BeginPopupContextItem(("ctx##" + std::to_string(entry.id)).c_str())) {
            if (ImGui::MenuItem("Delete")) {
                EntityManager::DeleteEntity(entry.id);
                entityCache.dirty = true;
            }
            ImGui::EndPopup();
        }
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

    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1), "Selected: Entity %u", selected);
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
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Components");
    ImGui::Separator();

    bool hasTrans = (t != nullptr);
    bool hasMesh = (m != nullptr);
    bool hasModel = (model != nullptr && model->isValid());
    bool hasCam = world.hasComponent<ecs::CameraComponent>(ecs::Entity{selected});
    bool hasLight = world.hasComponent<ecs::LightComponent>(ecs::Entity{selected});
    bool hasGeo = (geo != nullptr);

    ImGui::TextColored(hasTrans ? ImVec4(0.4f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "Transform");
    ImGui::SameLine(100);
    ImGui::TextColored(hasMesh ? ImVec4(0.4f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "Mesh");
    ImGui::SameLine(160);
    ImGui::TextColored(hasModel ? ImVec4(0.4f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "Model");
    ImGui::SameLine(220);
    ImGui::TextColored(hasCam ? ImVec4(0.4f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "Camera");
    ImGui::SameLine(280);
    ImGui::TextColored(hasLight ? ImVec4(0.4f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "Light");
    ImGui::SameLine(340);
    ImGui::TextColored(hasGeo ? ImVec4(0.4f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "Geo");
    
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

    const char* tabs[] = { "Content Browser", "Output Log", "Profiler" };
    const int tabCount = IM_ARRAYSIZE(tabs);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float tabW = (avail.x - (tabCount - 1) * 4) / tabCount;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
    for (int i = 0; i < tabCount; ++i) {
        bool active = (editor.uiState.bottomPanelTab == i);
        ImGui::PushStyleColor(ImGuiCol_Button, active ? IM_COL32(60, 60, 60, 255) : IM_COL32(40, 40, 40, 255));
        if (ImGui::Button(tabs[i], ImVec2(tabW, 24))) editor.uiState.bottomPanelTab = i;
        ImGui::PopStyleColor();
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
    static char pathBuffer[256] = "/Game/Assets";
    UI::InputTextSafe("Path", pathBuffer, IM_ARRAYSIZE(pathBuffer));
    ImGui::Separator();

    // Placeholder asset folders using available Phosphor icons
    struct AssetFolder { const char* name; PhosphorIcons::Icon icon; };
    static const AssetFolder folders[] = {
        { "Materials", PhosphorIcons::Pencil },
        { "Meshes", PhosphorIcons::Cube },
        { "Textures", PhosphorIcons::FileText },
        { "Blueprints", PhosphorIcons::FilePlus },
        { "Sounds", PhosphorIcons::Star },
    };

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
    for (size_t i = 0; i < IM_ARRAYSIZE(folders); ++i) {
        const auto& folder = folders[i];

        // Group each folder icon + label so ImGui sees proper bounds
        ImGui::BeginGroup();
        PhosphorImGui::PushIconFont();
        ImGui::Button(PhosphorImGui::GetCodepointSafe(folder.icon), ImVec2(64, 64));
        PhosphorImGui::PopIconFont();
        ImGui::Text("%s", folder.name);
        ImGui::EndGroup();

        if (i < IM_ARRAYSIZE(folders) - 1) {
            ImGui::SameLine();
        }
    }
    ImGui::PopStyleVar();
}

void RenderProfilerPanel(Editor::Editor& editor, float fps) {
    ImGui::Columns(2, "ProfilerStats", true);
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "FPS"); ImGui::NextColumn();
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "%.1f", fps); ImGui::NextColumn();

    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Frame Time"); ImGui::NextColumn();
    ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.9f, 1), "%.2f ms", 1000.0f / (fps > 0 ? fps : 60)); ImGui::NextColumn();

    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Entities"); ImGui::NextColumn();
    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1), "%zu", editor.world().getEntityCount()); ImGui::NextColumn();

    ImGui::Columns(1);

    flyCamera* camera = editor.camera();
    if (camera) {
        ImGui::Text("Camera Position: (%.1f, %.1f, %.1f)", camera->Position.x, camera->Position.y, camera->Position.z);
    }
}

void RenderConsolePanel() {
    const auto& msgs = EditorConsole::GetMessages();
    int n = std::min(20, (int)msgs.size());
    for (int i = std::max(0, (int)msgs.size() - n); i < (int)msgs.size(); i++) {
        const auto& m = msgs[i];
        ImVec4 c = (m.level == 0) ? ImVec4(0.7f, 0.7f, 0.7f, 1) :
                  (m.level == 1) ? ImVec4(1.0f, 0.8f, 0.2f, 1) :
                  ImVec4(1.0f, 0.3f, 0.3f, 1);
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        ImGui::Text("%s", m.text.c_str());
        ImGui::PopStyleColor();
    }
}

// ============================================================================
// Viewport - fixed center panel, always the largest area
// ============================================================================
void RenderViewport(Editor::Editor& editor, GLuint viewportTexture,
                 int windowW, int windowH, GLFWwindow* window, ImGuiIO& io,
                 const glm::mat4& projection) {
    // Safety guard 1: no ImGui context -> nothing to render into.
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

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
    // the editor's UI panels.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.07f, 1.0f));
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

    static int vpDiagFrame = 0;
    vpDiagFrame++;
    if (vpDiagFrame <= 3 || vpDiagFrame % 120 == 0) {
        printf("[viewport-ui-diag] frame=%d pos=(%.0f,%.0f) size=(%.0fx%.0f) tex=%u\n",
               vpDiagFrame, vpPos.x, vpPos.y, vpSize.x, vpSize.y, viewportTexture);
        fflush(stdout);
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // ----- Header bar (28px) -----
    ImVec2 winPos = ImGui::GetWindowPos();
    ImVec2 winSize = ImGui::GetWindowSize();

    // Background for the header
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(
        ImVec2(winPos.x, winPos.y),
        ImVec2(winPos.x + winSize.x, winPos.y + 28.0f),
        IM_COL32(35, 38, 45, 255)
    );
    // Bottom border
    dl->AddLine(
        ImVec2(winPos.x, winPos.y + 28.0f),
        ImVec2(winPos.x + winSize.x, winPos.y + 28.0f),
        IM_COL32(60, 60, 70, 255)
    );

    // Title text (manual placement, since we have WindowPadding=0)
    dl->AddText(ImVec2(winPos.x + 10.0f, winPos.y + 6.0f), IM_COL32(220, 225, 235, 255), "Viewport");

    // Render-mode toggle in header (right side) - read state from editor.
    const char* modeLabels[] = { "Lit", "Wire", "Normals", "Unlit" };
    int showWireframe = editor.showWireframe();
    int modeIdx = showWireframe == 1 ? 1 : 0;
    for (int i = 0; i < 4; i++) {
        ImVec2 btnSize(50, 20);
        ImVec2 btnPos(winPos.x + winSize.x - (4 - i) * (btnSize.x + 2) - 8, winPos.y + 4);
        bool isActive = (i == modeIdx);
        dl->AddRectFilled(btnPos, ImVec2(btnPos.x + btnSize.x, btnPos.y + btnSize.y),
                          isActive ? IM_COL32(70, 110, 180, 255) : IM_COL32(50, 53, 60, 255));
        dl->AddRect(btnPos, ImVec2(btnPos.x + btnSize.x, btnPos.y + btnSize.y),
                    IM_COL32(80, 85, 95, 255));
        dl->AddText(ImVec2(btnPos.x + 8, btnPos.y + 3),
                    isActive ? IM_COL32(240, 245, 255, 255) : IM_COL32(170, 175, 185, 255),
                    modeLabels[i]);
    }

    // ----- 3D viewport content area (below header) -----
    ImVec2 contentAvail = ImGui::GetContentRegionAvail();
    float viewY = 28.0f;
    ImVec2 viewSize(contentAvail.x, contentAvail.y - viewY);
    if (viewSize.x > 10 && viewSize.y > 10) {
        // Safety guard 3: missing viewport texture -> draw placeholder text instead
        // of calling ImGui::Image with a null texture handle.
        if (viewportTexture == 0) {
            ImVec2 center(
                (winPos.x + winSize.x) * 0.5f,
                (winPos.y + winSize.y) * 0.5f
            );
            const char* msg = "Viewport unavailable";
            ImVec2 textSize = ImGui::CalcTextSize(msg);
            dl->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f),
                        IM_COL32(180, 180, 180, 255), msg);
        } else {
            // Fill the entire viewport panel with the FBO texture so the skybox
            // and scene are visible across the whole viewport, matching Unreal.
            ImVec2 imgSize = viewSize;

            ImVec2 imgScreenMin = ImGui::GetCursorScreenPos();
            ImVec2 imgScreenMax(imgScreenMin.x + imgSize.x, imgScreenMin.y + imgSize.y);

            ImGui::Image((void*)(intptr_t)viewportTexture, imgSize, ImVec2(0, 1), ImVec2(1, 0));

            // ----- Overlay: bottom-left camera/tool info -----
            char camBuf[160];
            // Safety guard 4: camera may be null (editor not initialized yet).
            flyCamera* camera = editor.camera();
            GizmoRenderer::GizmoType gizmoType = editor.gizmoType();
            GizmoRenderer::SpaceType spaceType = editor.spaceType();
            bool showGrid = editor.showGrid();
            bool showGizmo = editor.showGizmo();
            if (camera) {
                glm::vec3 target = camera->Target;
                glm::vec3 delta = target - camera->Position;
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
            ImVec2 overlayBL(imgScreenMin.x + 8.0f, imgScreenMax.y - 36.0f);
            dl->AddRectFilled(ImVec2(overlayBL.x - 4, overlayBL.y - 2),
                              ImVec2(overlayBL.x + 240, overlayBL.y + 36),
                              IM_COL32(15, 17, 22, 200));
            dl->AddText(overlayBL, IM_COL32(190, 210, 230, 255), camBuf);
            dl->AddText(ImVec2(overlayBL.x, overlayBL.y + 16), IM_COL32(160, 175, 195, 255), toolBuf);

            // ----- Overlay: top-right FPS / draw calls -----
            float fps = io.Framerate;
            char fpsBuf[32];
            snprintf(fpsBuf, sizeof(fpsBuf), "%.0f FPS", fps);
            char resBuf[48];
            snprintf(resBuf, sizeof(resBuf), "%dx%d", (int)imgSize.x, (int)imgSize.y);

            ImVec2 overlayTR(imgScreenMax.x - 110.0f, imgScreenMin.y + 6.0f);
            dl->AddRectFilled(ImVec2(overlayTR.x - 4, overlayTR.y - 2),
                              ImVec2(overlayTR.x + 110, overlayTR.y + 36),
                              IM_COL32(15, 17, 22, 200));
            // Color FPS based on value
            ImU32 fpsCol = fps >= 60.0f ? IM_COL32(120, 220, 140, 255) :
                           fps >= 30.0f ? IM_COL32(220, 220, 120, 255) :
                                           IM_COL32(220, 120, 120, 255);
            dl->AddText(overlayTR, fpsCol, fpsBuf);
            dl->AddText(ImVec2(overlayTR.x, overlayTR.y + 16), IM_COL32(160, 175, 195, 255), resBuf);

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
            if (showGrid && camera && ImGui::IsItemHovered()) {
                glm::mat4 view = camera->GetViewMatrix();
                GridRenderer::Draw(view, projection, ShaderManager::GetMainShaderProgram());
            }

            // ----- Selection gizmo (only when something is selected and toggle is on) -----
            // Safety guard 5: validate the cached selection before dereferencing
            // its transform. If the selected entity no longer exists, clear it.
            ecs::EntityID selected = EntityManager::ValidateOrClear(editor.selectedEntity());
            if (showGizmo && gizmoType != GizmoRenderer::GizmoType::None &&
                selected != ecs::INVALID_ENTITY_ID && camera) {
                const ecs::World& w = editor.world();
                const ecs::TransformComponent* t =
                    w.getComponent<ecs::TransformComponent>(ecs::Entity{selected});
                if (t) {
                    glm::vec3 pos = t->position;
                    glm::quat rot = (spaceType == GizmoRenderer::SpaceType::World)
                                       ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
                                       : glm::quat(t->rotation);
                    float size = 1.0f;
                    glm::mat4 view = camera->GetViewMatrix();
                    GizmoRenderer::Draw(pos, size, rot, view, projection,
                                        ShaderManager::GetGizmoShaderProgram(),
                                        gizmoType);
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

    ImVec2 displaySize = GetDisplaySize();
    ImVec2 pos(0, displaySize.y - STATUS_BAR_HEIGHT);
    ImVec2 size(displaySize.x, STATUS_BAR_HEIGHT);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(25, 25, 25, 255));

    if (!UI::BeginFixedPanel("StatusBar", pos, size)) {
        UI::EndFixedPanel();
        ImGui::PopStyleColor();
        return;
    }

    ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1), "%.0f FPS", fps);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), " | %zu entities", entityCount);

    // Geo status (if geo API available)
    ImGui::SameLine();
#ifndef DISABLE_GEOSPATIAL
    UI::RenderGeoStatusBar(g_editor.geospatialSystem().getGeoAPI());
#else
    // geo status bar disabled
#endif

    ImGui::SameLine(ImGui::GetWindowWidth() - 200);
    if (selected != ecs::INVALID_ENTITY_ID) {
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.9f, 1), "Selected: %u", selected);
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "No selection");
    }

    if (isPlaying) ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1), "Playing");
    else if (wasPlaying) ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1), "Paused");
    else ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "Stopped");

    UI::EndFixedPanel();
    ImGui::PopStyleColor();
}

void RenderAboutDialog(bool& showAbout) {
    if (!showAbout) return;
    ImGui::OpenPopup("About");
    if (ImGui::BeginPopupModal("About", &showAbout, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1), "RTT Engine Editor");
        ImGui::Separator();
        ImGui::Text("Version: 1.0.0");
        ImGui::Text("A professional 3D game engine editor");
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 60, 60, 255));
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            showAbout = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

void RenderTransformSection(ecs::TransformComponent* t) {
    if (!t) return;
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Transform");
        glm::vec3 p = t->position;
        if (ImGui::DragFloat3("##Position", &p.x, 0.1f)) t->position = p;
        glm::vec3 r = glm::degrees(glm::eulerAngles(t->rotation));
        if (ImGui::DragFloat3("##Rotation", &r.x, 1.0f)) t->setEulerAngles(glm::radians(r));
        glm::vec3 s = t->scale;
        if (ImGui::DragFloat3("##Scale", &s.x, 0.01f)) t->scale = s;
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

} // namespace UI
