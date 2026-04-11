#include "gizmo_renderer.h"
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <vector>

namespace GizmoRenderer {

static GizmoType g_gizmoType = GizmoType::Translate;
static SpaceType g_spaceType = SpaceType::World;
static bool g_show = true;

void Draw(const glm::vec3& position, float size, const glm::quat& rotation,
          const glm::mat4& view, const glm::mat4& projection, GLuint gizmoShaderProgram,
          GizmoType type) {
    if (!g_show || type == GizmoType::None) return;

    glDisable(GL_DEPTH_TEST);
    glUseProgram(gizmoShaderProgram);

    GLint viewLoc = glGetUniformLocation(gizmoShaderProgram, "uView");
    GLint projLoc = glGetUniformLocation(gizmoShaderProgram, "uProjection");
    GLint modelLoc = glGetUniformLocation(gizmoShaderProgram, "uModel");
    GLint colorLoc = glGetUniformLocation(gizmoShaderProgram, "uColor");

    glUniformMatrix4fv(viewLoc, 1, GL_FALSE, &view[0][0]);
    glUniformMatrix4fv(projLoc, 1, GL_FALSE, &projection[0][0]);

    glm::mat4 rotMat = glm::toMat4(rotation);

    struct AxisGizmo {
        glm::vec3 direction;
        glm::vec3 color;
        std::vector<float> verts;
    };

    float arrowLen = size * 0.8f;
    float headLen = size * 0.25f;
    float headWidth = size * 0.12f;

    AxisGizmo axes[3];

    // X axis (red)
    axes[0].direction = glm::vec3(1, 0, 0);
    axes[0].color = glm::vec3(1.0f, 0.2f, 0.2f);
    axes[0].verts = {
        0, 0, 0,  arrowLen, 0, 0,
        arrowLen, 0, 0,  arrowLen - headLen, headWidth, 0,
        arrowLen, 0, 0,  arrowLen - headLen, -headWidth, 0,
        arrowLen, 0, 0,  arrowLen - headLen, 0, headWidth,
        arrowLen, 0, 0,  arrowLen - headLen, 0, -headWidth,
    };

    // Y axis (green)
    axes[1].direction = glm::vec3(0, 1, 0);
    axes[1].color = glm::vec3(0.2f, 1.0f, 0.2f);
    axes[1].verts = {
        0, 0, 0,  0, arrowLen, 0,
        0, arrowLen, 0,  headWidth, arrowLen - headLen, 0,
        0, arrowLen, 0,  -headWidth, arrowLen - headLen, 0,
        0, arrowLen, 0,  0, arrowLen - headLen, headWidth,
        0, arrowLen, 0,  0, arrowLen - headLen, -headWidth,
    };

    // Z axis (blue)
    axes[2].direction = glm::vec3(0, 0, 1);
    axes[2].color = glm::vec3(0.2f, 0.2f, 1.0f);
    axes[2].verts = {
        0, 0, 0,  0, 0, arrowLen,
        0, 0, arrowLen,  headWidth, 0, arrowLen - headLen,
        0, 0, arrowLen,  -headWidth, 0, arrowLen - headLen,
        0, 0, arrowLen,  0, headWidth, arrowLen - headLen,
        0, 0, arrowLen,  0, -headWidth, arrowLen - headLen,
    };

    // Create VAO for gizmo
    GLuint gizmoVAO, gizmoVBO;
    glGenVertexArrays(1, &gizmoVAO);
    glGenBuffers(1, &gizmoVBO);
    glBindVertexArray(gizmoVAO);
    glBindBuffer(GL_ARRAY_BUFFER, gizmoVBO);
    glBufferData(GL_ARRAY_BUFFER, 256 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glEnableVertexAttribArray(0);

    // Draw each axis
    for (int a = 0; a < 3; a++) {
        std::vector<float> transformedVerts;
        transformedVerts.reserve(axes[a].verts.size());

        for (size_t i = 0; i < axes[a].verts.size(); i += 3) {
            glm::vec3 localPos(axes[a].verts[i], axes[a].verts[i+1], axes[a].verts[i+2]);
            glm::vec3 worldPos = position + glm::vec3(rotMat * glm::vec4(localPos, 1.0f));
            transformedVerts.push_back(worldPos.x);
            transformedVerts.push_back(worldPos.y);
            transformedVerts.push_back(worldPos.z);
        }

        glBufferSubData(GL_ARRAY_BUFFER, 0, transformedVerts.size() * sizeof(float), transformedVerts.data());

        glUniform3f(colorLoc, axes[a].color.r, axes[a].color.g, axes[a].color.b);
        glDrawArrays(GL_LINES, 0, transformedVerts.size() / 3);
    }

    glBindVertexArray(0);
    glDeleteVertexArrays(1, &gizmoVAO);
    glDeleteBuffers(1, &gizmoVBO);
    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
}

void SetGizmoType(GizmoType type) { g_gizmoType = type; }
GizmoType GetGizmoType() { return g_gizmoType; }
void SetSpaceType(SpaceType space) { g_spaceType = space; }
SpaceType GetSpaceType() { return g_spaceType; }
void Show(bool show) { g_show = show; }
bool IsVisible() { return g_show; }

} // namespace GizmoRenderer
