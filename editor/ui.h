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
    class PlayModeController;
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

// Scene file dialogs (Save As / Open) + Import Model + Preferences modal.
// Call once per frame inside the ImGui frame; safe to call always.
void RenderSceneDialogs(Editor::Editor& editor);

// Toolbar (minimal, no heavy operations)
void RenderToolbar(Editor::Editor& editor);

// Left panel (Outliner / Layers / World)
void RenderLeftPanel(Editor::Editor& editor);

void RenderOutlinerPanel(ecs::EntityID& selected, EntityCache& entityCache, const char* searchBuffer);
void RenderDetailsPanel(ecs::World& world, ecs::EntityID& selected);
void RenderWorldSettingsPanel(Editor::Editor& editor);

// Right panel (Details). When in play mode and a PlayModeController is
// supplied, the play-mode debug section (character state, speed, active clip,
// motion-matching diagnostics, camera mode) is rendered at the top of the
// panel instead of as a floating overlay on the viewport - keeping the 3D
// view clear so the character stays visible.
void RenderRightPanel(Editor::Editor& editor,
                      const Editor::PlayModeController* play = nullptr,
                      const char* camMode = nullptr);
void RenderModelSection(ecs::ModelComponent* model);

// Bottom panel (Content Browser / Output Log / Profiler)
void RenderBottomPanel(Editor::Editor& editor, float fps);

void RenderConsolePanel();
void RenderContentBrowserPanel();
void RenderProfilerPanel(Editor::Editor& editor, float fps);

// Viewport (Editor-based, hardened signature). io is nullable: without an
// ImGui context (or explicit ImGuiIO*) the function early-outs safely.
// view is the current view matrix used to render the scene (so grid + gizmo
// stay aligned even when the engine drives a follow camera); camModeName is
// shown in the bottom-left overlay when non-null.
//
// swapchainBacked: the 3D scene is rendered INTO the swapchain (Vulkan path)
// instead of an offscreen FBO texture, so the viewport panel is transparent
// (no WindowBg, no FBO image) and the swapchain content shows through below
// the header strip. viewportTexture is ignored in that mode.
void RenderViewport(Editor::Editor& editor, GLuint viewportTexture,
                    int windowW, int windowH, GLFWwindow* window, ImGuiIO* io,
                    const glm::mat4& projection = glm::mat4(1.0f),
                    const glm::mat4& view = glm::mat4(1.0f),
                    const char* camModeName = nullptr,
                    const glm::vec3* cameraPos = nullptr,
                    const glm::vec3* cameraTarget = nullptr,
                    bool swapchainBacked = false);

// True when the mouse cursor is inside the viewport's 3D content area. Lets
// the engine hand the camera (orbit / zoom / fly) ownership of the mouse +
// keyboard instead of ImGui swallowing it. Popups/menus on top still win.
bool IsViewport3DHovered();

// Rect of the FBO image inside the viewport (below the header), in window
// coordinates, as captured by the last RenderViewport call. Returns false
// until the viewport has rendered at least once. The engine uses this to map
// mouse input for gizmo dragging + viewport picking.
bool GetViewportContentRect(float& x, float& y, float& w, float& h);

// Play-mode debug section - renders INTO the currently open ImGui window
// (the Details panel) rather than as a floating overlay on the viewport, so
// the 3D view stays unobstructed. Draws character state, speed, active clip
// and pose diagnostics under a "Play Mode" header. Safe to call without an
// ImGui context (early-outs) and without a loaded character. camMode
// (optional) shows the active play camera mode + control hints.
void RenderPlayModeDebug(const Editor::PlayModeController& ctrl, const char* camMode = nullptr);

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
