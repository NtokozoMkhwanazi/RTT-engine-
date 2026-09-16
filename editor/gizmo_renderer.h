#ifndef EDITOR_GIZMO_RENDERER_H
#define EDITOR_GIZMO_RENDERER_H

#include <glad/glad.h>
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

// Query current drag state
bool IsDragging();

// Get the world-space direction of the axis being dragged
// (used by the caller to apply rotation deltas)
glm::vec3 GetDragAxisWorld();

// Check if currently dragging
bool IsDragging();

// Get the currently dragged axis
GizmoAxis GetDragAxis();

// World-space direction of the axis being dragged (for rotate/scale math).
glm::vec3 GetDragAxisWorld();

} // namespace GizmoRenderer

#endif // EDITOR_GIZMO_RENDERER_H
