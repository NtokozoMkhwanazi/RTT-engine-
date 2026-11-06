#ifndef EDITOR_SHADER_MANAGER_H
#define EDITOR_SHADER_MANAGER_H

#include <glad/glad.h>
#include <glm/glm.hpp>

// ============================================================================
// Shader Management
// ============================================================================
namespace ShaderManager {

// Initialize shaders (main PBR shader + gizmo shader)
void InitShaders();

// Cleanup shaders
void CleanupShaders();

// Get shader program IDs
GLuint GetMainShaderProgram();
GLuint GetGizmoShaderProgram();
GLuint GetUnlitShaderProgram();
GLuint GetNormalsShaderProgram();

// Use shader programs
void UseMainShader();
void UseGizmoShader();

// Set uniform helpers
void SetMat4(GLuint program, const char* name, const glm::mat4& mat);
void SetVec3(GLuint program, const char* name, const glm::vec3& vec);

// Set a scalar float uniform (gizmo uHoverGlow / uIsOccludedPass).
void SetFloat(GLuint program, const char* name, float v);

} // namespace ShaderManager

#endif // EDITOR_SHADER_MANAGER_H
