#ifndef EDITOR_UI_H
#define EDITOR_UI_H

#include <imgui.h>
#include <glm/glm.hpp>
#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "gizmo_renderer.h"
#include "ui_cache.h"
#include <GLFW/glfw3.h>

// Forward declarations
struct EditorState;
class flyCamera;

namespace geo {
    class GeoAPI;
}

namespace UI {

// Rebuild cache only when dirty (not every frame)
void RebuildEntityCache(EntityCache& cache, ecs::World& world);

// ============================================================================
// Menu bar
// ============================================================================
void RenderMenuBar(bool& showAbout, ecs::World& world, ecs::EntityID& selected,
                   bool& isPlaying, bool& wasPlaying, const std::string& currentSceneFile,
                   bool& shouldClose);

// Preferences dialog
void RenderPreferencesDialog(bool& showPreferences, EditorState& editor);

// Toolbar (minimal, no heavy operations)
void RenderToolbar(GizmoRenderer::GizmoType& gizmoType, GizmoRenderer::SpaceType& spaceType, bool& showGrid,
                   bool& showGizmo, int& showWireframe);

// Left panel
void RenderLeftPanel(int& activeTab, ecs::World& world, ecs::EntityID& selected,
                     EntityCache& entityCache, const char* searchBuffer);

void RenderOutlinerPanel(ecs::EntityID& selected, EntityCache& entityCache, const char* searchBuffer);
void RenderDetailsPanel(ecs::World& world, ecs::EntityID& selected);

// Bottom panel
void RenderBottomPanel(int& activeTab, float fps, const flyCamera* camera, ecs::EntityID& selected);

void RenderConsolePanel();

// Viewport
void RenderViewport(ecs::EntityID& selected, flyCamera* camera, bool& isViewing,
                    glm::vec2& lastMousePos, GLuint viewportTexture, int windowW, int windowH,
                    GizmoRenderer::GizmoType gizmoType, GizmoRenderer::SpaceType spaceType, int showWireframe,
                    bool showGrid, bool showGizmo, GLFWwindow* window, ImGuiIO& io);

// Status bar (reads from cached data, no ECS iteration)
void RenderStatusBar(size_t entityCount, ecs::EntityID selected, float fps,
                     bool isPlaying, bool wasPlaying, int windowW);

// About dialog
void RenderAboutDialog(bool& showAbout);

// Transform section
void RenderTransformSection(ecs::TransformComponent* t);
void RenderMeshSection(ecs::MeshComponent* m);

} // namespace UI

#endif // EDITOR_UI_H
