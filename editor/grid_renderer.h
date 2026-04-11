#ifndef EDITOR_GRID_RENDERER_H
#define EDITOR_GRID_RENDERER_H

#include <glad/glad.h>
#include <glm/glm.hpp>

// ============================================================================
// Grid Rendering
// ============================================================================
namespace GridRenderer {

// Initialize grid
void Init(int gridSize = 20, float spacing = 1.0f);

// Cleanup grid
void Cleanup();

// Draw grid
void Draw(const glm::mat4& view, const glm::mat4& projection, GLuint shaderProgram);

// Toggle grid visibility
void Show(bool show);
bool IsVisible();

} // namespace GridRenderer

#endif // EDITOR_GRID_RENDERER_H
