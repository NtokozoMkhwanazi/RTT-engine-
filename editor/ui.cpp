#include "ui.h"
#include "phosphor_icons_codepoints.h"
#include "editor_state.h"
#include "entity_manager.h"
#include "console.h"
#include "scene_manager.h"
#include "model_loader.h"
#include "ecs/components/Components.h"
#include "cameraSystem/flyCamera.h"
#include "gizmo_renderer.h"
#include "geo_config_panel.h"
#include <GLFW/glfw3.h>
#include <iostream>
#include <algorithm>
#include <vector>
#include <cstring>

namespace UI {

// ============================================================================
// Entity cache rebuild - only on dirty flag, not every frame
// ============================================================================
void RebuildEntityCache(EntityCache& cache, ecs::World& world) {
    cache.entries.clear();

    // Reserve to avoid reallocations
    size_t estimatedCount = world.getEntityCount();
    cache.entries.reserve(estimatedCount);

    // Single pass: collect all entities with TransformComponent
    // Use archetype iteration which is faster than forEach
    world.forEach<ecs::TransformComponent>([&](ecs::EntityID id, ecs::TransformComponent&) {
        EntityCache::Entry entry;
        entry.id = id;
        entry.hasGeo = false;

        // Determine icon type (minimal checks)
        if (world.hasComponent<ecs::CameraComponent>(ecs::Entity{id})) {
            entry.icon = "C";
        } else if (world.hasComponent<ecs::LightComponent>(ecs::Entity{id})) {
            entry.icon = "L";
        } else if (world.hasComponent<ecs::ModelComponent>(ecs::Entity{id})) {
            entry.icon = "M";
        } else {
            entry.icon = "[]";
        }

        if (world.hasComponent<ecs::GeospatialComponent>(ecs::Entity{id})) {
            entry.hasGeo = true;
        }

        cache.entries.push_back(entry);
    });

    cache.dirty = false;
    cache.lastRebuildTime = (float)glfwGetTime();
}

// ============================================================================
// Menu bar
// ============================================================================
void RenderMenuBar(bool& showAbout, ecs::World& world, ecs::EntityID& selected,
               bool& isPlaying, bool& wasPlaying, const std::string& currentSceneFile,
               bool& shouldClose) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Scene", "Ctrl+N")) {
                world.shutdown();
                world.init();
                selected = ecs::INVALID_ENTITY_ID;
                EditorConsole::Log("New scene created");
            }
            if (ImGui::MenuItem("Open Scene...", "Ctrl+O")) {
                if (SceneManager::LoadScene("scene.json", world)) {
                    EditorConsole::Log("Scene loaded");
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S")) {
                SceneManager::SaveScene(currentSceneFile.empty() ? "scene.json" : currentSceneFile, world);
            }
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                shouldClose = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            bool hasSel = (selected != ecs::INVALID_ENTITY_ID);
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, false)) {}
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, false)) {}
            ImGui::Separator();
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSel)) {
                EntityManager::DuplicateEntity(selected);
            }
            if (ImGui::MenuItem("Delete", "Del", false, hasSel)) {
                EntityManager::DeleteEntity(selected);
                selected = ecs::INVALID_ENTITY_ID;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Show Grid", nullptr, &g_editor.showGrid);
            ImGui::MenuItem("Show Gizmo", nullptr, &g_editor.showGizmo);
            ImGui::Separator();
            ImGui::MenuItem("Outliner", nullptr, &g_editor.uiState.showOutliner);
            ImGui::MenuItem("Details", nullptr, &g_editor.uiState.showDetails);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) {
                showAbout = true;
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

void RenderPreferencesDialog(bool& showPreferences, EditorState& editor) {
    if (!showPreferences) return;
    ImGui::OpenPopup("Preferences");
    if (ImGui::BeginPopupModal("Preferences", &showPreferences, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1), "Editor Preferences");
        ImGui::Separator();
        ImGui::Checkbox("Show Grid", &g_editor.showGrid);
        ImGui::Checkbox("Show Gizmo", &g_editor.showGizmo);
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            showPreferences = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

// ============================================================================
// Toolbar - minimal, no heavy ops
// ============================================================================
void RenderToolbar(GizmoRenderer::GizmoType& gizmoType, GizmoRenderer::SpaceType& spaceType,
               bool& showGrid, bool& showGizmo, int& showWireframe) {
    ImGui::BeginChild("Toolbar", ImVec2(-1, 38), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));

    bool tActive = (gizmoType == GizmoRenderer::GizmoType::Translate);
    ImGui::PushStyleColor(ImGuiCol_Button, tActive ? IM_COL32(60, 60, 60, 255) : IM_COL32(40, 40, 40, 255));
    if (ImGui::Button(phosphor_icons::arrows_out(), ImVec2(28, 28))) {
        gizmoType = GizmoRenderer::GizmoType::Translate;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();

    bool rActive = (gizmoType == GizmoRenderer::GizmoType::Rotate);
    ImGui::PushStyleColor(ImGuiCol_Button, rActive ? IM_COL32(60, 60, 60, 255) : IM_COL32(40, 40, 40, 255));
    if (ImGui::Button("R", ImVec2(28, 28))) {
        gizmoType = GizmoRenderer::GizmoType::Rotate;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();

    bool sActive = (gizmoType == GizmoRenderer::GizmoType::Scale);
    ImGui::PushStyleColor(ImGuiCol_Button, sActive ? IM_COL32(60, 60, 60, 255) : IM_COL32(40, 40, 40, 255));
    if (ImGui::Button("S", ImVec2(28, 28))) {
        gizmoType = GizmoRenderer::GizmoType::Scale;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::Separator();
    ImGui::SameLine();

    bool worldSpace = (spaceType == GizmoRenderer::SpaceType::World);
    ImGui::PushStyleColor(ImGuiCol_Button, worldSpace ? IM_COL32(60, 60, 60, 255) : IM_COL32(40, 40, 40, 255));
    if (ImGui::Button(worldSpace ? "World" : "Local", ImVec2(55, 28))) {
        spaceType = worldSpace ? GizmoRenderer::SpaceType::Local : GizmoRenderer::SpaceType::World;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::Separator();
    ImGui::SameLine();

    bool wf = showWireframe != 0;
    ImGui::PushStyleColor(ImGuiCol_Button, wf ? IM_COL32(60, 60, 60, 255) : IM_COL32(40, 40, 40, 255));
    if (ImGui::Button("W", ImVec2(28, 28))) {
        showWireframe = wf ? 0 : 1;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, showGrid ? IM_COL32(60, 60, 60, 255) : IM_COL32(40, 40, 40, 255));
    if (ImGui::Button("G", ImVec2(28, 28))) {
        showGrid = !showGrid;
    }
    ImGui::PopStyleColor();

    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// ============================================================================
// Left panel - uses cached entity list
// ============================================================================
void RenderLeftPanel(int& activeTab, ecs::World& world, ecs::EntityID& selected,
                  EntityCache& entityCache, const char* searchBuffer) {
    auto& panelConfig = g_editor.scenePanelConfig;
    ImGui::Begin("Scene");

    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 tabSize = ImVec2(w / 3.0f - 4, 24);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 4));
    ImGui::PushStyleColor(ImGuiCol_Button, panelConfig.activeTabIndex == 0 ? IM_COL32(60, 60, 60, 255) : IM_COL32(40, 40, 40, 255));
    if (ImGui::Button(phosphor_icons::list(), tabSize)) panelConfig.activeTabIndex = 0;
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, panelConfig.activeTabIndex == 1 ? IM_COL32(60, 60, 60, 255) : IM_COL32(40, 40, 40, 255));
    if (ImGui::Button(phosphor_icons::pencil(), tabSize)) panelConfig.activeTabIndex = 1;
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, panelConfig.activeTabIndex == 2 ? IM_COL32(60, 60, 60, 255) : IM_COL32(40, 40, 40, 255));
    if (ImGui::Button(phosphor_icons::globe(), tabSize)) panelConfig.activeTabIndex = 2;
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::Separator();

    if (panelConfig.activeTabIndex == 0) RenderOutlinerPanel(selected, entityCache, searchBuffer);
    else if (panelConfig.activeTabIndex == 1) RenderDetailsPanel(world, selected);
    else RenderGeoPanel(g_editor.geospatialSystem.getGeoAPI(), g_editor.geoPanelState);

    ImGui::End();
}

// ============================================================================
// Outliner panel - uses cached entity list (O(1) per entity, no ECS queries)
// ============================================================================
void RenderOutlinerPanel(ecs::EntityID& selected, EntityCache& entityCache, const char* searchBuffer) {
    ImGui::PushItemWidth(-1);
    static char buf[64] = "";
    ImGui::InputTextWithHint("##Search", "Search...", buf, 64);
    ImGui::PopItemWidth();

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.5f, 1), "%zu entities", entityCache.entries.size());
    ImGui::Separator();

    // Rebuild cache if dirty (not every frame)
    if (entityCache.dirty) {
        RebuildEntityCache(entityCache, g_editor.world);
    }

    // Render from cache - zero ECS queries
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 1));
    for (const auto& entry : entityCache.entries) {
        bool isSel = (selected == entry.id);

        // Format: "Icon ID"
        char label[32];
        snprintf(label, sizeof(label), "%s %u", entry.icon, entry.id);

        if (isSel) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(70, 130, 180, 255));
        if (entry.hasGeo) {
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(40, 60, 40, 255));
        }

        if (ImGui::Selectable(label, isSel, ImGuiSelectableFlags_SpanAllColumns)) {
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
    if (selected == ecs::INVALID_ENTITY_ID) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "No entity selected");
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
        if (ImGui::DragFloat3("Position", &p.x, 0.1f)) t->position = p;

        glm::vec3 r = glm::degrees(glm::eulerAngles(t->rotation));
        if (ImGui::DragFloat3("Rotation", &r.x, 1.0f, -180, 180)) {
            t->setEulerAngles(glm::radians(r));
        }

        glm::vec3 s = t->scale;
        if (ImGui::DragFloat3("Scale", &s.x, 0.01f, 0.01f, 1000)) t->scale = s;
        ImGui::PopID();
    }

    if (model && model->isValid() && ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Visible", &model->visible);
    }

    if (m && ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Mesh");
        ImVec4 c = ImVec4(m->color.r, m->color.g, m->color.b, 1.0f);
        if (ImGui::ColorEdit3("Albedo", &c.x)) m->color = glm::vec3(c.x, c.y, c.z);
        ImGui::Checkbox("Visible", &m->visible);
        ImGui::SliderFloat("Metallic", &m->metallic, 0.0f, 1.0f);
        ImGui::SliderFloat("Roughness", &m->roughness, 0.0f, 1.0f);
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
}

// ============================================================================
// Bottom panel
// ============================================================================
void RenderBottomPanel(int& activeTab, float fps, const flyCamera* camera, ecs::EntityID& selected) {
    ImGui::Begin("Debug");

    ImVec2 tabSize = ImVec2(100, 24);
    ImGui::PushStyleColor(ImGuiCol_Button, activeTab == 0 ? IM_COL32(60, 60, 60, 255) : IM_COL32(40, 40, 40, 255));
    if (ImGui::Button("Console", tabSize)) activeTab = 0;
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, activeTab == 1 ? IM_COL32(60, 60, 60, 255) : IM_COL32(40, 40, 40, 255));
    if (ImGui::Button("Profiler", tabSize)) activeTab = 1;
    ImGui::PopStyleColor();

    ImGui::Separator();

    if (activeTab == 0) {
        RenderConsolePanel();
    } else {
        ImGui::Columns(3, "Stats", true);
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "FPS"); ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "%.1f", fps); ImGui::NextColumn();

        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Frame"); ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.9f, 1), "%.2f ms", 1000.0f / (fps > 0 ? fps : 60)); ImGui::NextColumn();

        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Entities"); ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1), "%zu", g_editor.world.getEntityCount()); ImGui::NextColumn();

        ImGui::Columns(1);

        if (camera) {
            ImGui::Text("Camera: (%.1f, %.1f, %.1f)", camera->Position.x, camera->Position.y, camera->Position.z);
        }
    }

    ImGui::End();
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
// Viewport
// ============================================================================
void RenderViewport(ecs::EntityID& selected, flyCamera* camera, bool& isViewing,
                 glm::vec2& lastMousePos, GLuint viewportTexture, int windowW, int windowH,
                 GizmoRenderer::GizmoType gizmoType, GizmoRenderer::SpaceType spaceType, int showWireframe,
                 bool showGrid, bool showGizmo, GLFWwindow* window, ImGuiIO& io) {
    float vpX = 270;
    float vpY = 25 + 38;
    float vpW = windowW - 270 - 300;
    float vpH = windowW > 0 ? (windowH - 25 - 38 - 180 - 24) : 800;

    ImGui::SetNextWindowPos(ImVec2(vpX, vpY), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(vpW, vpH), ImGuiCond_FirstUseEver);
    ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ImVec2 cs = ImGui::GetContentRegionAvail();
    if (cs.x > 10 && cs.y > 10) {
        ImGui::Image((void*)(intptr_t)viewportTexture, cs, ImVec2(0, 1), ImVec2(1, 0));

        ImVec2 vpMin = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(vpMin.x + 10, vpMin.y + 10));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 0.8f));

        if (camera) {
            ImGui::Text("Camera: (%.1f, %.1f, %.1f)", camera->Position.x, camera->Position.y, camera->Position.z);
        }

        const char* giz = (gizmoType == GizmoRenderer::GizmoType::Translate) ? "Translate" :
                       (gizmoType == GizmoRenderer::GizmoType::Rotate) ? "Rotate" : "Scale";
        const char* sp = (spaceType == GizmoRenderer::SpaceType::World) ? "World" : "Local";
        ImGui::Text("Tool: %s | Space: %s", giz, sp);
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

// ============================================================================
// Status bar - reads from cached data, NO ECS iteration
// ============================================================================
void RenderStatusBar(size_t entityCount, ecs::EntityID selected, float fps,
                   bool isPlaying, bool wasPlaying, int windowW) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(25, 25, 25, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);

    ImGui::SetNextWindowPos(ImVec2(0, windowW > 0 ? windowW - 24 : 1056));
    ImGui::SetNextWindowSize(ImVec2(windowW > 0 ? windowW : 1920, 24));
    ImGui::Begin("StatusBar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);

    ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1), "%.0f FPS", fps);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), " | %zu entities", entityCount);

    // Geo status (if geo API available)
    ImGui::SameLine();
    RenderGeoStatusBar(g_editor.geospatialSystem.getGeoAPI());

    ImGui::SameLine(ImGui::GetWindowWidth() - 200);
    if (selected != ecs::INVALID_ENTITY_ID) {
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.9f, 1), "Selected: %u", selected);
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "No selection");
    }

    if (isPlaying) ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1), "Playing");
    else if (wasPlaying) ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1), "Paused");
    else ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "Stopped");

    ImGui::End();
    ImGui::PopStyleVar(2);
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
