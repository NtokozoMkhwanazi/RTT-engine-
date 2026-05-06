#include "gizmo_renderer.h"
#include "editor_state.h"
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <algorithm>

namespace GizmoRenderer {

static GizmoType g_gizmoType = GizmoType::Translate;
static SpaceType g_spaceType = SpaceType::World;

// Interaction state
static bool g_dragging = false;
static GizmoAxis g_dragAxis = GizmoAxis::None;
static glm::vec3 g_dragStartPos = glm::vec3(0.0f);
static glm::quat g_dragStartRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
static glm::vec2 g_dragStartMouse = glm::vec2(0.0f);
static glm::vec3 g_dragAxisWorld = glm::vec3(0.0f);

// Helper: project world point to screen space
static glm::vec2 WorldToScreen(const glm::vec3& worldPos, const glm::mat4& view,
                                const glm::mat4& projection, int vpX, int vpY, int vpW, int vpH) {
    glm::vec4 clip = projection * view * glm::vec4(worldPos, 1.0f);
    if (clip.w == 0.0f) return glm::vec2(-1.0f);
    glm::vec3 ndc = glm::vec3(clip) / clip.w;
    return glm::vec2(
        vpX + (ndc.x * 0.5f + 0.5f) * vpW,
        vpY + (1.0f - (ndc.y * 0.5f + 0.5f)) * vpH
    );
}

// Helper: unproject screen point to world ray (origin + direction)
static std::pair<glm::vec3, glm::vec3> ScreenToWorldRay(float screenX, float screenY,
    const glm::mat4& view, const glm::mat4& projection, int vpW, int vpH) {
    // Convert screen coords to NDC
    float ndcX = (screenX / vpW) * 2.0f - 1.0f;
    float ndcY = 1.0f - (screenY / vpH) * 2.0f;

    glm::vec4 rayClip = glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::mat4 invVP = glm::inverse(projection * view);
    glm::vec4 rayWorld = invVP * rayClip;
    glm::vec3 rayDir = glm::normalize(glm::vec3(rayWorld));

    // Ray origin is camera position (extracted from view matrix)
    glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);
    return {camPos, rayDir};
}

// Helper: distance from point to line segment
static float PointToLineDistance(const glm::vec3& point, const glm::vec3& lineStart, const glm::vec3& lineEnd) {
    glm::vec3 lineDir = lineEnd - lineStart;
    float lineLen = glm::length(lineDir);
    if (lineLen < 0.0001f) return glm::distance(point, lineStart);
    lineDir /= lineLen;

    glm::vec3 toPoint = point - lineStart;
    float t = glm::dot(toPoint, lineDir);
    t = glm::clamp(t, 0.0f, lineLen);

    glm::vec3 closest = lineStart + lineDir * t;
    return glm::distance(point, closest);
}

void Draw(const glm::vec3& position, float size, const glm::quat& rotation,
          const glm::mat4& view, const glm::mat4& projection, GLuint gizmoShaderProgram,
          GizmoType type) {
    if (!g_editor.showGizmo || type == GizmoType::None) return;

    glDisable(GL_DEPTH_TEST);
    glUseProgram(gizmoShaderProgram);

    GLint viewLoc = glGetUniformLocation(gizmoShaderProgram, "uView");
    GLint projLoc = glGetUniformLocation(gizmoShaderProgram, "uProjection");
    GLint modelLoc = glGetUniformLocation(gizmoShaderProgram, "uModel");
    GLint colorLoc = glGetUniformLocation(gizmoShaderProgram, "uColor");

    glUniformMatrix4fv(viewLoc, 1, GL_FALSE, &view[0][0]);
    glUniformMatrix4fv(projLoc, 1, GL_FALSE, &projection[0][0]);

    // Model matrix is identity since vertices are already in world space
    glm::mat4 identity = glm::mat4(1.0f);
    glUniformMatrix4fv(modelLoc, 1, GL_FALSE, &identity[0][0]);

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
void Show(bool show) { g_editor.showGizmo = show; }
bool IsVisible() { return g_editor.showGizmo; }

// ============================================================================
// Gizmo Interaction Implementation
// ============================================================================

GizmoAxis HitTest(const glm::vec3& gizmoPos, float gizmoSize, const glm::quat& gizmoRot,
                  const glm::mat4& view, const glm::mat4& projection,
                  int vpX, int vpY, int vpW, int vpH,
                  float mouseX, float mouseY) {
    if (g_gizmoType == GizmoType::None) return GizmoAxis::None;

    glm::mat4 rotMat = glm::toMat4(gizmoRot);
    float axisLen = gizmoSize * 0.8f;

    // Define axis endpoints in world space
    struct AxisLine {
        GizmoAxis axis;
        glm::vec3 start;
        glm::vec3 end;
    };

    AxisLine axes[3] = {
        { GizmoAxis::X, gizmoPos, gizmoPos + glm::vec3(rotMat * glm::vec4(axisLen, 0, 0, 0)) },
        { GizmoAxis::Y, gizmoPos, gizmoPos + glm::vec3(rotMat * glm::vec4(0, axisLen, 0, 0)) },
        { GizmoAxis::Z, gizmoPos, gizmoPos + glm::vec3(rotMat * glm::vec4(0, 0, axisLen, 0)) },
    };

    // Project mouse to world ray (used for future 3D picking)
    auto [rayOrigin, rayDir] = ScreenToWorldRay(mouseX, mouseY, view, projection, vpW, vpH);
    (void)rayOrigin; (void)rayDir;

    // Find closest axis
    float minDist = 15.0f; // Pixel threshold
    GizmoAxis closest = GizmoAxis::None;

    for (const auto& ax : axes) {
        glm::vec2 screenStart = WorldToScreen(ax.start, view, projection, vpX, vpY, vpW, vpH);
        glm::vec2 screenEnd = WorldToScreen(ax.end, view, projection, vpX, vpY, vpW, vpH);

        // Distance from mouse point to screen-space line segment
        float dist = PointToLineDistance(glm::vec3(mouseX, mouseY, 0),
                                          glm::vec3(screenStart, 0),
                                          glm::vec3(screenEnd, 0));
        
        static int axisDebug = 0;
        if (axisDebug++ % 60 == 0) {
            std::cout << "[HITTEST] axis=" << (int)ax.axis 
                      << " screenStart=(" << screenStart.x << "," << screenStart.y << ")"
                      << " screenEnd=(" << screenEnd.x << "," << screenEnd.y << ")"
                      << " mouse=(" << mouseX << "," << mouseY << ")"
                      << " dist=" << dist << "\n";
        }
        
        if (dist < minDist) {
            minDist = dist;
            closest = ax.axis;
        }
    }

    return closest;
}

void BeginDrag(GizmoAxis axis, const glm::vec3& gizmoPos, const glm::quat& gizmoRot,
               float mouseX, float mouseY) {
    g_dragging = true;
    g_dragAxis = axis;
    g_dragStartPos = gizmoPos;
    g_dragStartRot = gizmoRot;
    g_dragStartMouse = glm::vec2(mouseX, mouseY);

    // Compute world axis direction
    glm::mat4 rotMat = glm::toMat4(gizmoRot);
    switch (axis) {
        case GizmoAxis::X: g_dragAxisWorld = glm::normalize(glm::vec3(rotMat * glm::vec4(1, 0, 0, 0))); break;
        case GizmoAxis::Y: g_dragAxisWorld = glm::normalize(glm::vec3(rotMat * glm::vec4(0, 1, 0, 0))); break;
        case GizmoAxis::Z: g_dragAxisWorld = glm::normalize(glm::vec3(rotMat * glm::vec4(0, 0, 1, 0))); break;
        default: g_dragAxisWorld = glm::vec3(0, 1, 0); break;
    }
}

glm::vec3 UpdateDrag(float mouseX, float mouseY, const glm::mat4& view,
                     const glm::mat4& projection, int vpW, int vpH) {
    if (!g_dragging || g_dragAxis == GizmoAxis::None) return glm::vec3(0.0f);

    // Get world ray for current mouse position
    auto [rayOrigin, rayDir] = ScreenToWorldRay(mouseX, mouseY, view, projection, vpW, vpH);
    // Get world ray for start mouse position
    auto [startOrigin, startDir] = ScreenToWorldRay(g_dragStartMouse.x, g_dragStartMouse.y, view, projection, vpW, vpH);

    // Project both rays onto a plane perpendicular to the drag axis
    // Then find the intersection points and compute the delta
    glm::vec3 axis = g_dragAxisWorld;

    // Plane normal = axis, plane passes through gizmo position
    // Intersect ray with plane: t = dot(planePoint - rayOrigin, planeNormal) / dot(rayDir, planeNormal)
    float denomStart = glm::dot(startDir, axis);
    float denomCurr = glm::dot(rayDir, axis);

    if (std::abs(denomStart) < 0.0001f || std::abs(denomCurr) < 0.0001f) {
        return glm::vec3(0.0f); // Ray parallel to plane
    }

    float tStart = glm::dot(g_dragStartPos - startOrigin, axis) / denomStart;
    float tCurr = glm::dot(g_dragStartPos - rayOrigin, axis) / denomCurr;

    glm::vec3 worldStart = startOrigin + startDir * tStart;
    glm::vec3 worldCurr = rayOrigin + rayDir * tCurr;

    glm::vec3 delta = worldCurr - worldStart;

    // Project delta onto axis
    float axisDelta = glm::dot(delta, axis);
    return axis * axisDelta;
}

void EndDrag() {
    g_dragging = false;
    g_dragAxis = GizmoAxis::None;
    g_dragAxisWorld = glm::vec3(0.0f);
}

bool IsDragging() { return g_dragging; }
GizmoAxis GetDragAxis() { return g_dragAxis; }

} // namespace GizmoRenderer
