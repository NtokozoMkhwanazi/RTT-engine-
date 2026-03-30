#pragma once

/**
 * Debug Renderer - Simple OpenGL debug visualization
 * 
 * Draws basic shapes for debugging:
 * - Grid floor
 * - Colored boxes
 * - Axes
 * - Wireframe shapes
 */

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>

class DebugRenderer {
public:
    DebugRenderer();
    ~DebugRenderer();

    // Initialization
    bool initialize();
    void shutdown();

    // Drawing
    void beginFrame(const glm::mat4& view, const glm::mat4& projection);
    void endFrame();

    // Primitives
    void drawGrid(const glm::vec3& center, float size, int divisions, const glm::vec3& color);
    void drawBox(const glm::vec3& position, const glm::vec3& size, const glm::vec3& color, bool wireframe = false);
    void drawAxes(float size = 1.0f);
    void drawPoint(const glm::vec3& position, const glm::vec3& color, float size = 5.0f);
    void drawLine(const glm::vec3& start, const glm::vec3& end, const glm::vec3& color);

    // Settings
    void setViewMatrix(const glm::mat4& view) { m_viewMatrix = view; }
    void setProjectionMatrix(const glm::mat4& proj) { m_projectionMatrix = proj; }

private:
    struct Vertex {
        glm::vec3 position;
        glm::vec3 color;
    };

    GLuint m_VAO = 0;
    GLuint m_VBO = 0;
    GLuint m_shaderProgram = 0;
    
    glm::mat4 m_viewMatrix{1.0f};
    glm::mat4 m_projectionMatrix{1.0f};
    
    std::vector<Vertex> m_vertices;
    
    bool m_initialized = false;
    bool m_inFrame = false;

    // Helper functions
    void createShaderProgram();
    void uploadVertices();
    void drawArrays(GLenum mode);
};

// Global debug renderer instance
extern DebugRenderer g_debugRenderer;

// Convenience macros
#define DEBUG_DRAW_GRID(center, size, div, color) g_debugRenderer.drawGrid(center, size, div, color)
#define DEBUG_DRAW_BOX(pos, size, color) g_debugRenderer.drawBox(pos, size, color)
#define DEBUG_DRAW_AXES(size) g_debugRenderer.drawAxes(size)
#define DEBUG_DRAW_POINT(pos, color) g_debugRenderer.drawPoint(pos, color)
#define DEBUG_DRAW_LINE(start, end, color) g_debugRenderer.drawLine(start, end, color)
