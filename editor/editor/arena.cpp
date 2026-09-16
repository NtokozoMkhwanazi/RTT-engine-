#include "arena.h"
#include "shaderSystem/Shader.h"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <cmath>

namespace Arena {

// ===========================================================================
// Embedded GLSL shaders (self-contained — no external file dependencies)
// ===========================================================================

static const char* kArenaVertexSrc = R"(#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uModel;

out vec3 vFragPos;
out vec3 vNormal;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vFragPos = worldPos.xyz;
    // Normal matrix (transpose of inverse) for correct lighting under scaling
    vNormal = normalize(mat3(transpose(inverse(uModel))) * aNormal);
    gl_Position = uProjection * uView * worldPos;
}
)";

static const char* kArenaFragmentSrc = R"(#version 430 core
out vec4 FragColor;

in vec3 vFragPos;
in vec3 vNormal;

uniform vec3 uColor;
uniform vec3 uLightDir;   // world-space light direction (points FROM surface TO sun)
uniform vec3 uLightColor;
uniform vec3 uViewPos;
uniform float uAmbient;

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(-uLightDir);
    float diff = max(dot(N, L), 0.0);

    // Simple Blinn-Phong specular highlight
    vec3 V = normalize(uViewPos - vFragPos);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 32.0) * 0.25;

    vec3 lit = (diff + uAmbient) * uColor + spec * uLightColor;
    FragColor = vec4(lit, 1.0);
}
)";

// ===========================================================================
// Construction / lifecycle
// ===========================================================================

ArenaScene::ArenaScene() = default;

bool ArenaScene::initialize() {
    if (m_initialized) return true;

    // --- Cube VAO (positions + normals, per-face for flat shading) ---
    // A 1x1x1 cube centred at the origin. Per-face normals give crisp edges
    // that read clearly as "blocky walls", matching the Unreal QA-arena look.
    static const float verts[] = {
        //  pos                     normal
        // -Z face (back, -Z)
        -0.5f,-0.5f,-0.5f,   0, 0,-1,   0.5f,-0.5f,-0.5f,  0, 0,-1,
         0.5f, 0.5f,-0.5f,   0, 0,-1,  -0.5f, 0.5f,-0.5f,  0, 0,-1,
        // +Z face (front, +Z)
        -0.5f,-0.5f, 0.5f,   0, 0, 1,   0.5f,-0.5f, 0.5f,  0, 0, 1,
         0.5f, 0.5f, 0.5f,   0, 0, 1,  -0.5f, 0.5f, 0.5f,  0, 0, 1,
        // -X face (left)
        -0.5f,-0.5f,-0.5f,  -1, 0, 0,  -0.5f,-0.5f, 0.5f, -1, 0, 0,
        -0.5f, 0.5f, 0.5f,  -1, 0, 0,  -0.5f, 0.5f,-0.5f, -1, 0, 0,
        // +X face (right)
         0.5f,-0.5f,-0.5f,   1, 0, 0,   0.5f,-0.5f, 0.5f,  1, 0, 0,
         0.5f, 0.5f, 0.5f,   1, 0, 0,   0.5f, 0.5f,-0.5f,  1, 0, 0,
        // -Y face (bottom)
        -0.5f,-0.5f,-0.5f,   0,-1, 0,   0.5f,-0.5f,-0.5f,  0,-1, 0,
         0.5f,-0.5f, 0.5f,   0,-1, 0,  -0.5f,-0.5f, 0.5f,  0,-1, 0,
        // +Y face (top)
        -0.5f, 0.5f,-0.5f,   0, 1, 0,   0.5f, 0.5f,-0.5f,  0, 1, 0,
         0.5f, 0.5f, 0.5f,   0, 1, 0,  -0.5f, 0.5f, 0.5f,  0, 1, 0,
    };
    static const unsigned short idx[] = {
        0,1,2, 2,3,0,          // -Z
        4,5,6, 6,7,4,          // +Z
        8,9,10, 10,11,8,       // -X
        12,13,14, 14,15,12,     // +X
        16,17,18, 18,19,16,     // -Y
        20,21,22, 22,23,20      // +Y
    };

    glGenVertexArrays(1, &m_cubeVAO);
    glGenBuffers(1, &m_cubeVBO);
    glGenBuffers(1, &m_cubeEBO);
    glBindVertexArray(m_cubeVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_cubeVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_cubeEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float),
                          (void*)(3*sizeof(float)));
    m_cubeIndexCount = (GLsizei)(sizeof(idx) / sizeof(idx[0]));
    glBindVertexArray(0);

    // --- Simple lit shader (compiled from embedded source, like Skybox) ---
    m_shader = new Shader(kArenaVertexSrc, kArenaFragmentSrc, /*fromString=*/true);
    if (m_shader->ID == 0) {
        std::cerr << "[Arena] Failed to compile arena shader\n";
        delete m_shader;
        m_shader = nullptr;
        shutdown();
        return false;
    }

    m_initialized = true;
    buildWalls();
    std::cout << "[Arena] Initialized — " << m_walls.size()
              << " walls (" << (m_fullHeight * 0.25f) << "m partial, "
              << m_fullHeight << "m full), radius=" << m_radius << "m\n";
    return true;
}

void ArenaScene::shutdown() {
    if (m_shader)      { delete m_shader; m_shader = nullptr; }
    if (m_cubeEBO)     { glDeleteBuffers(1, &m_cubeEBO); m_cubeEBO = 0; }
    if (m_cubeVBO)     { glDeleteBuffers(1, &m_cubeVBO); m_cubeVBO = 0; }
    if (m_cubeVAO)     { glDeleteVertexArrays(1, &m_cubeVAO); m_cubeVAO = 0; }
    m_cubeIndexCount = 0;
    m_walls.clear();
    m_floor.clear();
    m_initialized = false;
}

// ===========================================================================
// Wall / floor layout
// ===========================================================================

void ArenaScene::buildWalls() {
    m_walls.clear();
    m_floor.clear();

    // --- Ground platform: a single wide, flat cube ---
    {
        CubeInstance f;
        f.position = {0.0f, -0.02f, 0.0f};  // just below 0 so it sits flush
        f.scale    = {m_groundSize, 0.04f, m_groundSize};
        f.color    = m_floorColor;
        m_floor.push_back(f);
    }

    // --- Circular wall ring ---
    // 32 segments around the ring. Every third wall is full-height; the rest
    // are quarter-height, creating a rhythm of "tall, short, short" around
    // the arena. The short walls act as low barriers / visual rhythm.
    constexpr int kSegs = 32;
    const float halfH     = m_fullHeight * 0.5f;
    const float quarterH  = m_fullHeight * 0.25f;
    const float segAngle  = (2.0f * 3.14159265f) / kSegs;

    for (int i = 0; i < kSegs; ++i) {
        float angle = segAngle * i;
        float cx = std::cos(angle) * m_radius;
        float cz = std::sin(angle) * m_radius;

        // Wall length: arc length for one segment, slightly shortened to
        // leave small gaps between segments for a lighter visual rhythm.
        float arcLen = m_radius * segAngle * 0.92f;

        // Every 3rd segment (slots 0, 3, 6, ...) is FULL height.
        // Slots 1, 4, 7, ... are PARTIAL (lower) walls.
        // Slots 2, 5, 8, ... are also full height (gap — no wall).
        // This gives: full, partial, open, full, partial, open, ...
        if (i % 3 == 1) {
            // Partial (shorter) wall
            CubeInstance w;
            w.position = {cx, quarterH, cz};
            w.scale    = {arcLen, quarterH, m_thickness};
            w.color    = m_partialHeightColor;
            m_walls.push_back(w);
        } else if (i % 3 == 0) {
            // Full-height wall
            CubeInstance w;
            w.position = {cx, halfH, cz};
            w.scale    = {arcLen, m_fullHeight, m_thickness};
            w.color    = m_fullHeightColor;
            m_walls.push_back(w);
        }
        // i % 3 == 2: gap (no wall) — open view
    }
}

void ArenaScene::rebuild() {
    if (!m_initialized) return;
    buildWalls();
    std::cout << "[Arena] Rebuilt — " << m_walls.size() << " walls\n";
}

// ===========================================================================
// Rendering
// ===========================================================================

void ArenaScene::render(const glm::mat4& view, const glm::mat4& projection,
                        const glm::vec3& cameraPos) {
    if (!m_visible || !m_initialized || !m_shader || m_cubeVAO == 0)
        return;

    m_shader->use();
    m_shader->setMat4("uView", view);
    m_shader->setMat4("uProjection", projection);
    m_shader->setVec3("uViewPos", cameraPos);
    m_shader->setVec3("uLightDir", glm::vec3(0.3f, 1.0f, 0.4f));     // warm sun
    m_shader->setVec3("uLightColor", glm::vec3(1.0f, 0.95f, 0.85f));
    m_shader->setFloat("uAmbient", 0.25f);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glBindVertexArray(m_cubeVAO);

    // Draw walls
    for (const auto& w : m_walls) {
        glm::mat4 m(1.0f);
        m = glm::translate(m, w.position);
        m = glm::scale(m, w.scale);
        m_shader->setMat4("uModel", m);
        m_shader->setVec3("uColor", w.color);
        glDrawElements(GL_TRIANGLES, m_cubeIndexCount, GL_UNSIGNED_SHORT, 0);
    }

    // Draw floor
    for (const auto& f : m_floor) {
        glm::mat4 m(1.0f);
        m = glm::translate(m, f.position);
        m = glm::scale(m, f.scale);
        m_shader->setMat4("uModel", m);
        m_shader->setVec3("uColor", f.color);
        glDrawElements(GL_TRIANGLES, m_cubeIndexCount, GL_UNSIGNED_SHORT, 0);
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

} // namespace Arena
