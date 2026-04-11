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

} // namespace GizmoRenderer

#endif // EDITOR_GIZMO_RENDERER_H
