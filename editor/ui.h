#ifndef EDITOR_UI_H
#define EDITOR_UI_H

#include <imgui.h>
#include <glm/glm.hpp>
#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "gizmo_renderer.h"
#include "ui_cache.h"
#include "editor_state.h"
#include "entity_manager.h"
#include <GLFW/glfw3.h>

// Forward declarations
struct EditorState;
class flyCamera;

namespace Editor {
    class Editor;
}

namespace geo {
    class GeoAPI;
}

namespace UI {

// Rebuild cache only when dirty (not every frame)
void RebuildEntityCache(EntityCache& cache, const ecs::World& world);

// ============================================================================
// Menu bar
// ============================================================================
void RenderMenuBar(Editor::Editor& editor, const std::string& currentSceneFile);

// Preferences dialog
void RenderPreferencesDialog(bool& showPreferences, Editor::Editor& editor);

// Toolbar (minimal, no heavy operations)
void RenderToolbar(Editor::Editor& editor);

// Left panel (Outliner / Layers / World)
void RenderLeftPanel(Editor::Editor& editor);

void RenderOutlinerPanel(ecs::EntityID& selected, EntityCache& entityCache, const char* searchBuffer);
void RenderDetailsPanel(ecs::World& world, ecs::EntityID& selected);
void RenderWorldSettingsPanel(Editor::Editor& editor);

// Right panel (Details)
void RenderRightPanel(Editor::Editor& editor);
void RenderModelSection(ecs::ModelComponent* model);

// Bottom panel (Content Browser / Output Log / Profiler)
void RenderBottomPanel(Editor::Editor& editor, float fps);

void RenderConsolePanel();
void RenderContentBrowserPanel();
void RenderProfilerPanel(Editor::Editor& editor, float fps);

// Viewport (Editor-based, hardened signature)
void RenderViewport(Editor::Editor& editor, GLuint viewportTexture,
                    int windowW, int windowH, GLFWwindow* window, ImGuiIO& io,
                    const glm::mat4& projection = glm::mat4(1.0f));

// Deprecated legacy signature - no-op stub that should be replaced by the Editor-based
// overload at every call site. Kept inline so existing translation units continue to
// compile until they migrate.
inline void RenderViewport(ecs::EntityID& selected, flyCamera* camera, bool& isViewing,
                           glm::vec2& lastMousePos, GLuint viewportTexture, int windowW, int windowH,
                           GizmoRenderer::GizmoType gizmoType, GizmoRenderer::SpaceType spaceType,
                           int showWireframe, bool showGrid, bool showGizmo, GLFWwindow* window,
                           ImGuiIO& io, const glm::mat4& projection = glm::mat4(1.0f)) {
    // Deprecated: delegate to Editor-based overload when a global editor exists.
    (void)selected; (void)camera; (void)isViewing; (void)lastMousePos;
    (void)gizmoType; (void)spaceType; (void)showWireframe; (void)showGrid; (void)showGizmo;
    (void)viewportTexture; (void)windowW; (void)windowH; (void)window; (void)io; (void)projection;
}

// Status bar (reads from cached data, no ECS iteration)
void RenderStatusBar(size_t entityCount, ecs::EntityID selected, float fps,
                     bool isPlaying, bool wasPlaying, int windowW, int windowH);

// About dialog
void RenderAboutDialog(bool& showAbout);

// Transform section
void RenderTransformSection(ecs::TransformComponent* t);
void RenderMeshSection(ecs::MeshComponent* m);

} // namespace UI

#endif // EDITOR_UI_H
