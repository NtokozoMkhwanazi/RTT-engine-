#pragma once

/**
 * GeospatialPointRenderer - Phase 4: Digital Twin Visualization
 *
 * Renders geospatial data as points in the 3D viewport:
 * - GPS tracking positions
 * - Historical trajectory points
 * - Predicted trajectory points
 * - Entity markers
 *
 * Designed for the digital twin pipeline - clean, GPU-efficient,
 * no dependencies on primitive meshes.
 */

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <string>
#include <iostream>
#include "../ecs/components/GeospatialComponent.h"
#include "../geospatial/GeospatialConverter.h"
#include "../geospatial/PredictiveModel.h"
#include "../geospatial/TimeSeriesDB.h"
#include "../editor/gl_context_lifecycle.h"

namespace ecs {

struct GeoPoint {
    glm::vec3 position;
    glm::vec3 color;
    float size;
    float alpha;
};

class GeospatialPointRenderer {
public:
    GeospatialPointRenderer() : m_VAO(0), m_VBO(0), m_shaderProgram(0) {}

    ~GeospatialPointRenderer() {
        // Static destruction runs after glfwTerminate — never issue GL calls on
        // a dead context. Zero the handles so teardown stays idempotent.
        if (glctx::isAlive()) {
            if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
            if (m_VBO) glDeleteBuffers(1, &m_VBO);
            if (m_shaderProgram) glDeleteProgram(m_shaderProgram);
        }
        m_VAO = m_VBO = m_shaderProgram = 0;
    }

    void initialize() {
        createShader();

        glGenVertexArrays(1, &m_VAO);
        glGenBuffers(1, &m_VBO);

        glBindVertexArray(m_VAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_VBO);

        // Position (3 floats)
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        // Color (3 floats)
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        // Size (1 float)
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);
        // Alpha (1 float)
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(7 * sizeof(float)));
        glEnableVertexAttribArray(3);

        glBindVertexArray(0);

        std::cout << "[GeospatialPointRenderer] Initialized (VAO=" << m_VAO << ")\n";
    }

    void render(const std::vector<GeoPoint>& points,
                const glm::mat4& view,
                const glm::mat4& projection) {
        if (points.empty() || !m_shaderProgram) return;

        std::vector<float> vertexData;
        vertexData.reserve(points.size() * 8);

        for (const auto& p : points) {
            vertexData.push_back(p.position.x);
            vertexData.push_back(p.position.y);
            vertexData.push_back(p.position.z);
            vertexData.push_back(p.color.r);
            vertexData.push_back(p.color.g);
            vertexData.push_back(p.color.b);
            vertexData.push_back(p.size);
            vertexData.push_back(p.alpha);
        }

        glUseProgram(m_shaderProgram);

        GLint viewLoc = glGetUniformLocation(m_shaderProgram, "view");
        GLint projLoc = glGetUniformLocation(m_shaderProgram, "projection");
        if (viewLoc >= 0) glUniformMatrix4fv(viewLoc, 1, GL_FALSE, &view[0][0]);
        if (projLoc >= 0) glUniformMatrix4fv(projLoc, 1, GL_FALSE, &projection[0][0]);

        glBindVertexArray(m_VAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
        glBufferData(GL_ARRAY_BUFFER, vertexData.size() * sizeof(float), vertexData.data(), GL_DYNAMIC_DRAW);

        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_PROGRAM_POINT_SIZE);

        glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(points.size()));

        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_PROGRAM_POINT_SIZE);

        glBindVertexArray(0);
        glUseProgram(0);
    }

    void renderTrajectoryLines(const std::vector<glm::vec3>& positions,
                                const glm::vec3& color,
                                const glm::mat4& view,
                                const glm::mat4& projection) {
        if (positions.size() < 2) return;

        std::vector<float> lineData;
        lineData.reserve((positions.size() - 1) * 2 * 6);

        for (size_t i = 0; i < positions.size() - 1; i++) {
            lineData.push_back(positions[i].x);
            lineData.push_back(positions[i].y);
            lineData.push_back(positions[i].z);
            lineData.push_back(color.r);
            lineData.push_back(color.g);
            lineData.push_back(color.b);

            lineData.push_back(positions[i + 1].x);
            lineData.push_back(positions[i + 1].y);
            lineData.push_back(positions[i + 1].z);
            lineData.push_back(color.r);
            lineData.push_back(color.g);
            lineData.push_back(color.b);
        }

        GLuint lineVAO, lineVBO;
        glGenVertexArrays(1, &lineVAO);
        glGenBuffers(1, &lineVBO);

        glBindVertexArray(lineVAO);
        glBindBuffer(GL_ARRAY_BUFFER, lineVBO);
        glBufferData(GL_ARRAY_BUFFER, lineData.size() * sizeof(float), lineData.data(), GL_DYNAMIC_DRAW);

        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glUseProgram(m_shaderProgram);

        GLint viewLoc = glGetUniformLocation(m_shaderProgram, "view");
        GLint projLoc = glGetUniformLocation(m_shaderProgram, "projection");
        if (viewLoc >= 0) glUniformMatrix4fv(viewLoc, 1, GL_FALSE, &view[0][0]);
        if (projLoc >= 0) glUniformMatrix4fv(projLoc, 1, GL_FALSE, &projection[0][0]);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(lineData.size() / 6));

        glDisable(GL_BLEND);

        glBindVertexArray(0);
        glDeleteVertexArrays(1, &lineVAO);
        glDeleteBuffers(1, &lineVBO);
        glUseProgram(0);
    }

private:
    GLuint m_VAO;
    GLuint m_VBO;
    GLuint m_shaderProgram;

    void createShader() {
        const char* vs = R"(
            #version 330 core
            layout(location = 0) in vec3 aPos;
            layout(location = 1) in vec3 aColor;
            layout(location = 2) in float aSize;
            layout(location = 3) in float aAlpha;

            uniform mat4 view;
            uniform mat4 projection;

            out vec3 vColor;
            out float vAlpha;

            void main() {
                vColor = aColor;
                vAlpha = aAlpha;
                gl_Position = projection * view * vec4(aPos, 1.0);
                gl_PointSize = aSize;
            }
        )";

        const char* fs = R"(
            #version 330 core
            in vec3 vColor;
            in float vAlpha;
            out vec4 FragColor;

            void main() {
                // Circular point shape
                vec2 center = gl_PointCoord - vec2(0.5);
                float dist = length(center);
                if (dist > 0.5) discard;

                // Soft edge
                float alpha = smoothstep(0.5, 0.3, dist) * vAlpha;
                FragColor = vec4(vColor, alpha);
            }
        )";

        GLuint vShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vShader, 1, &vs, nullptr);
        glCompileShader(vShader);

        GLint success;
        glGetShaderiv(vShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char info[512];
            glGetShaderInfoLog(vShader, 512, nullptr, info);
            std::cerr << "[GeospatialPointRenderer] Vertex shader error: " << info << "\n";
        }

        GLuint fShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fShader, 1, &fs, nullptr);
        glCompileShader(fShader);

        glGetShaderiv(fShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char info[512];
            glGetShaderInfoLog(fShader, 512, nullptr, info);
            std::cerr << "[GeospatialPointRenderer] Fragment shader error: " << info << "\n";
        }

        m_shaderProgram = glCreateProgram();
        glAttachShader(m_shaderProgram, vShader);
        glAttachShader(m_shaderProgram, fShader);
        glLinkProgram(m_shaderProgram);

        glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, &success);
        if (!success) {
            char info[512];
            glGetProgramInfoLog(m_shaderProgram, 512, nullptr, info);
            std::cerr << "[GeospatialPointRenderer] Program link error: " << info << "\n";
        }

        glDeleteShader(vShader);
        glDeleteShader(fShader);
    }
};

} // namespace ecs
