#ifndef EDITOR_GIZMO_RENDERER_H
#define EDITOR_GIZMO_RENDERER_H

#include <glad/glad.h>
#include <utility>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "ecs/ECS.h"

// ============================================================================
// Gizmo Rendering (Translate/Rotate/Scale)
// ============================================================================
namespace GizmoRenderer {

enum class GizmoType { None, Translate, Rotate, Scale };
enum class SpaceType { Local, World };
enum class GizmoAxis { None, X, Y, Z };

// Draw gizmo at position
void Draw(const glm::vec3& position, float size, const glm::quat& rotation,
          const glm::mat4& view, const glm::mat4& projection, GLuint gizmoShaderProgram,
          GizmoType type);

// Draw a screen-space ADDITIVE OUTLINE halo around the gizmo handles (the
// "selected" highlight). Renders the gizmo geometry a second time with a bright
// additive tint, depth-test OFF + depth-mask OFF so the glow is visible behind
// scene geometry, and half-dilates the line verts in view space (+/- uThickness,
// two passes => a centered ribbon) so it works under the core profile (where wide
// glLineWidth is unreliable). Self-contained: compiles its own outline program
// on first use and fully restores the caller's GL state. Does NOT touch Draw()'s
// state save/restore (the gizmo depth tests rely on Draw() staying unchanged);
// only the editor caller invokes this.
void DrawOutline(const glm::vec3& position, float size, const glm::quat& rotation,
                 const glm::mat4& view, const glm::mat4& projection, GizmoType type);

// True iff the outline shader program compiled+linked in the current GL context.
// Forces the lazy compile on first call (same program DrawOutline uses). Exposed
// so tests can lock the inline `#version 430 core` outline GLSL green — there is
// no glslangValidator in this environment, so compile-time never inspects those
// strings; only a runtime GL compile (via DrawOutline's compileGizmoOutlineProgram)
// can fail them.
bool GizmoOutlineCompiled();

// Draw gizmo as a screen-space ImGui overlay (depth-test-free) over the viewport.
// API-agnostic: projects the same geometry as Draw() to screen space and emits
// ImGui lines via the background draw list. Used by the Vulkan editor path
// (the GL path uses Draw() into the viewport FBO from renderSceneOnly).
void DrawImGui(const glm::vec3& position, float size, const glm::quat& rotation,
               const glm::mat4& view, const glm::mat4& projection,
               int viewportX, int viewportY, int viewportW, int viewportH,
               GizmoType type);

// Set/get current gizmo type
void SetGizmoType(GizmoType type);
GizmoType GetGizmoType();

// Set/get space type
void SetSpaceType(SpaceType space);
SpaceType GetSpaceType();

// Toggle gizmo visibility
void Show(bool show);
bool IsVisible();

// ============================================================================
// Gizmo Interaction (Picking & Dragging)
// ============================================================================

// Check if mouse is hovering over a gizmo axis (returns which axis)
// screenX/Y are in viewport coordinates, viewportW/H are the viewport dimensions
GizmoAxis HitTest(const glm::vec3& gizmoPos, float gizmoSize, const glm::quat& gizmoRot,
                  const glm::mat4& view, const glm::mat4& projection,
                  int viewportX, int viewportY, int viewportW, int viewportH,
                  float mouseX, float mouseY);

// Start dragging a gizmo axis (call on mouse down)
void BeginDrag(GizmoAxis axis, const glm::vec3& gizmoPos, const glm::quat& gizmoRot,
               float mouseX, float mouseY);

// Update drag (call on mouse move while dragging)
// Returns the delta to apply to the entity's transform
glm::vec3 UpdateDrag(float mouseX, float mouseY, const glm::mat4& view,
                     const glm::mat4& projection, int viewportW, int viewportH);

// Rotate drag: returns the INCREMENTAL swept angle (degrees) around the drag
// axis since the previous call, so the caller accumulates
// rotation = angleAxis(radians(deg), axis) * rotation each frame. The first
// call of a drag seeds the reference and returns 0 (robust when the grab
// starts at the gizmo center).
float UpdateDragRotate(float mouseX, float mouseY, const glm::mat4& view,
                       const glm::mat4& projection, int viewportW, int viewportH);

// Scale drag: returns the multiplicative scale factor to apply this frame
// (1.0 = no change). Mouse travel along the axis screen direction scales
// the object up/down.
float UpdateDragScale(float mouseX, float mouseY, const glm::mat4& view,
                      const glm::mat4& projection, int viewportW, int viewportH);

// End dragging (call on mouse up)
void EndDrag();

// Check if currently dragging
bool IsDragging();

// Unproject a screen point to a world-space ray (origin, direction) for any
// camera. Exposed (was previously a file-local helper) so editor UI can share
// the gizmo's NDC->world-ray convention for viewport prop picking instead of
// duplicating the inverse-projection math. screenX/Y are viewport coords;
// viewportW/H are the viewport dimensions.
std::pair<glm::vec3, glm::vec3> ScreenToWorldRay(float screenX, float screenY,
                                                 const glm::mat4& view,
                                                 const glm::mat4& projection,
                                                 int viewportW, int viewportH);

// Get the currently dragged axis
GizmoAxis GetDragAxis();

// World-space direction of the axis being dragged (for rotate/scale math).
glm::vec3 GetDragAxisWorld();

} // namespace GizmoRenderer

#endif // EDITOR_GIZMO_RENDERER_H
