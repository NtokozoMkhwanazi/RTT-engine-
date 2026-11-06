#include "gizmo_renderer.h"
#include "editor_state.h"
#include "imgui.h"   // DrawImGui(): screen-space overlay (Vulkan editor path)
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>

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
// forPlayer.txt Theme 3: made non-static (declared in gizmo_renderer.h) so
// editor UI can share the gizmo's NDC->world-ray convention for viewport prop
// picking, instead of duplicating the inverse-projection math here.
std::pair<glm::vec3, glm::vec3> ScreenToWorldRay(float screenX, float screenY,
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

// ---------------------------------------------------------------------------
// Gizmo outline halo (the "this handle is selected" glow).
// A screen-space additive outline: the gizmo handle geometry is rendered a
// second time, half-dilated in view space along +/- uThickness (two passes =>
// a centered ribbon) so it works under the core profile (glLineWidth>1 is not
// reliable there), blended ADDITIVELY with a warm tint, depth-TEST OFF +
// depth-MASK OFF so the glow wraps the handles even behind scene geometry.
// Self-contained: compiles its own outline program on first use (matches the
// #version 430 core of editor/shader_manager.cpp gizmo shader) and fully
// restores the caller's GL state.
//
// IMPORTANT: Draw() (and the depth save/restore guard the gizmo tests rely on)
// is left UNTOUCHED in behavior; only its vertex-generation is factored into
// buildAxisVerts() so the halo stays geometrically in sync. DrawOutline itself
// is invoked only by the editor caller — no test calls it, so its shader compile
// is never exercised on the test GL path.
// ---------------------------------------------------------------------------

struct GizmoGeometry {
    std::array<std::vector<float>, 3> pos; // 3 floats/vert, per axis (mirrors Draw's axisVerts)
    std::array<std::vector<float>, 3> dir; // 3 floats/vert: owning axis world dir per vertex
};

// Builds gizmo handle line vertices. Mirrors editor/gizmo_renderer.cpp Draw()
// geometry generation exactly — keep in sync if handle shapes change.
static GizmoGeometry buildAxisVerts(const glm::vec3& position, float size,
                                    const glm::quat& rotation, GizmoType type,
                                    const glm::mat4& rotMat, float len, float sizeS) {
    GizmoGeometry verts;
    struct AxisDef {
        glm::vec3 dir;
        glm::vec3 color;
        glm::vec3 highlight;
    };
    // NOTE: must match Draw()'s axis color table exactly so halo + handles align.
    const AxisDef axes[3] = {
        { glm::vec3(1, 0, 0), glm::vec3(0.85f, 0.25f, 0.22f), glm::vec3(1.0f, 0.80f, 0.30f) },
        { glm::vec3(0, 1, 0), glm::vec3(0.22f, 0.80f, 0.25f), glm::vec3(0.85f, 1.00f, 0.35f) },
        { glm::vec3(0, 0, 1), glm::vec3(0.25f, 0.40f, 0.90f), glm::vec3(0.60f, 0.85f, 1.00f) },
    };

    auto world = [&](const glm::vec3& local) -> glm::vec3 {
        return position + glm::vec3(rotMat * glm::vec4(local, 1.0f));
    };
    auto line = [&](const glm::vec3& a, const glm::vec3& b, int axisIdx) {
        const glm::vec3 wa = world(a);
        const glm::vec3 wb = world(b);
        const glm::vec3 wd = glm::normalize(glm::vec3(rotMat * glm::vec4(axes[axisIdx].dir, 0.0f)));
        std::vector<float>& p = verts.pos[axisIdx];
        std::vector<float>& d = verts.dir[axisIdx];
        p.insert(p.end(), { wa.x, wa.y, wa.z, wb.x, wb.y, wb.z });
        d.insert(d.end(), { wd.x, wd.y, wd.z, wd.x, wd.y, wd.z });
    };

    const int kRingSegments = 48;
    for (int a = 0; a < 3; ++a) {
        const glm::vec3 d = axes[a].dir;
        glm::vec3 p1 = (std::abs(d.y) < 0.9f)
                           ? glm::normalize(glm::cross(d, glm::vec3(0, 1, 0)))
                           : glm::normalize(glm::cross(d, glm::vec3(1, 0, 0)));
        const glm::vec3 p2 = glm::cross(d, p1);

        if (type == GizmoType::Rotate) {
            glm::vec3 prev = (p1 * glm::cos(0.0f) + p2 * glm::sin(0.0f)) * len;
            for (int i = 1; i <= kRingSegments; ++i) {
                const float th = 2.0f * (float)M_PI * i / kRingSegments;
                const glm::vec3 cur = (p1 * std::cos(th) + p2 * std::sin(th)) * len;
                line(prev, cur, a);
                prev = cur;
            }
        } else {
            line(glm::vec3(0.0f), d * len, a);
            if (type == GizmoType::Scale) {
                const float s = sizeS * 0.12f;
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
                const float headLen = sizeS * 0.25f;
                const float headW = sizeS * 0.09f;
                const glm::vec3 tip = d * len;
                const glm::vec3 base = d * (len - headLen);
                line(tip, base + p1 * headW, a);
                line(tip, base - p1 * headW, a);
                line(tip, base + p2 * headW, a);
                line(tip, base - p2 * headW, a);
            }
        }
    }
    return verts;
}

static GLuint compileGizmoOutlineProgram() {
    static GLuint prog = 0;
    static bool tried = false;
    if (tried) return prog;
    tried = true;

    const char* vs = R"(#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aAxis;       // world-space segment dir (unit)
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uModel;
uniform vec4 uColor;
uniform float uThickness = 0.05;           // view-space half-width
uniform float uSign = 1.0;                 // +1/-1 => two-sided ribbon
out vec4 vColor;
void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vec4 viewPos  = uView * worldPos;
    // screen-space perpendicular to the view-space segment (glow direction)
    vec3 vAxis = mat3(uView) * aAxis;
    vec3 perp  = cross(normalize(vAxis), vec3(0.0, 0.0, 1.0));
    if (length(perp) < 1e-4) perp = vec3(1.0, 0.0, 0.0);
    viewPos.xyz += normalize(perp) * (uSign * uThickness);
    vColor = uColor;
    gl_Position = uProjection * viewPos;
}
)";
    const char* fs = R"(#version 430 core
in vec4 vColor;
out vec4 FragColor;
void main() { FragColor = vColor; }
)";

    GLint ok = 0;
    char log[512];

    GLuint gvs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(gvs, 1, &vs, nullptr);
    glCompileShader(gvs);
    glGetShaderiv(gvs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(gvs, 512, nullptr, log);
        std::fprintf(stderr, "[GizmoRenderer] outline VS compile failed: %s\n", log);
        glDeleteShader(gvs);
        return prog; // 0
    }

    GLuint gfs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(gfs, 1, &fs, nullptr);
    glCompileShader(gfs);
    glGetShaderiv(gfs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(gfs, 512, nullptr, log);
        std::fprintf(stderr, "[GizmoRenderer] outline FS compile failed: %s\n", log);
        glDeleteShader(gvs);
        glDeleteShader(gfs);
        return prog; // 0
    }

    prog = glCreateProgram();
    glAttachShader(prog, gvs);
    glAttachShader(prog, gfs);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(prog, 512, nullptr, log);
        std::fprintf(stderr, "[GizmoRenderer] outline program link failed: %s\n", log);
        glDeleteProgram(prog);
        prog = 0;
    }
    glDeleteShader(gvs);
    glDeleteShader(gfs);
    return prog;
}

void DrawOutline(const glm::vec3& position, float size, const glm::quat& rotation,
                 const glm::mat4& view, const glm::mat4& projection, GizmoType type) {
    if (type == GizmoType::None) return;

    const GLuint prog = compileGizmoOutlineProgram();
    if (!prog) return; // no GL context or compile failed -> silent no-op

    // Track the per-axis scale Draw() uses so the halo matches at any distance.
    const glm::mat4 rotMat = glm::toMat4(rotation);
    const glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);
    const float dist = glm::max(glm::distance(camPos, position), 1.0f);
    const float distScale = glm::clamp(dist * 0.05f, 0.5f, 4.0f);
    const float sizeS = size * distScale;
    const float len = sizeS * 0.8f;
    const GizmoGeometry verts = buildAxisVerts(position, size, rotation, type, rotMat, len, sizeS);

    // Save caller GL state, then switch to the additive outline state.
    const bool blendEnabled = glIsEnabled(GL_BLEND) == GL_TRUE;
    const bool depthTestEnabled = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
    GLint prevDepthFunc = 0;           glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
    GLboolean prevDepthMask = GL_FALSE; glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);  // additive glow
    glDisable(GL_DEPTH_TEST);           // halo visible behind scene geometry
    glDepthMask(GL_FALSE);

    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uView"),      1, GL_FALSE, &view[0][0]);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uProjection"), 1, GL_FALSE, &projection[0][0]);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uModel"),     1, GL_FALSE, glm::value_ptr(glm::mat4(1.0f)));
    glUniform1f(glGetUniformLocation(prog, "uThickness"), 0.04f);
    const glm::vec4 tint(1.0f, 1.0f, 0.9f, 0.55f); // warm additive outline
    glUniform4fv(glGetUniformLocation(prog, "uColor"), 1, &tint[0]);

    GLuint vao = 0, vboPos = 0, vboDir = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vboPos);
    glGenBuffers(1, &vboDir);
    glBindVertexArray(vao);

    glBindBuffer(GL_ARRAY_BUFFER, vboPos);
    glBufferData(GL_ARRAY_BUFFER, 2048 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, vboDir);
    glBufferData(GL_ARRAY_BUFFER, 2048 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glEnableVertexAttribArray(1);

    for (int sign = -1; sign <= 1; ++sign) {   // two-sided => centered ribbon
        if (sign == 0) continue;
        glUniform1f(glGetUniformLocation(prog, "uSign"), static_cast<float>(sign));
        for (int a = 0; a < 3; ++a) {
            if (verts.pos[a].empty()) continue;
            glBindBuffer(GL_ARRAY_BUFFER, vboPos);
            glBufferSubData(GL_ARRAY_BUFFER, 0, verts.pos[a].size() * sizeof(float), verts.pos[a].data());
            glBindBuffer(GL_ARRAY_BUFFER, vboDir);
            glBufferSubData(GL_ARRAY_BUFFER, 0, verts.dir[a].size() * sizeof(float), verts.dir[a].data());
            glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(verts.pos[a].size() / 3));
        }
    }

    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vboPos);
    glDeleteBuffers(1, &vboDir);
    glUseProgram(0);

    // Restore caller GL state. Blend *func* is owned by the caller (the editor
    // viewport re-sets it after we return), so we only restore enable + depth state.
    if (blendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glDepthMask(prevDepthMask);
    if (depthTestEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthFunc(prevDepthFunc);
}

void Draw(const glm::vec3& position, float size, const glm::quat& rotation,
          const glm::mat4& view, const glm::mat4& projection, GLuint gizmoShaderProgram,
          GizmoType type) {
    if (!g_editor.showGizmo() || type == GizmoType::None) return;

    // Save the caller's depth-test enable state. The gizmo is drawn with depth
    // testing OFF so its manipulation handles stay pickable behind scene geometry;
    // the prior code unconditionally re-enabled GL_DEPTH_TEST at exit, leaking a
    // stale enable into the downstream ImGui / forward passes. Mirror
    // grid_renderer.cpp's save/restore guard so the caller's state is restored.
    const bool depthTestWasEnabled = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
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

    // Perspective-aware scaling (forUI.txt gizmo point): grow the gizmo with
    // camera distance so it stays a usable on-screen size at any range — the
    // previous `size * 0.8f` shrank it to a pixel when zoomed out. Clamped so
    // it can't explode near the far plane.
    const glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);
    const float dist = glm::max(glm::distance(camPos, position), 1.0f);
    const float distScale = glm::clamp(dist * 0.05f, 0.5f, 4.0f);
    const float sizeS = size * distScale;
    const float len = sizeS * 0.8f;

    // Direction camera->gizmo, used to fade axes that point straight at/away
    // from the viewer ("looking down the pipe") so edge-on axes can't be
    // accidentally grabbed.
    const glm::vec3 viewDir = (dist > 0.001f) ? glm::normalize(position - camPos)
                                              : glm::vec3(0, 0, -1);

    // Geometry is generated once (shared with DrawOutline) so the colored
    // handles and the screen-space outline halo stay geometrically in sync.
    GizmoGeometry verts = buildAxisVerts(position, size, rotation, type, rotMat, len, sizeS);
    std::array<std::vector<float>, 3>& axisVerts = verts.pos;

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
        // Fade axes that are edge-on to the camera (aligned with viewDir) so
        // they can't be grabbed when the view lines up with an axis.
        const glm::vec3 worldAxis = glm::normalize(glm::vec3(rotMat * glm::vec4(axes[a].dir, 0.0f)));
        const float align = glm::abs(glm::dot(worldAxis, viewDir));
        const float dim = (align > 0.92f) ? 0.35f : 1.0f;
        glUniform3f(colorLoc, col.r * dim, col.g * dim, col.b * dim);
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(axisVerts[a].size() / 3));
    }

    glBindVertexArray(0);
    glDeleteVertexArrays(1, &gizmoVAO);
    glDeleteBuffers(1, &gizmoVBO);
    glUseProgram(0);
    // Restore the caller's depth-test enable bit instead of forcing GL_DEPTH_TEST
    // on unconditionally (matches grid_renderer.cpp's parity guard).
    if (depthTestWasEnabled) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
}

// Vulkan editor gizmo: a depth-test-free screen-space ImGui overlay across the
// swapchain scene. Mirrors Draw()'s geometry (perspective-aware scale, arrow
// heads / rotate ring / scale cube, edge-on axis dimming, active-axis
// highlight) but emits ImGui lines instead of GL VAO/VBO draws, so no GL/Vulkan
// shader or vertex path is needed. Draw() (GL) is left untouched.
void DrawImGui(const glm::vec3& position, float size, const glm::quat& rotation,
               const glm::mat4& view, const glm::mat4& projection,
               int vpX, int vpY, int vpW, int vpH, GizmoType type) {
    if (!g_editor.showGizmo() || type == GizmoType::None) return;

    const glm::mat4 rotMat = glm::toMat4(rotation);
    const GizmoAxis activeAxis = g_dragging ? g_dragAxis : g_editor.hoveredAxis();
    const glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);
    const float dist = glm::max(glm::distance(camPos, position), 1.0f);
    const float distScale = glm::clamp(dist * 0.05f, 0.5f, 4.0f);
    const float sizeS = size * distScale;
    const float len = sizeS * 0.8f;
    const glm::vec3 viewDir = (dist > 0.001f) ? glm::normalize(position - camPos)
                                              : glm::vec3(0, 0, -1);

    struct AxisDef { glm::vec3 dir, color, highlight; };
    const AxisDef axes[3] = {
        { glm::vec3(1, 0, 0), glm::vec3(0.85f, 0.25f, 0.22f), glm::vec3(1.0f, 0.80f, 0.30f) },
        { glm::vec3(0, 1, 0), glm::vec3(0.22f, 0.80f, 0.25f), glm::vec3(0.85f, 1.00f, 0.35f) },
        { glm::vec3(0, 0, 1), glm::vec3(0.25f, 0.40f, 0.90f), glm::vec3(0.60f, 0.85f, 1.00f) },
    };

    auto world = [&](const glm::vec3& local) -> glm::vec3 {
        return position + glm::vec3(rotMat * glm::vec4(local, 1.0f));
    };
    auto emit = [&](const glm::vec3& a, const glm::vec3& b,
                    const glm::vec3& col, float dim) {
        const glm::vec2 sa = WorldToScreen(a, view, projection, vpX, vpY, vpW, vpH);
        const glm::vec2 sb = WorldToScreen(b, view, projection, vpX, vpY, vpW, vpH);
        const float f = std::clamp(dim, 0.0f, 1.0f) * 255.0f;
        ImU32 c = IM_COL32((int)(col.r * f), (int)(col.g * f), (int)(col.b * f), 255);
        ImGui::GetBackgroundDrawList()->AddLine(ImVec2(sa.x, sa.y),
                                              ImVec2(sb.x, sb.y), c, 2.0f);
    };

    const int kRingSegments = 48;
    for (int a = 0; a < 3; ++a) {
        const glm::vec3 d = axes[a].dir;
        glm::vec3 p1 = (std::abs(d.y) < 0.9f)
                           ? glm::normalize(glm::cross(d, glm::vec3(0, 1, 0)))
                           : glm::normalize(glm::cross(d, glm::vec3(1, 0, 0)));
        const glm::vec3 p2 = glm::cross(d, p1);
        const glm::vec3 col = (activeAxis == static_cast<GizmoAxis>(a + 1))
                                  ? axes[a].highlight : axes[a].color;
        const glm::vec3 worldAxis =
            glm::normalize(glm::vec3(rotMat * glm::vec4(axes[a].dir, 0.0f)));
        const float align = glm::abs(glm::dot(worldAxis, viewDir));
        const float dim = (align > 0.92f) ? 0.35f : 1.0f;

        if (type == GizmoType::Rotate) {
            glm::vec3 prev = (p1 * std::cos(0.0f) + p2 * std::sin(0.0f)) * len;
            for (int i = 1; i <= kRingSegments; ++i) {
                const float th = 2.0f * (float)M_PI * i / kRingSegments;
                glm::vec3 cur = (p1 * std::cos(th) + p2 * std::sin(th)) * len;
                emit(world(prev), world(cur), col, dim);
                prev = cur;
            }
        } else {
            emit(world(glm::vec3(0.0f)), world(d * len), col, dim);
            if (type == GizmoType::Scale) {
                const float s = sizeS * 0.12f;
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
                for (const auto& e : edges) emit(world(c[e[0]]), world(c[e[1]]), col, dim);
            } else {
                const float headLen = sizeS * 0.25f;
                const float headW  = sizeS * 0.09f;
                const glm::vec3 tip  = d * len;
                const glm::vec3 base = d * (len - headLen);
                emit(world(tip), world(base + p1 * headW), col, dim);
                emit(world(tip), world(base - p1 * headW), col, dim);
                emit(world(tip), world(base + p2 * headW), col, dim);
                emit(world(tip), world(base - p2 * headW), col, dim);
            }
        }
    }
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

    // Perspective-aware click bounds (Todo 8). Draw() scales the gizmo on screen
    // by `distScale` so it stays a usable size at any camera range; the pick
    // volume MUST track that same scale or the gizmo becomes un-clickable when
    // zoomed out (the drawn gizmo grows but the unscaled pick volume stays tiny)
    // and overly grabby when zoomed in. Mirrors Draw()'s distScale formula.
    const glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);
    const float dist = glm::max(glm::distance(camPos, gizmoPos), 1.0f);
    const float distScale = glm::clamp(dist * 0.05f, 0.5f, 4.0f);

    const float axisLen = gizmoSize * 0.8f * distScale;   // matches Draw()'s `len`

    struct AxisLine {
        GizmoAxis axis;
        glm::vec3 start;
        glm::vec3 end;
    };

    // Rotate gizmo: hit-test the circle (pick the axis whose ring passes near
    // the cursor). The ring radius is gizmoSize * 0.8, matching Draw()'s scaled
    // ring radius (gizmoSize * 0.8 * distScale).
    const float ringR = gizmoSize * 0.8f * distScale;
    AxisLine axes[3] = {
        { GizmoAxis::X, gizmoPos, gizmoPos + glm::vec3(rotMat * glm::vec4(axisLen, 0, 0, 0)) },
        { GizmoAxis::Y, gizmoPos, gizmoPos + glm::vec3(rotMat * glm::vec4(0, axisLen, 0, 0)) },
        { GizmoAxis::Z, gizmoPos, gizmoPos + glm::vec3(rotMat * glm::vec4(0, 0, axisLen, 0)) },
    };

    // Find closest axis
    // Pixel click threshold. Grows with camera distance so a zoomed-out (large
    // on-screen) gizmo and a zoomed-in (small) gizmo demand similar relative
    // precision, then clamped to a sane [5, 60] px band.
    float minDist = 15.0f * distScale;
    minDist = glm::clamp(minDist, 5.0f, 60.0f);
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

// Forces compileGizmoOutlineProgram()'s lazy compile and reports linker success.
// Same program DrawOutline() uses internally; cached after the first compile, so
// this is a cheap read thereafter. Lets tests assert the outline GLSL linked.
bool GizmoOutlineCompiled() { return compileGizmoOutlineProgram() != 0; }

} // namespace GizmoRenderer
