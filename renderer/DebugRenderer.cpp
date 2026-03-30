#include "DebugRenderer.h"
#include <iostream>

// ============================================================================
// Shader Sources
// ============================================================================

static const char* debugVertexShader = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aColor;

out vec3 vColor;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

void main() {
    vColor = aColor;
    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
}
)";

static const char* debugFragmentShader = R"(
#version 330 core
in vec3 vColor;
out vec4 fragColor;

void main() {
    fragColor = vec4(vColor, 1.0);
}
)";

// ============================================================================
// Global Instance
// ============================================================================

DebugRenderer g_debugRenderer;

// ============================================================================
// DebugRenderer Implementation
// ============================================================================

DebugRenderer::DebugRenderer() {}

DebugRenderer::~DebugRenderer() {
    shutdown();
}

bool DebugRenderer::initialize() {
    if (m_initialized) return true;

    createShaderProgram();
    
    // Create VAO and VBO
    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    
    // Position attribute
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(0);
    
    // Color attribute
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, color));
    glEnableVertexAttribArray(1);
    
    glBindVertexArray(0);
    
    m_initialized = true;
    std::cout << "[DebugRenderer] Initialized\n";
    
    return true;
}

void DebugRenderer::shutdown() {
    if (m_VAO != 0) glDeleteVertexArrays(1, &m_VAO);
    if (m_VBO != 0) glDeleteBuffers(1, &m_VBO);
    if (m_shaderProgram != 0) glDeleteProgram(m_shaderProgram);
    
    m_initialized = false;
}

void DebugRenderer::createShaderProgram() {
    // Compile vertex shader
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &debugVertexShader, nullptr);
    glCompileShader(vertexShader);
    
    // Check for compilation errors
    GLint success;
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(vertexShader, 512, nullptr, infoLog);
        std::cerr << "[DebugRenderer] Vertex shader compilation failed: " << infoLog << "\n";
        return;
    }
    
    // Compile fragment shader
    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &debugFragmentShader, nullptr);
    glCompileShader(fragmentShader);
    
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(fragmentShader, 512, nullptr, infoLog);
        std::cerr << "[DebugRenderer] Fragment shader compilation failed: " << infoLog << "\n";
        return;
    }
    
    // Link program
    m_shaderProgram = glCreateProgram();
    glAttachShader(m_shaderProgram, vertexShader);
    glAttachShader(m_shaderProgram, fragmentShader);
    glLinkProgram(m_shaderProgram);
    
    glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(m_shaderProgram, 512, nullptr, infoLog);
        std::cerr << "[DebugRenderer] Shader program linking failed: " << infoLog << "\n";
        return;
    }
    
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
}

void DebugRenderer::beginFrame(const glm::mat4& view, const glm::mat4& projection) {
    if (!m_initialized) return;
    
    m_viewMatrix = view;
    m_projectionMatrix = projection;
    m_vertices.clear();
    m_inFrame = true;
}

void DebugRenderer::endFrame() {
    if (!m_initialized || !m_inFrame) return;
    
    if (m_vertices.empty()) {
        m_inFrame = false;
        return;
    }
    
    // Upload vertices
    uploadVertices();
    
    // Draw
    glUseProgram(m_shaderProgram);
    
    GLint viewLoc = glGetUniformLocation(m_shaderProgram, "uView");
    GLint projLoc = glGetUniformLocation(m_shaderProgram, "uProjection");
    GLint modelLoc = glGetUniformLocation(m_shaderProgram, "uModel");
    
    glUniformMatrix4fv(viewLoc, 1, GL_FALSE, &m_viewMatrix[0][0]);
    glUniformMatrix4fv(projLoc, 1, GL_FALSE, &m_projectionMatrix[0][0]);
    
    glm::mat4 model = glm::mat4(1.0f);
    glUniformMatrix4fv(modelLoc, 1, GL_FALSE, &model[0][0]);
    
    glBindVertexArray(m_VAO);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(m_vertices.size()));
    glBindVertexArray(0);
    
    glUseProgram(0);
    
    m_vertices.clear();
    m_inFrame = false;
}

void DebugRenderer::uploadVertices() {
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, m_vertices.size() * sizeof(Vertex), 
                 m_vertices.data(), GL_DYNAMIC_DRAW);
}

void DebugRenderer::drawGrid(const glm::vec3& center, float size, int divisions, const glm::vec3& color) {
    if (!m_inFrame) return;
    
    float halfSize = size * 0.5f;
    float step = size / static_cast<float>(divisions);
    
    // Draw lines along X axis
    for (int i = 0; i <= divisions; i++) {
        float z = center.z - halfSize + i * step;
        
        // Color based on position (darker for major lines)
        glm::vec3 lineColor = (i % 5 == 0) ? color : color * 0.5f;
        
        m_vertices.push_back({{center.x - halfSize, center.y, z}, lineColor});
        m_vertices.push_back({{center.x + halfSize, center.y, z}, lineColor});
    }
    
    // Draw lines along Z axis
    for (int i = 0; i <= divisions; i++) {
        float x = center.x - halfSize + i * step;
        
        glm::vec3 lineColor = (i % 5 == 0) ? color : color * 0.5f;
        
        m_vertices.push_back({{x, center.y, center.z - halfSize}, lineColor});
        m_vertices.push_back({{x, center.y, center.z + halfSize}, lineColor});
    }
}

void DebugRenderer::drawBox(const glm::vec3& position, const glm::vec3& size, const glm::vec3& color, bool wireframe) {
    if (!m_inFrame) return;
    
    glm::vec3 halfSize = size * 0.5f;
    glm::vec3 min = position - halfSize;
    glm::vec3 max = position + halfSize;
    
    // 12 edges of the box
    // Bottom face
    m_vertices.push_back({{min.x, min.y, min.z}, color});
    m_vertices.push_back({{max.x, min.y, min.z}, color});
    
    m_vertices.push_back({{max.x, min.y, min.z}, color});
    m_vertices.push_back({{max.x, min.y, max.z}, color});
    
    m_vertices.push_back({{max.x, min.y, max.z}, color});
    m_vertices.push_back({{min.x, min.y, max.z}, color});
    
    m_vertices.push_back({{min.x, min.y, max.z}, color});
    m_vertices.push_back({{min.x, min.y, min.z}, color});
    
    // Top face
    m_vertices.push_back({{min.x, max.y, min.z}, color});
    m_vertices.push_back({{max.x, max.y, min.z}, color});
    
    m_vertices.push_back({{max.x, max.y, min.z}, color});
    m_vertices.push_back({{max.x, max.y, max.z}, color});
    
    m_vertices.push_back({{max.x, max.y, max.z}, color});
    m_vertices.push_back({{min.x, max.y, max.z}, color});
    
    m_vertices.push_back({{min.x, max.y, max.z}, color});
    m_vertices.push_back({{min.x, max.y, min.z}, color});
    
    // Vertical edges
    m_vertices.push_back({{min.x, min.y, min.z}, color});
    m_vertices.push_back({{min.x, max.y, min.z}, color});
    
    m_vertices.push_back({{max.x, min.y, min.z}, color});
    m_vertices.push_back({{max.x, max.y, min.z}, color});
    
    m_vertices.push_back({{max.x, min.y, max.z}, color});
    m_vertices.push_back({{max.x, max.y, max.z}, color});
    
    m_vertices.push_back({{min.x, min.y, max.z}, color});
    m_vertices.push_back({{min.x, max.y, max.z}, color});
}

void DebugRenderer::drawAxes(float size) {
    if (!m_inFrame) return;
    
    // X axis - red
    m_vertices.push_back({{0.0f, 0.0f, 0.0f}, glm::vec3(1.0f, 0.0f, 0.0f)});
    m_vertices.push_back({{size, 0.0f, 0.0f}, glm::vec3(1.0f, 0.0f, 0.0f)});
    
    // Y axis - green
    m_vertices.push_back({{0.0f, 0.0f, 0.0f}, glm::vec3(0.0f, 1.0f, 0.0f)});
    m_vertices.push_back({{0.0f, size, 0.0f}, glm::vec3(0.0f, 1.0f, 0.0f)});
    
    // Z axis - blue
    m_vertices.push_back({{0.0f, 0.0f, 0.0f}, glm::vec3(0.0f, 0.0f, 1.0f)});
    m_vertices.push_back({{0.0f, 0.0f, size}, glm::vec3(0.0f, 0.0f, 1.0f)});
}

void DebugRenderer::drawPoint(const glm::vec3& position, const glm::vec3& color, float size) {
    if (!m_inFrame) return;
    
    // Draw a small box around the point
    drawBox(position, glm::vec3(size), color);
}

void DebugRenderer::drawLine(const glm::vec3& start, const glm::vec3& end, const glm::vec3& color) {
    if (!m_inFrame) return;
    
    m_vertices.push_back({start, color});
    m_vertices.push_back({end, color});
}
