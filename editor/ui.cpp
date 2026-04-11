#include "ui.h"
#include "editor_state.h"
#include "entity_manager.h"
#include "console.h"
#include "scene_manager.h"
#include "ecs/components/Components.h"
#include "cameraSystem/flyCamera.h"
#include "geospatial/GeospatialConverter.h"
#include "geospatial/GPSTracker.h"
#include "renderer/GPUProfilerAdvanced.h"
#include "gizmo_renderer.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <algorithm>

// Icon definitions
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

// Forward declarations for entity creation functions (defined in test.cpp or entity_manager.cpp)
extern ecs::Entity EntityManager::CreateCube(const glm::vec3& pos, const glm::vec3& scale, const glm::vec3& color);
extern ecs::Entity EntityManager::CreateSphere(const glm::vec3& pos, float radius, const glm::vec3& color);
extern ecs::Entity EntityManager::CreatePlane(const glm::vec3& pos, const glm::vec2& size, const glm::vec3& color);
#include "entity_manager.h"
#include "console.h"
#include "scene_manager.h"
namespace UI {

void RenderTransformSection(ecs::TransformComponent* t) {
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

void RenderMeshSection(ecs::MeshComponent* m) {
    if (!m) return;

    if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Mesh");

        ImGui::SeparatorText("Appearance");
        glm::vec3 c = m->color;
        if (ImGui::ColorEdit3("Albedo", &c.x, ImGuiColorEditFlags_Float)) {
            m->color = c;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(60, 0))) {
            m->color = glm::vec3(0.8f);
        }

        ImGui::Checkbox("Visible", &m->visible);

        ImGui::SeparatorText("Material");
        ImGui::SliderFloat("Metallic", &m->metallic, 0.0f, 1.0f);
        ImGui::SliderFloat("Roughness", &m->roughness, 0.0f, 1.0f);

        ImGui::PopID();
    }
}

void RenderMenuBar(bool& showAbout, ecs::World& world, ecs::EntityID& selected,
                   bool& isPlaying, bool& wasPlaying, const std::string& currentSceneFile,
                   bool& shouldClose) {
    if (ImGui::BeginMainMenuBar()) {
        // ========== FILE MENU ==========
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Scene", "Ctrl+N")) {
                // Confirm if there are unsaved changes
                world.shutdown();
                world.init();
                selected = ecs::INVALID_ENTITY_ID;
                EditorConsole::Log("New scene created");
            }
            if (ImGui::MenuItem("Open Scene...", "Ctrl+O")) {
                // Open file dialog (simplified - loads default scene.json)
                if (SceneManager::LoadScene("scene.json", world)) {
                    EditorConsole::Log("Scene loaded: scene.json");
                } else {
                    EditorConsole::Log("Failed to load scene", 2);
                }
            }
            
            // Recent files (placeholder)
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "Recent Files:");
            ImGui::MenuItem("scene.json");
            
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S")) {
                std::string file = currentSceneFile.empty() ? "scene.json" : currentSceneFile;
                if (SceneManager::SaveScene(file, world)) {
                    EditorConsole::Log("Scene saved: " + file);
                } else {
                    EditorConsole::Log("Failed to save scene", 2);
                }
            }
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) {
                // Save with new filename
                if (SceneManager::SaveScene("scene_autosave.json", world)) {
                    EditorConsole::Log("Scene saved: scene_autosave.json");
                } else {
                    EditorConsole::Log("Failed to save scene", 2);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                shouldClose = true;
            }
            ImGui::EndMenu();
        }

        // ========== EDIT MENU ==========
        if (ImGui::BeginMenu("Edit")) {
            bool canUndo = false; // TODO: Implement undo system
            bool canRedo = false;
            
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) {
                EditorConsole::Log("Undo not yet implemented", 1);
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo)) {
                EditorConsole::Log("Redo not yet implemented", 1);
            }
            ImGui::Separator();
            
            // Edit operations
            bool hasSelection = (selected != ecs::INVALID_ENTITY_ID);
            
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSelection)) {
                EntityManager::DuplicateEntity(selected);
                EditorConsole::Log("Entity duplicated");
            }
            if (ImGui::MenuItem("Delete", "Del", false, hasSelection)) {
                EntityManager::DeleteEntity(selected);
                selected = ecs::INVALID_ENTITY_ID;
                EditorConsole::Log("Entity deleted");
            }
            if (ImGui::MenuItem("Select All", "Ctrl+A")) {
                EditorConsole::Log("Select all not yet implemented", 1);
            }
            if (ImGui::MenuItem("Deselect All", "Esc")) {
                selected = ecs::INVALID_ENTITY_ID;
                EditorConsole::Log("Selection cleared");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Preferences...")) {
                g_editor.uiState.showPreferences = true;
            }
            ImGui::EndMenu();
        }

        // ========== GAMEOBJECT MENU ==========
        if (ImGui::BeginMenu("GameObject")) {
            if (ImGui::MenuItem("Cube")) {
                EntityManager::CreateCube(glm::vec3(0, 1, 0), glm::vec3(1), glm::vec3(0.8f, 0.8f, 0.8f));
                EditorConsole::Log("Created Cube");
            }
            if (ImGui::MenuItem("Sphere")) {
                EntityManager::CreateSphere(glm::vec3(0, 1, 0), 0.5f, glm::vec3(0.2f, 0.8f, 0.2f));
                EditorConsole::Log("Created Sphere");
            }
            if (ImGui::MenuItem("Plane")) {
                EntityManager::CreatePlane(glm::vec3(0, 0, 0), glm::vec2(10, 10), glm::vec3(0.5f, 0.5f, 0.5f));
                EditorConsole::Log("Created Plane");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Cylinder")) {
                EntityManager::CreateCylinder(glm::vec3(0, 1, 0), 0.5f, 1.0f, glm::vec3(0.8f, 0.8f, 0.2f));
                EditorConsole::Log("Created Cylinder");
            }
            if (ImGui::MenuItem("Cone")) {
                EntityManager::CreateCone(glm::vec3(0, 1, 0), 0.5f, 1.0f, glm::vec3(0.2f, 0.8f, 0.8f));
                EditorConsole::Log("Created Cone");
            }
            if (ImGui::MenuItem("Torus")) {
                EntityManager::CreateTorus(glm::vec3(0, 1, 0), 0.35f, 0.15f, glm::vec3(0.8f, 0.2f, 0.8f));
                EditorConsole::Log("Created Torus");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Point Light")) {
                EntityManager::CreateLight(glm::vec3(5, 10, 5), glm::vec3(1, 1, 0.9f), 1.0f);
                EditorConsole::Log("Created Light");
            }
            if (ImGui::MenuItem("Camera")) {
                EntityManager::CreateCamera(glm::vec3(0, 5, 10), glm::vec3(0, 0, 0));
                EditorConsole::Log("Created Camera");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Empty Object")) {
                auto entity = g_editor.world.createEntity();
                g_editor.world.addComponent<ecs::TransformComponent>(entity);
                EditorConsole::Log("Created Empty Object");
            }
            ImGui::EndMenu();
        }

        // ========== WINDOW MENU ==========
        if (ImGui::BeginMenu("Window")) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Panels:");
            
            ImGui::MenuItem("World Outliner", nullptr, &g_editor.uiState.showOutliner);
            ImGui::MenuItem("Details", nullptr, &g_editor.uiState.showDetails);
            ImGui::MenuItem("Toolbox", nullptr, &g_editor.uiState.showToolbox);
            ImGui::Separator();
            ImGui::MenuItem("Console", nullptr, &g_editor.uiState.showConsole);
            ImGui::MenuItem("Content Browser", nullptr, &g_editor.uiState.showContentBrowser);
            ImGui::MenuItem("Game Mode", nullptr, &g_editor.uiState.showGameMode);
            ImGui::MenuItem("Profiler", nullptr, &g_editor.uiState.showProfiler);
            ImGui::Separator();
            
            if (ImGui::MenuItem("Reset Layout")) {
                g_editor.uiState.showOutliner = true;
                g_editor.uiState.showDetails = true;
                g_editor.uiState.showToolbox = true;
                g_editor.uiState.showConsole = false;
                g_editor.uiState.showContentBrowser = false;
                g_editor.uiState.showGameMode = false;
                g_editor.uiState.showProfiler = false;
                EditorConsole::Log("UI layout reset");
            }
            ImGui::EndMenu();
        }

        // ========== HELP MENU ==========
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Documentation")) {
                // Try to open documentation in browser
                EditorConsole::Log("Documentation: Check docs/ folder or README.md");
            }
            if (ImGui::MenuItem("Keyboard Shortcuts")) {
                EditorConsole::Log("Shortcuts: W=Translate, E=Rotate, R=Scale, X=Space, Del=Delete, Ctrl+D=Duplicate");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("System Info")) {
                EditorConsole::Log("OpenGL: " + std::string(reinterpret_cast<const char*>(glGetString(GL_VERSION))));
                EditorConsole::Log("Renderer: " + std::string(reinterpret_cast<const char*>(glGetString(GL_RENDERER))));
                EditorConsole::Log("Vendor: " + std::string(reinterpret_cast<const char*>(glGetString(GL_VENDOR))));
            }
            ImGui::Separator();
            if (ImGui::MenuItem("About RTT Engine")) {
                showAbout = true;
            }
            ImGui::EndMenu();
        }

        // Performance stats on right side
        ImGui::Separator();
        ImGui::SameLine();

        char perfText[64];
        snprintf(perfText, sizeof(perfText), "FPS: %.0f  |  Entities: %zu", g_editor.fps, world.getEntityCount());
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
}

void RenderToolbar(GizmoRenderer::GizmoType& gizmoType, GizmoRenderer::SpaceType& spaceType, bool& showGrid,
                   bool& showGizmo, bool& showWireframe, ecs::World& world,
                   ecs::EntityID& selected) {
    const float toolbarHeight = 36.0f;
    
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.08f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::BeginChild("Toolbar", ImVec2(-1, toolbarHeight),
                     false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollbar);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 3));

    // Entity creation buttons
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.3f, 0.4f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.4f, 0.5f, 1));

    if (ImGui::Button(ICON_FA_CUBE " Cube", ImVec2(75, 26))) {
        EntityManager::CreateCube(glm::vec3(0, 1, 0), glm::vec3(1), glm::vec3(1.0f, 0.2f, 0.2f));
        EditorConsole::Log("Created Cube");
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CUBE " Sphere", ImVec2(75, 26))) {
        EntityManager::CreateSphere(glm::vec3(0, 1, 0), 0.5f, glm::vec3(0.2f, 1.0f, 0.2f));
        EditorConsole::Log("Created Sphere");
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CUBE " Plane", ImVec2(75, 26))) {
        EntityManager::CreatePlane(glm::vec3(0, 0, 0), glm::vec2(10, 10), glm::vec3(0.5f, 0.5f, 0.5f));
        EditorConsole::Log("Created Plane");
    }
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    ImGui::PopStyleColor(2);

    // Transform tools
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Tools:");
    ImGui::SameLine();

    const char* transformTools[] = {"Translate", "Rotate", "Scale"};
    GizmoRenderer::GizmoType transformTypes[] = {GizmoRenderer::GizmoType::Translate, GizmoRenderer::GizmoType::Rotate, GizmoRenderer::GizmoType::Scale};
    const char* transformTooltips[] = {"Translate Tool (W)", "Rotate Tool (E)", "Scale Tool (R)"};

    for (int i = 0; i < 3; i++) {
        bool isActive = (gizmoType == transformTypes[i]);
        ImGui::PushStyleColor(ImGuiCol_Button, isActive ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button(transformTools[i], ImVec2(75, 28))) {
            gizmoType = transformTypes[i];
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

    const char* spaceText = (spaceType == GizmoRenderer::SpaceType::World) ? "World" : "Local";
    if (ImGui::Button(spaceText, ImVec2(70, 28))) {
        spaceType = (spaceType == GizmoRenderer::SpaceType::World) ? GizmoRenderer::SpaceType::Local : GizmoRenderer::SpaceType::World;
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

    ImGui::PushStyleColor(ImGuiCol_Button, showGrid ? ImVec4(0.3f, 0.3f, 0.2f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
    if (ImGui::Button("Grid", ImVec2(50, 28))) showGrid = !showGrid;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Grid Display");
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, showGizmo ? ImVec4(0.3f, 0.3f, 0.2f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
    if (ImGui::Button("Gizmo", ImVec2(55, 28))) showGizmo = !showGizmo;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Gizmo Display");
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, showWireframe ? ImVec4(0.3f, 0.2f, 0.3f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
    if (ImGui::Button("Wire", ImVec2(50, 28))) showWireframe = !showWireframe;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Wireframe Mode (Debug)");
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PopStyleVar(2);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void RenderLeftPanel(int& activeTab, ecs::World& world, ecs::EntityID& selected,
                     const char* searchBuffer) {
    const float sidePanelWidth = 280.0f;
    const float menuBarHeight = 25.0f;
    const float toolbarHeight = 36.0f;
    const float tabbedBottomHeight = 180.0f;

    ImGui::SetNextWindowPos(ImVec2(0, menuBarHeight + toolbarHeight));
    ImGui::SetNextWindowSize(ImVec2(sidePanelWidth,
        1080.0f - menuBarHeight - toolbarHeight - tabbedBottomHeight));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.25f, 0.25f, 0.25f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1));
    ImGui::Begin("Scene", nullptr,
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Tab buttons
    ImGui::PushStyleColor(ImGuiCol_Button, activeTab == 0 ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
    if (ImGui::Button("Outliner", ImVec2(sidePanelWidth/3 - 5, 28))) activeTab = 0;
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, activeTab == 1 ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
    if (ImGui::Button("Details", ImVec2(sidePanelWidth/3 - 5, 28))) activeTab = 1;
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, activeTab == 2 ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
    if (ImGui::Button("Geo", ImVec2(sidePanelWidth/3 - 5, 28))) activeTab = 2;
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    if (activeTab == 0) {
        // ========== WORLD OUTLINER ==========
        ImGui::PushItemWidth(-1);
        ImGui::InputTextWithHint("##Search", "Search entities...", const_cast<char*>(searchBuffer), 128);
        ImGui::PopItemWidth();
        ImGui::Separator();
        ImGui::Spacing();

        int entityCount = 0;
        world.forEach<ecs::TransformComponent, ecs::MeshComponent>(
            [&](ecs::EntityID id, ecs::TransformComponent&, ecs::MeshComponent& m) {
                bool isSelected = (selected == id);
                ImGui::PushStyleColor(ImGuiCol_Text,
                    isSelected ? ImVec4(1.0f, 0.8f, 0.2f, 1) : ImVec4(0.85f, 0.85f, 0.85f, 1));

                char label[64];
                snprintf(label, sizeof(label), "Entity %d", id);

                if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    selected = id;
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        EditorConsole::Log("Focus on: " + std::string(label));
                    }
                }

                char popupId[128];
                snprintf(popupId, sizeof(popupId), "EntityPopup##%d", id);
                if (ImGui::BeginPopupContextItem(popupId)) {
                    selected = id;
                    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
                        EntityManager::DuplicateEntity(g_editor.selectedEntity);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Delete", "Delete")) {
                        EntityManager::DeleteEntity(g_editor.selectedEntity);
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

        if (ImGui::BeginPopupContextWindow("OutlinerContext")) {
            if (ImGui::MenuItem("Create Cube")) {
                EntityManager::CreateCube(glm::vec3(0, 1, 0), glm::vec3(1), glm::vec3(0.8f, 0.8f, 0.8f));
            }
            if (ImGui::MenuItem("Create Sphere")) {
                EntityManager::CreateSphere(glm::vec3(0, 1, 0), 0.5f, glm::vec3(0.2f, 0.8f, 0.2f));
            }
            if (ImGui::MenuItem("Create Plane")) {
                EntityManager::CreatePlane(glm::vec3(0, 0, 0), glm::vec2(10, 10), glm::vec3(0.5f, 0.5f, 0.5f));
            }
            ImGui::EndPopup();
        }

        if (entityCount == 0) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "No entities in scene");
            ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1), "Right-click to create one");
        }
    } else if (activeTab == 1) {
        // ========== DETAILS ==========
        if (selected != ecs::INVALID_ENTITY_ID) {
            ecs::Entity selectedEntity{selected};
            bool entityStillExists = false;
            ecs::TransformComponent* t = nullptr;
            ecs::MeshComponent* m = nullptr;

            if (selectedEntity.isValid()) {
                t = world.getComponentArchetype<ecs::TransformComponent>(selectedEntity);
                m = world.getComponentArchetype<ecs::MeshComponent>(selectedEntity);
                entityStillExists = (t || m);
            }

            ImGui::Separator();
            ImGui::Spacing();

            if (t && entityStillExists) RenderTransformSection(t);
            if (m && entityStillExists) RenderMeshSection(m);
            
            // Component management section
            ImGui::Spacing();
            RenderComponentPanel(world, selected);

        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "No entity selected");
            ImGui::Dummy(ImVec2(0, 20));
            ImGui::TextWrapped("Select an entity from the Outliner to view and edit its properties.");
        }
    } else if (activeTab == 2) {
        // ========== GEOSPATIAL PANEL ==========
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "Geospatial panel (placeholder)");
    }

    ImGui::PopStyleColor(2);
    ImGui::End();
}

void RenderBottomPanel(int& activeTab, ecs::World& world, ecs::EntityID& selected,
                       float fps, const flyCamera* camera) {
    const float tabbedBottomHeight = 180.0f;
    int windowW = 1920; // Placeholder

    ImGui::SetNextWindowPos(ImVec2(0, 1080.0f - tabbedBottomHeight));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(windowW), tabbedBottomHeight));
    ImGui::Begin("Toolbox", nullptr,
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1));

    // Tab buttons - simplified since we have separate windows now
    ImGui::PushStyleColor(ImGuiCol_Button, activeTab == 0 ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
    if (ImGui::Button("Console", ImVec2(90, 28))) activeTab = 0;
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, activeTab == 1 ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
    if (ImGui::Button("Profiler", ImVec2(90, 28))) activeTab = 1;
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::Spacing();

    if (activeTab == 0) {
        // Quick console view (last 10 messages)
        const auto& messages = EditorConsole::GetMessages();
        int displayCount = std::min(10, (int)messages.size());
        
        ImGui::BeginChild("QuickConsole", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
        for (int i = messages.size() - displayCount; i < (int)messages.size(); i++) {
            if (i < 0) continue;
            const auto& msg = messages[i];
            
            ImVec4 color;
            switch (msg.level) {
                case 0: color = ImVec4(0.7f, 0.7f, 0.7f, 1.0f); break;
                case 1: color = ImVec4(1.0f, 0.8f, 0.2f, 1.0f); break;
                case 2: color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); break;
                default: color = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
            }
            
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::Text("%s", msg.text.c_str());
            ImGui::PopStyleColor();
        }
        
        // Auto-scroll
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    } else if (activeTab == 1) {
        // Profiler
        ImGui::Text("FPS: %.1f", fps);
        ImGui::Text("Frame Time: %.2f ms", 1000.0f / (fps > 0 ? fps : 60));
        ImGui::Text("Entity Count: %zu", world.getEntityCount());
        if (camera) {
            ImGui::Text("Camera: (%.2f, %.2f, %.2f)",
                camera->Position.x, camera->Position.y, camera->Position.z);
        }
        
        ImGui::Separator();
        
        // GPU Profiler
        auto& profiler = AdvancedGPUProfiler::getInstance();
        float gpuTime = profiler.getFrameTimeMs();
        if (gpuTime > 0) {
            ImGui::Text("GPU Time: %.2f ms", gpuTime);
        }
    }

    ImGui::PopStyleColor();
    ImGui::End();
}

void RenderViewport(ecs::EntityID& selected, flyCamera* camera, bool& isViewing,
                    glm::vec2& lastMousePos, GLuint viewportTexture, int windowW, int windowH,
                    GizmoRenderer::GizmoType gizmoType, GizmoRenderer::SpaceType spaceType, bool showWireframe,
                    bool showGrid, bool showGizmo, GLFWwindow* window, ImGuiIO& io) {
    const float sidePanelWidth = 280.0f;
    const float menuBarHeight = 25.0f;
    const float toolbarHeight = 36.0f;
    const float statusBarHeight = 24.0f;
    const float tabbedBottomHeight = 180.0f;

    // Calculate viewport position and size dynamically
    float vpX = sidePanelWidth;
    float vpY = menuBarHeight + toolbarHeight;
    float vpW = static_cast<float>(windowW) - sidePanelWidth;
    float vpH = static_cast<float>(windowH) - menuBarHeight - toolbarHeight - tabbedBottomHeight - statusBarHeight;

    // Ensure minimum dimensions
    if (vpW < 100.0f) vpW = 100.0f;
    if (vpH < 100.0f) vpH = 100.0f;

    ImGui::SetNextWindowPos(ImVec2(vpX, vpY), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(vpW, vpH), ImGuiCond_FirstUseEver);
    ImGui::Begin("Viewport", nullptr,
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Get actual window size for texture display
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    if (contentSize.x > 10.0f && contentSize.y > 10.0f) {
        int w = static_cast<int>(contentSize.x);
        int h = static_cast<int>(contentSize.y);

        // Display FBO texture
        ImGui::Image((void*)(intptr_t)viewportTexture,
                    ImVec2(static_cast<float>(w), static_cast<float>(h)),
                    ImVec2(0, 1), ImVec2(1, 0));

        // Viewport overlay info
        ImVec2 viewportMin = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(viewportMin.x + 10, viewportMin.y + 10));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 0.8f));
        if (camera) {
            ImGui::Text("Camera: (%.1f, %.1f, %.1f)", camera->Position.x, camera->Position.y, camera->Position.z);
        }
        ImGui::Text("Gizmo: %s | Space: %s",
            gizmoType == GizmoRenderer::GizmoType::Translate ? "Translate" :
            gizmoType == GizmoRenderer::GizmoType::Rotate ? "Rotate" : "Scale",
            spaceType == GizmoRenderer::SpaceType::World ? "World" : "Local");
        ImGui::PopStyleColor();
    }

    ImGui::End();
}

void RenderStatusBar(ecs::World& world, ecs::EntityID& selected, float fps,
                     bool isPlaying, bool wasPlaying, int windowW) {
    const float statusBarHeight = 24.0f;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
    ImGui::SetNextWindowPos(ImVec2(0, static_cast<float>(1080) - statusBarHeight));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(windowW), statusBarHeight));
    ImGui::Begin("StatusBar", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    int totalEntities = 0;
    world.forEach<ecs::TransformComponent>([&](ecs::EntityID, ecs::TransformComponent&) {
        totalEntities++;
    });

    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Entities: %d  |  Selected: %s",
        totalEntities,
        selected != ecs::INVALID_ENTITY_ID ? "Yes" : "No");

    if (isPlaying) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1));
        ImGui::Text("\xef\x81\x8b  PLAYING");
        ImGui::PopStyleColor();
    }

    char statusText[128];
    snprintf(statusText, sizeof(statusText), "FPS: %.0f  |  Frame: %.2fms",
        fps, 1000.0f / (fps > 0 ? fps : 60));

    float textWidth = ImGui::CalcTextSize(statusText).x + 20;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - textWidth);
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "%s", statusText);

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void RenderAboutDialog(bool& showAbout) {
    if (!showAbout) return;

    ImGui::OpenPopup("About RTT Engine");
    if (ImGui::BeginPopupModal("About RTT Engine", &showAbout, ImGuiWindowFlags_AlwaysAutoResize)) {
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
        ImGui::Dummy(ImVec2(0, 15));

        if (ImGui::Button("OK", ImVec2(120, 0))) {
            showAbout = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

void RenderConsolePanel(ecs::World& world) {
    ImGui::Begin("Console");
    
    // Filter buttons
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
    ImGui::Text("Filter:");
    ImGui::SameLine();
    
    int& filter = EditorConsole::GetFilterRef();
    bool filterAll = filter == -1;
    bool filterInfo = filter == 0;
    bool filterWarn = filter == 1;
    bool filterError = filter == 2;
    
    if (ImGui::Checkbox("All", &filterAll)) filter = -1;
    ImGui::SameLine();
    if (ImGui::Checkbox("Info", &filterInfo)) filter = 0;
    ImGui::SameLine();
    if (ImGui::Checkbox("Warning", &filterWarn)) filter = 1;
    ImGui::SameLine();
    if (ImGui::Checkbox("Error", &filterError)) filter = 2;
    
    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
    if (ImGui::SmallButton("Clear")) {
        EditorConsole::Clear();
    }
    ImGui::PopStyleVar();
    
    ImGui::Separator();
    
    // Display log messages
    const auto& messages = EditorConsole::GetMessages();
    for (const auto& msg : messages) {
        // Apply filter
        if (filter != -1 && msg.level != filter) continue;
        
        ImVec4 color;
        const char* prefix;
        switch (msg.level) {
            case 0: color = ImVec4(0.7f, 0.7f, 0.7f, 1.0f); prefix = "[INFO]"; break;
            case 1: color = ImVec4(1.0f, 0.8f, 0.2f, 1.0f); prefix = "[WARN]"; break;
            case 2: color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); prefix = "[ERROR]"; break;
            default: color = ImVec4(0.7f, 0.7f, 0.7f, 1.0f); prefix = "[INFO]";
        }
        
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::Text("%s %s", prefix, msg.text.c_str());
        ImGui::PopStyleColor();
    }
    
    // Auto-scroll to bottom
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    
    ImGui::End();
}

void RenderContentBrowser(ecs::World& world) {
    ImGui::Begin("Content Browser");
    
    // Path navigation
    static char currentPath[512] = "assets/";
    static std::vector<std::string> pathHistory;
    
    ImGui::Text("Path:");
    ImGui::SameLine();
    ImGui::PushItemWidth(ImGui::GetWindowWidth() - 200);
    ImGui::InputText("##Path", currentPath, sizeof(currentPath));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        // Refresh current directory
    }
    
    ImGui::Separator();
    
    // Asset display area
    ImGui::Text("Assets:");
    ImGui::Separator();
    
    // Placeholder asset grid
    ImGui::BeginChild("AssetGrid", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), false);
    
    // Example assets
    const char* assetFolders[] = {"Materials", "Meshes", "Textures", "Shaders", "Audio", "Prefabs"};
    const char* folderIcons[] = {ICON_FA_FOLDER, ICON_FA_FOLDER, ICON_FA_FOLDER, ICON_FA_FOLDER, ICON_FA_FOLDER, ICON_FA_FOLDER};
    
    for (int i = 0; i < 6; i++) {
        if (i % 4 != 0) ImGui::SameLine();
        
        ImGui::BeginGroup();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s", folderIcons[i]);
        ImGui::Text("%s", assetFolders[i]);
        ImGui::EndGroup();
        
        if (ImGui::IsItemClicked()) {
            EditorConsole::Log("Opening folder: " + std::string(assetFolders[i]));
        }
    }
    
    // Example files
    ImGui::Spacing();
    ImGui::Text("Files:");
    const char* files[] = {"cube.obj", "sphere.fbx", "plane.gltf", "texture.png", "material.json"};
    for (int i = 0; i < 5; i++) {
        if (i % 4 != 0) ImGui::SameLine();
        
        ImGui::BeginGroup();
        ImGui::Text(ICON_FA_FILE);
        ImGui::Text("%s", files[i]);
        ImGui::EndGroup();
        
        if (ImGui::IsItemClicked()) {
            EditorConsole::Log("Selected file: " + std::string(files[i]));
        }
        
        char filePopupId[128];
        snprintf(filePopupId, sizeof(filePopupId), "FilePopup##%d", i);
        if (ImGui::BeginPopupContextItem(filePopupId)) {
            if (ImGui::MenuItem("Import")) {
                EditorConsole::Log("Importing: " + std::string(files[i]));
            }
            if (ImGui::MenuItem("Delete")) {
                EditorConsole::Log("Deleting: " + std::string(files[i]), 1);
            }
            ImGui::EndPopup();
        }
    }
    
    ImGui::EndChild();
    
    ImGui::End();
}

void RenderComponentPanel(ecs::World& world, ecs::EntityID selected) {
    if (selected == ecs::INVALID_ENTITY_ID) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No entity selected");
        return;
    }
    
    ecs::Entity entity{selected};
    if (!entity.isValid()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Invalid entity");
        return;
    }
    
    ImGui::SeparatorText("Components");
    
    // List existing components
    ImGui::Text("Existing Components:");
    
    bool hasTransform = world.hasComponent<ecs::TransformComponent>(entity);
    bool hasMesh = world.hasComponent<ecs::MeshComponent>(entity);
    bool hasCamera = world.hasComponent<ecs::CameraComponent>(entity);
    bool hasLight = world.hasComponent<ecs::LightComponent>(entity);
    
    if (hasTransform) {
        ImGui::BulletText("Transform");
        ImGui::SameLine(ImGui::GetWindowWidth() - 80);
        if (ImGui::SmallButton("Remove##Transform")) {
            EditorConsole::Log("Cannot remove Transform component", 1);
        }
    }
    
    if (hasMesh) {
        ImGui::BulletText("Mesh");
        ImGui::SameLine(ImGui::GetWindowWidth() - 80);
        if (ImGui::SmallButton("Remove##Mesh")) {
            // world.removeComponent<ecs::MeshComponent>(selected);
            EditorConsole::Log("Removed Mesh component");
        }
    }
    
    if (hasCamera) {
        ImGui::BulletText("Camera");
        ImGui::SameLine(ImGui::GetWindowWidth() - 80);
        if (ImGui::SmallButton("Remove##Camera")) {
            // world.removeComponent<ecs::CameraComponent>(selected);
            EditorConsole::Log("Removed Camera component");
        }
    }
    
    if (hasLight) {
        ImGui::BulletText("Light");
        ImGui::SameLine(ImGui::GetWindowWidth() - 80);
        if (ImGui::SmallButton("Remove##Light")) {
            //world.removeComponent<ecs::LightComponent>(selected);
            EditorConsole::Log("Removed Light component");
        }
    }
    
    ImGui::Separator();
    
    // Add component buttons
    ImGui::Text("Add Components:");
    
    if (!hasMesh) {
        if (ImGui::Button("Add Mesh")) {
            // world.addComponent<ecs::MeshComponent>(selected);
            EditorConsole::Log("Added Mesh component");
        }
    }
    
    if (!hasCamera) {
        if (ImGui::Button("Add Camera")) {
            // world.addComponent<ecs::CameraComponent>(selected);
            EditorConsole::Log("Added Camera component");
        }
    }
    
    if (!hasLight) {
        if (ImGui::Button("Add Light")) {
            // world.addComponent<ecs::LightComponent>(selected);
            EditorConsole::Log("Added Light component");
        }
    }
}

void RenderGameModeControls(bool& isPlaying, bool& wasPlaying, float& gameSpeed, ecs::World& world) {
    ImGui::Begin("Game Mode");
    
    if (!isPlaying) {
        if (ImGui::Button(ICON_FA_PLAY " Play", ImVec2(-1, 30))) {
            isPlaying = true;
            wasPlaying = true;
            EditorConsole::Log("Game started");
        }
    } else {
        if (ImGui::Button(ICON_FA_PAUSE " Pause", ImVec2(-1, 30))) {
            isPlaying = false;
            EditorConsole::Log("Game paused");
        }
        
        if (ImGui::Button(ICON_FA_STOP " Stop", ImVec2(-1, 30))) {
            isPlaying = false;
            wasPlaying = false;
            EditorConsole::Log("Game stopped");
        }
    }
    
    ImGui::Separator();
    
    ImGui::Text("Game Speed:");
    if (ImGui::SliderFloat("##Speed", &gameSpeed, 0.1f, 3.0f, "%.1fx")) {
        EditorConsole::Log("Game speed changed to " + std::to_string(gameSpeed) + "x");
    }
    
    ImGui::Separator();
    
    // Game state info
    if (isPlaying) {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Status: Playing");
    } else if (wasPlaying) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Status: Paused");
    } else {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Status: Stopped");
    }
    
    ImGui::Text("Entities: %zu", world.getEntityCount());
    
    ImGui::End();
}

void RenderPreferencesDialog(bool& showPreferences, EditorState& editor) {
    if (!showPreferences) return;

    ImGui::OpenPopup("Preferences");
    if (ImGui::BeginPopupModal("Preferences", &showPreferences, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetWindowSize(ImVec2(500, 400));
        
        // Tab bar for preference categories
        if (ImGui::BeginTabBar("PrefsTabs")) {
            // ===== General Tab =====
            if (ImGui::BeginTabItem("General")) {
                ImGui::Checkbox("Verbose Logging", &editor.debugConfig.verbose);
                ImGui::SliderFloat("Game Speed", &editor.gameSpeed, 0.1f, 3.0f, "%.1fx");
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "Engine Version: 1.0.0");
                ImGui::EndTabItem();
            }
            
            // ===== Rendering Tab =====
            if (ImGui::BeginTabItem("Rendering")) {
                ImGui::Checkbox("Show Grid", &editor.showGrid);
                ImGui::Checkbox("Show Gizmo", &editor.showGizmo);
                ImGui::Checkbox("Wireframe Mode", &editor.showWireframe);
                ImGui::Separator();
                ImGui::Text("Viewport Resolution: %dx%d", editor.viewportFB.width, editor.viewportFB.height);
                ImGui::EndTabItem();
            }
            
            // ===== Input Tab =====
            if (ImGui::BeginTabItem("Input")) {
                ImGui::Text("Camera Controls:");
                ImGui::BulletText("Right-click + drag: Orbit camera");
                ImGui::BulletText("WASD: Move camera");
                ImGui::BulletText("Q/E: Move down/up");
                ImGui::BulletText("Scroll: Zoom");
                ImGui::Separator();
                ImGui::Text("Gizmo Shortcuts:");
                ImGui::BulletText("W: Translate tool");
                ImGui::BulletText("E: Rotate tool");
                ImGui::BulletText("R: Scale tool");
                ImGui::BulletText("X: Toggle World/Local space");
                ImGui::EndTabItem();
            }
            
            // ===== UI Tab =====
            if (ImGui::BeginTabItem("UI")) {
                ImGui::Text("Panel Visibility:");
                ImGui::Checkbox("Outliner", &editor.uiState.showOutliner);
                ImGui::Checkbox("Details", &editor.uiState.showDetails);
                ImGui::Checkbox("Toolbox", &editor.uiState.showToolbox);
                ImGui::Checkbox("Console", &editor.uiState.showConsole);
                ImGui::Checkbox("Content Browser", &editor.uiState.showContentBrowser);
                ImGui::Checkbox("Game Mode", &editor.uiState.showGameMode);
                ImGui::Checkbox("Profiler", &editor.uiState.showProfiler);
                ImGui::EndTabItem();
            }
            
            ImGui::EndTabBar();
        }
        
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            showPreferences = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        
        ImGui::EndPopup();
    }
}

} // namespace UI
