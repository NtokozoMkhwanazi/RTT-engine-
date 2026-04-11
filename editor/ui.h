#ifndef EDITOR_UI_H
#define EDITOR_UI_H

#include <imgui.h>
#include <glm/glm.hpp>
#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "gizmo_renderer.h"
#include <GLFW/glfw3.h>

// Forward declarations
struct UIState;
struct DebugConfig;
struct EditorState;

// Forward declare flyCamera
class flyCamera;

// ============================================================================
// UI Panel Rendering Functions
// ============================================================================
namespace UI {

// Menu bar
void RenderMenuBar(bool& showAbout, ecs::World& world, ecs::EntityID& selected,
                   bool& isPlaying, bool& wasPlaying, const std::string& currentSceneFile,
                   bool& shouldClose);

// Preferences dialog
void RenderPreferencesDialog(bool& showPreferences, EditorState& editor);

// Toolbar
void RenderToolbar(GizmoRenderer::GizmoType& gizmoType, GizmoRenderer::SpaceType& spaceType, bool& showGrid, 
                   bool& showGizmo, bool& showWireframe, ecs::World& world,
                   ecs::EntityID& selected);

// Left panel (Outliner/Details/Geo)
void RenderLeftPanel(int& activeTab, ecs::World& world, ecs::EntityID& selected,
                     const char* searchBuffer);

// Bottom panel (Content/Console/Profiler)
void RenderBottomPanel(int& activeTab, ecs::World& world, ecs::EntityID& selected,
                       float fps, const class flyCamera* camera);

// Console panel (standalone)
void RenderConsolePanel(ecs::World& world);

// Content Browser panel
void RenderContentBrowser(ecs::World& world);

// Component management panel
void RenderComponentPanel(ecs::World& world, ecs::EntityID selected);

// Game mode controls
void RenderGameModeControls(bool& isPlaying, bool& wasPlaying, float& gameSpeed, ecs::World& world);

// Viewport
void RenderViewport(ecs::EntityID& selected, flyCamera* camera, bool& isViewing,
                    glm::vec2& lastMousePos, GLuint viewportTexture, int windowW, int windowH,
                    GizmoRenderer::GizmoType gizmoType, GizmoRenderer::SpaceType spaceType, bool showWireframe,
                    bool showGrid, bool showGizmo, GLFWwindow* window, ImGuiIO& io);

// Status bar
void RenderStatusBar(ecs::World& world, ecs::EntityID& selected, float fps,
                     bool isPlaying, bool wasPlaying, int windowW);

// About dialog
void RenderAboutDialog(bool& showAbout);

// Transform section (for Details panel)
void RenderTransformSection(ecs::TransformComponent* t);

// Mesh section (for Details panel)
void RenderMeshSection(ecs::MeshComponent* m);

} // namespace UI

#endif // EDITOR_UI_H
