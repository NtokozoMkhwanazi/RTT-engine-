#include "gizmo_renderer.h"
#include "editor_state.h"
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

// Rotate drag state: previous ray/plane intersection point so each call
// returns the INCREMENTAL swept angle (robust even when the drag starts right
// on the gizmo center, where a total-angle reference vector is degenerate).
static glm::vec3 g_lastRotPlanePoint = glm::vec3(0.0f);
static bool g_hasLastRotPoint = false;

// Helper: project world point to screen space (y-down window coords)
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
// Unprojects BOTH near and far planes: a single NDC point at z=-1 is a
// near-plane point, and normalize(worldPoint) is only the correct direction
// when the camera sits at the world origin. This version works for any camera.
static std::pair<glm::vec3, glm::vec3> ScreenToWorldRay(float screenX, float screenY,
    const glm::mat4& view, const glm::mat4& projection, int vpW, int vpH) {
    // Convert screen coords to NDC (y-down screen -> y-up NDC)
    const float ndcX = (screenX / vpW) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (screenY / vpH) * 2.0f;

    const glm::mat4 invVP = glm::inverse(projection * view);
    const glm::vec4 nearClip(ndcX, ndcY, -1.0f, 1.0f);
    const glm::vec4 farClip(ndcX, ndcY, 1.0f, 1.0f);
    const glm::vec4 nearWorld = invVP * nearClip;
    const glm::vec4 farWorld = invVP * farClip;
    if (nearWorld.w == 0.0f || farWorld.w == 0.0f) {
        return {glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    }

    const glm::vec3 nearP = glm::vec3(nearWorld) / nearWorld.w;
    const glm::vec3 farP = glm::vec3(farWorld) / farWorld.w;
    const glm::vec3 rayDir = glm::normalize(farP - nearP);

    // Ray origin is camera position (extracted from view matrix)
    const glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);
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
    if (!g_editor.showGizmo() || type == GizmoType::None) return;

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

    const glm::mat4 rotMat = glm::toMat4(rotation);

    // Per-axis definition: local direction, base color, highlight color.
    struct AxisDef {
        glm::vec3 dir;
        glm::vec3 color;
        glm::vec3 highlight;
    };
    const AxisDef axes[3] = {
        { glm::vec3(1, 0, 0), glm::vec3(0.85f, 0.25f, 0.22f), glm::vec3(1.0f, 0.80f, 0.30f) },
        { glm::vec3(0, 1, 0), glm::vec3(0.22f, 0.80f, 0.25f), glm::vec3(0.85f, 1.00f, 0.35f) },
        { glm::vec3(0, 0, 1), glm::vec3(0.25f, 0.40f, 0.90f), glm::vec3(0.60f, 0.85f, 1.00f) },
    };

    const GizmoAxis activeAxis = g_dragging ? g_dragAxis : g_editor.hoveredAxis();
    const float len = size * 0.8f;

    // World-space line vertices per axis (3 floats per vertex, pairs = lines).
    std::array<std::vector<float>, 3> axisVerts;

    auto world = [&](const glm::vec3& local) -> glm::vec3 {
        return position + glm::vec3(rotMat * glm::vec4(local, 1.0f));
    };
    auto line = [&](const glm::vec3& a, const glm::vec3& b, int axisIdx) {
        const glm::vec3 wa = world(a);
        const glm::vec3 wb = world(b);
        std::vector<float>& v = axisVerts[axisIdx];
        v.insert(v.end(), { wa.x, wa.y, wa.z, wb.x, wb.y, wb.z });
    };

    const int kRingSegments = 48;
    for (int a = 0; a < 3; ++a) {
        const glm::vec3 d = axes[a].dir;
        // Two vectors perpendicular to the axis, defining the rotation plane.
        glm::vec3 p1 = (std::abs(d.y) < 0.9f)
                           ? glm::normalize(glm::cross(d, glm::vec3(0, 1, 0)))
                           : glm::normalize(glm::cross(d, glm::vec3(1, 0, 0)));
        const glm::vec3 p2 = glm::cross(d, p1);

        if (type == GizmoType::Rotate) {
            // Circle in the plane perpendicular to the axis.
            glm::vec3 prev = (p1 * glm::cos(0.0f) + p2 * glm::sin(0.0f)) * len;
            for (int i = 1; i <= kRingSegments; ++i) {
                const float th = 2.0f * (float)M_PI * i / kRingSegments;
                const glm::vec3 cur = (p1 * glm::cos(th) + p2 * glm::sin(th)) * len;
                line(prev, cur, a);
                prev = cur;
            }
        } else {
            // Shaft from origin to the handle.
            line(glm::vec3(0.0f), d * len, a);

            if (type == GizmoType::Scale) {
                // Small cube at the end (12 edges).
                const float s = size * 0.12f;
                glm::vec3 c[8];
                for (int i = 0; i < 8; ++i) {
                    const float sx = (i & 1) ? s : -s;
                    const float sy = (i & 2) ? s : -s;
                    const float sz = (i & 4) ? s : -s;
                    c[i] = d * (len + sz) + p1 * sx + p2 * sy;
                }
                const int edges[12][2] = {
                    {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}
                };
                for (const auto& e : edges) line(c[e[0]], c[e[1]], a);
            } else {
                // Arrow head (translate).
                const float headLen = size * 0.25f;
                const float headW = size * 0.09f;
                const glm::vec3 tip = d * len;
                const glm::vec3 base = d * (len - headLen);
                line(tip, base + p1 * headW, a);
                line(tip, base - p1 * headW, a);
                line(tip, base + p2 * headW, a);
                line(tip, base - p2 * headW, a);
            }
        }
    }

    // Create VAO for gizmo
    GLuint gizmoVAO, gizmoVBO;
    glGenVertexArrays(1, &gizmoVAO);
    glGenBuffers(1, &gizmoVBO);
    glBindVertexArray(gizmoVAO);
    glBindBuffer(GL_ARRAY_BUFFER, gizmoVBO);
    // Plenty for the largest gizmo (rotate ring: 3 * 48 * 2 * 3 floats).
    glBufferData(GL_ARRAY_BUFFER, 2048 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glEnableVertexAttribArray(0);

    for (int a = 0; a < 3; ++a) {
        if (axisVerts[a].empty()) continue;
        glBufferSubData(GL_ARRAY_BUFFER, 0, axisVerts[a].size() * sizeof(float), axisVerts[a].data());

        const glm::vec3 col = (activeAxis == static_cast<GizmoAxis>(a + 1))
                                  ? axes[a].highlight : axes[a].color;
        glUniform3f(colorLoc, col.r, col.g, col.b);
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(axisVerts[a].size() / 3));
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
void Show(bool show) { g_editor.setShowGizmo(show); }
bool IsVisible() { return g_editor.showGizmo(); }

// ============================================================================
// Gizmo Interaction Implementation
// ============================================================================

GizmoAxis HitTest(const glm::vec3& gizmoPos, float gizmoSize, const glm::quat& gizmoRot,
                  const glm::mat4& view, const glm::mat4& projection,
                  int vpX, int vpY, int vpW, int vpH,
                  float mouseX, float mouseY) {
    if (g_gizmoType == GizmoType::None) return GizmoAxis::None;

    const glm::mat4 rotMat = glm::toMat4(gizmoRot);
    const float axisLen = gizmoSize * 0.8f;

    struct AxisLine {
        GizmoAxis axis;
        glm::vec3 start;
        glm::vec3 end;
    };

    // Rotate gizmo: hit-test the circle (pick the axis whose ring passes near
    // the cursor). The ring radius is gizmoSize * 0.8, matching Draw().
    const float ringR = gizmoSize * 0.8f;
    AxisLine axes[3] = {
        { GizmoAxis::X, gizmoPos, gizmoPos + glm::vec3(rotMat * glm::vec4(axisLen, 0, 0, 0)) },
        { GizmoAxis::Y, gizmoPos, gizmoPos + glm::vec3(rotMat * glm::vec4(0, axisLen, 0, 0)) },
        { GizmoAxis::Z, gizmoPos, gizmoPos + glm::vec3(rotMat * glm::vec4(0, 0, axisLen, 0)) },
    };

    // Find closest axis
    float minDist = 15.0f; // Pixel threshold
    GizmoAxis closest = GizmoAxis::None;

    for (const auto& ax : axes) {
        glm::vec2 screenStart = WorldToScreen(ax.start, view, projection, vpX, vpY, vpW, vpH);
        glm::vec2 screenEnd = WorldToScreen(ax.end, view, projection, vpX, vpY, vpW, vpH);

        float dist;
        if (g_gizmoType == GizmoType::Rotate) {
            // Distance from the cursor to the projected ring. Sample the ring
            // by projecting a few points of the circle and taking the minimum.
            const glm::vec3 d = ax.end - ax.start;  // world axis dir * length
            const glm::vec3 axisDir = glm::normalize(d);
            glm::vec3 p1 = (std::abs(axisDir.y) < 0.9f)
                               ? glm::normalize(glm::cross(axisDir, glm::vec3(0, 1, 0)))
                               : glm::normalize(glm::cross(axisDir, glm::vec3(1, 0, 0)));
            const glm::vec3 p2 = glm::cross(axisDir, p1);
            dist = 1e9f;
            const int kSamples = 24;
            for (int i = 0; i < kSamples; ++i) {
                const float th = 2.0f * (float)M_PI * i / kSamples;
                const glm::vec3 wp = gizmoPos + (p1 * glm::cos(th) + p2 * glm::sin(th)) * ringR;
                const glm::vec2 sp = WorldToScreen(wp, view, projection, vpX, vpY, vpW, vpH);
                dist = std::min(dist, glm::distance(glm::vec2(mouseX, mouseY), sp));
            }
        } else {
            // Distance from mouse point to screen-space line segment
            dist = PointToLineDistance(glm::vec3(mouseX, mouseY, 0),
                                       glm::vec3(screenStart, 0),
                                       glm::vec3(screenEnd, 0));
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

float UpdateDragRotate(float mouseX, float mouseY, const glm::mat4& view,
                       const glm::mat4& projection, int vpW, int vpH) {
    if (!g_dragging || g_dragAxis == GizmoAxis::None) return 0.0f;

    // Intersect a mouse ray with the plane through the gizmo position whose
    // normal is the rotation axis.
    auto planePoint = [&](float sx, float sy) -> glm::vec3 {
        auto [origin, dir] = ScreenToWorldRay(sx, sy, view, projection, vpW, vpH);
        const glm::vec3 axis = g_dragAxisWorld;
        const float den = glm::dot(dir, axis);
        if (std::abs(den) < 1e-4f) return g_dragStartPos;  // ray parallel to plane
        const float t = glm::dot(g_dragStartPos - origin, axis) / den;
        return origin + dir * t;
    };

    const glm::vec3 cur = planePoint(mouseX, mouseY);

    // First call of a drag: seed the reference point and return no rotation.
    if (!g_hasLastRotPoint) {
        g_lastRotPlanePoint = planePoint(g_dragStartMouse.x, g_dragStartMouse.y);
        g_hasLastRotPoint = true;
        return 0.0f;
    }

    const glm::vec3 axis = g_dragAxisWorld;
    const glm::vec3 r0 = g_lastRotPlanePoint - g_dragStartPos;
    const glm::vec3 r1 = cur - g_dragStartPos;
    g_lastRotPlanePoint = cur;
    if (glm::length(r0) < 1e-4f || glm::length(r1) < 1e-4f) return 0.0f;

    const glm::vec3 n0 = glm::normalize(r0);
    const glm::vec3 n1 = glm::normalize(r1);
    const float ang = std::atan2(glm::dot(glm::cross(n0, n1), axis), glm::dot(n0, n1));
    return glm::degrees(ang);
}

float UpdateDragScale(float mouseX, float mouseY, const glm::mat4& view,
                      const glm::mat4& projection, int vpW, int vpH) {
    if (!g_dragging || g_dragAxis == GizmoAxis::None) return 1.0f;

    // Screen-space drag: project the gizmo origin + one axis-length unit,
    // then map mouse travel along that screen direction to a scale factor.
    const glm::vec2 g0 = WorldToScreen(g_dragStartPos, view, projection, 0, 0, vpW, vpH);
    const glm::vec2 g1 = WorldToScreen(g_dragStartPos + g_dragAxisWorld * 1.0f, view, projection, 0, 0, vpW, vpH);
    glm::vec2 dir = g1 - g0;
    const float screenLen = glm::length(dir);
    if (screenLen < 1e-4f) return 1.0f;
    dir /= screenLen;

    const float d0 = glm::dot(glm::vec2(g_dragStartMouse.x, g_dragStartMouse.y) - g0, dir);
    const float d1 = glm::dot(glm::vec2(mouseX, mouseY) - g0, dir);

    // Dragging the mouse one full axis length on screen roughly doubles the
    // scale; clamp so the object cannot be scaled to zero or flipped.
    const float factor = 1.0f + (d1 - d0) / (screenLen * 2.0f);
    return glm::clamp(factor, 0.05f, 20.0f);
}

void EndDrag() {
    g_dragging = false;
    g_dragAxis = GizmoAxis::None;
    g_dragAxisWorld = glm::vec3(0.0f);
    g_hasLastRotPoint = false;
}

bool IsDragging() { return g_dragging; }
GizmoAxis GetDragAxis() { return g_dragAxis; }
glm::vec3 GetDragAxisWorld() { return g_dragAxisWorld; }

} // namespace GizmoRenderer
