#include "arena.h"
#include "shaderSystem/Shader.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>  // transpose(inverse(...)) for the CPU normal matrix
#include <iostream>
#include <cmath>
#include <cstddef>                     // offsetof (instance vertex-attribute setup)
#include <cstdlib>                     // rand()/srand() (forArena.txt Part 2: obstacles)
#include <ctime>                       // time() (forArena.txt Part 2: RNG seed)

namespace Arena {

// ===========================================================================
// Embedded GLSL shaders (self-contained — no external file dependencies)
//
// forArena.txt asks for TWO things that the original doc states separately:
//   (1) instanced rendering  -> model/normal/color come from per-instance
//       vertex attributes (divisor = 1), NOT from per-draw uniforms.
//   (2) "CPU-optimised normal calculations" -> the inverse-transpose is
//       computed ONCE per instance on the CPU and shipped as an attribute,
//       so the vertex shader never calls transpose(inverse(...)).
//
// The original forArena.txt text was self-contradictory here (Section 1 said
// "remove uModel/uColor uniforms, use layout(location=2) mat4 aInstanceModel"
// while Step-1's shader kept the uniforms). This implementation resolves the
// contradiction by going fully instanced AND keeping the CPU normal matrix,
// which is the union of both goals and the highest-throughput option.
// ===========================================================================

static const char* kArenaVertexSrc = R"(#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

// Per-instance attributes (vertex attrib divisor = 1):
//   loc 2-5  : aInstanceModel  (mat4)
//   loc 6-9  : aInstanceNormal (mat4, consumed as mat3)
//   loc 10   : aInstanceColor  (rgb = albedo; a = 1.0 marks the floor for the grid)
layout(location = 2)  in mat4 aInstanceModel;
layout(location = 6)  in mat4 aInstanceNormal;
layout(location = 10) in vec4 aInstanceColor;

uniform mat4 uView;
uniform mat4 uProjection;
uniform vec3 uViewPos;       // world-space camera (shared by every instance)
uniform vec3 uLightDir;      // world-space, points FROM surface TO sun
uniform vec3 uLightColor;
uniform float uAmbient;

out vec3 vFragPos;
out vec3 vNormal;
out vec3 vColor;
out float vIsFloor;

void main() {
    vec4 worldPos = aInstanceModel * vec4(aPos, 1.0);
    vFragPos = worldPos.xyz;
    // No transpose(inverse(...)) here — the inverse-transpose was baked on the
    // CPU into aInstanceNormal. Just rotate the normal by the 3x3.
    vNormal   = normalize(mat3(aInstanceNormal) * aNormal);
    vColor    = aInstanceColor.rgb;
    vIsFloor  = aInstanceColor.a;
    gl_Position = uProjection * uView * worldPos;
}
)";

static const char* kArenaFragmentSrc = R"(#version 430 core
out vec4 FragColor;

in vec3 vFragPos;
in vec3 vNormal;
in vec3 vColor;
in float vIsFloor;

uniform vec3 uViewPos;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform float uAmbient;

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(-uLightDir);
    float diff = max(dot(N, L), 0.0);

    // Blinn-Phong specular highlight
    vec3 V = normalize(uViewPos - vFragPos);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 32.0) * 0.25;

    vec3 lit = (diff + uAmbient) * vColor + spec * uLightColor;

    // --- Procedural World-Space Grid (forArena.txt measurement tooling) ---
    // Anti-aliased 1-metre grid + heavier 5-metre major lines, using fwidth
    // so they stay ~1px wide at any distance. Only on the floor plane.
    if (vIsFloor > 0.5) {
        vec2 coord = vFragPos.xz;
        vec2 grid1 = abs(fract(coord - 0.5) - 0.5) / fwidth(coord);
        float grid1Pattern = 1.0 - min(min(grid1.x, grid1.y), 1.0);
        vec2 grid5 = abs(fract((coord / 5.0) - 0.5) - 0.5) / fwidth(coord / 5.0);
        float grid5Pattern = 1.0 - min(min(grid5.x, grid5.y), 1.0);
        lit = mix(lit, vec3(0.12), grid1Pattern * 0.3); // subtle 1m lines
        lit = mix(lit, vec3(0.05), grid5Pattern * 0.6); // sharp 5m lines
    }

    FragColor = vec4(lit, 1.0);
}
)";

// ===========================================================================
// Construction / lifecycle
// ===========================================================================

namespace {
// Build the GPU-side InstanceData payload for one cube: translate -> rotate
// (tangential Y, then prop-incline X) -> scale, plus a CPU inverse-transpose
// normal matrix so the vertex shader stays cheap. color.a encodes "is floor".
InstanceData makeInstanceData(const CubeInstance& c, bool isFloor) {
    glm::mat4 m(1.0f);
    m = glm::translate(m, c.position);
    if (c.rotationY != 0.0f) m = glm::rotate(m, c.rotationY, glm::vec3(0.0f, 1.0f, 0.0f));
    if (c.rotationX != 0.0f) m = glm::rotate(m, c.rotationX, glm::vec3(1.0f, 0.0f, 0.0f));
    m = glm::scale(m, c.scale);

    // Inverse-transpose of the 3x3 model (CPU normal matrix — forArena.txt tip).
    glm::mat3 n = glm::transpose(glm::inverse(glm::mat3(m)));

    InstanceData d;
    d.modelMatrix  = m;
    d.normalMatrix = glm::mat4(n);
    d.color        = glm::vec4(c.color, isFloor ? 1.0f : 0.0f);
    return d;
}
} // namespace

ArenaScene::ArenaScene() = default;

ArenaScene::~ArenaScene() { shutdown(); }  // out-of-line: unique_ptr<Shader> needs complete Shader

bool ArenaScene::initialize() {
    if (m_initialized) return true;

    // --- Cube VAO (positions + normals, per-face for flat shading) ---
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
        16,18,17, 18,16,19,     // -Y face: CCW winding (was CW -> culled by GL_BACK)
        20,22,21, 22,20,23     // +Y face: CCW winding (floor/walkable top now visible from above)
    };

    glGenVertexArrays(1, &m_cubeVAO);
    glGenBuffers(1, &m_cubeVBO);
    glGenBuffers(1, &m_cubeEBO);
    glGenBuffers(1, &m_instanceVBO);
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

    // --- Per-instance data buffer (instanced rendering — forArena Phase 1) ---
    // Placeholder allocation so the VAO captures a valid buffer handle for the
    // instance attribs below; uploadInstances() refills it with real data.
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
    glBufferData(GL_ARRAY_BUFFER, 1, nullptr, GL_STREAM_DRAW);

    // Helper: enable one vec4-wide instance attribute at (loc, byteOffset).
    auto setupInstanceAttrib = [](GLuint loc, size_t byteOffset) {
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(InstanceData),
                              (void*)byteOffset);
        glVertexAttribDivisor(loc, 1);  // advance once per instance, not per vertex
    };
    const size_t matRow = sizeof(glm::vec4);
    // aInstanceModel  -> locations 2,3,4,5
    setupInstanceAttrib(2, offsetof(InstanceData, modelMatrix) + 0 * matRow);
    setupInstanceAttrib(3, offsetof(InstanceData, modelMatrix) + 1 * matRow);
    setupInstanceAttrib(4, offsetof(InstanceData, modelMatrix) + 2 * matRow);
    setupInstanceAttrib(5, offsetof(InstanceData, modelMatrix) + 3 * matRow);
    // aInstanceNormal -> locations 6,7,8,9
    setupInstanceAttrib(6, offsetof(InstanceData, normalMatrix) + 0 * matRow);
    setupInstanceAttrib(7, offsetof(InstanceData, normalMatrix) + 1 * matRow);
    setupInstanceAttrib(8, offsetof(InstanceData, normalMatrix) + 2 * matRow);
    setupInstanceAttrib(9, offsetof(InstanceData, normalMatrix) + 3 * matRow);
    // aInstanceColor  -> location 10 (rgb albedo; a = floor-grid flag)
    setupInstanceAttrib(10, offsetof(InstanceData, color));
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    m_cubeIndexCount = (GLsizei)(sizeof(idx) / sizeof(idx[0]));
    glBindVertexArray(0);

    // --- Simple lit shader (compiled from embedded source, like Skybox) ---
    m_shader = std::make_unique<Shader>(kArenaVertexSrc, kArenaFragmentSrc, /*fromString=*/true);
    if (m_shader->ID == 0) {
        std::cerr << "[Arena] Failed to compile arena shader\n";
        m_shader.reset();
        shutdown();
        return false;
    }

    m_initialized = true;
    buildWalls();  // bakes + uploads the instance list
    std::cout << "[Arena] Initialized — " << m_walls.size() << " walls ("
              << (m_fullHeight * 0.25f) << "m partial, " << m_fullHeight << "m full), "
              << m_instanceData.size() << " instances, radius=" << m_radius << "m\n";
    return true;
}

void ArenaScene::shutdown() {
    m_shader.reset();
    if (m_instanceVBO)     { glDeleteBuffers(1, &m_instanceVBO); m_instanceVBO = 0; }
    if (m_cubeEBO)         { glDeleteBuffers(1, &m_cubeEBO); m_cubeEBO = 0; }
    if (m_cubeVBO)         { glDeleteBuffers(1, &m_cubeVBO); m_cubeVBO = 0; }
    if (m_cubeVAO)         { glDeleteVertexArrays(1, &m_cubeVAO); m_cubeVAO = 0; }
    m_cubeIndexCount = 0;
    m_walls.clear();
    m_floor.clear();
    m_instanceData.clear();
    m_initialized = false;
}

// ===========================================================================
// Wall / floor layout
// ===========================================================================

void ArenaScene::buildWalls() {
    m_walls.clear();
    m_floor.clear();

    // --- Ground platform: a single wide, flat cube (floor => grid enabled) ---
    {
        CubeInstance f;
        f.position = {0.0f, -0.02f, 0.0f};  // just below 0 so it sits flush
        f.scale    = {m_groundSize, 0.04f, m_groundSize};
        f.color    = m_floorColor;
        m_floor.push_back(f);
    }

    // --- Circular wall ring with Tangential Facing Rotations ---
    // forArena.txt Phase 2: every segment is translated to its spot on the
    // ring but NOT rotated, so all walls face parallel to world +Z (a jagged
    // stepped pattern). Fix: rotate each by -angle around Y so the wall
    // width arcs smoothly along the curvature.
    constexpr int kSegs = 32;
    const float halfH     = m_fullHeight * 0.5f;
    const float quarterH  = m_fullHeight * 0.25f;
    const float segAngle  = (2.0f * 3.14159265f) / kSegs;

    for (int i = 0; i < kSegs; ++i) {
        float angle = segAngle * i;
        float cx = std::cos(angle) * m_radius;
        float cz = std::sin(angle) * m_radius;
        float arcLen = m_radius * segAngle * 0.92f;

        CubeInstance w;
        w.rotationY = -angle; // twist inward to follow the ring curvature

        // Every 3rd segment (slots 0, 3, 6, ...) is FULL height.
        // Slots 1, 4, 7, ... are PARTIAL (lower) walls.
        // Slots 2, 5, 8, ... are open (gap — no wall).
        if (i % 3 == 1) {
            w.position = {cx, quarterH * 0.5f, cz};
            w.scale    = {arcLen, quarterH, m_thickness};
            w.color    = m_partialHeightColor;
            m_walls.push_back(w);
        } else if (i % 3 == 0) {
            w.position = {cx, halfH, cz};
            w.scale    = {arcLen, m_fullHeight, m_thickness};
            w.color    = m_fullHeightColor;
            m_walls.push_back(w);
        }
    }

    // --- Central Obstacle Suite: standard step-height verification ledges ---
    // Increments of 0.2m/0.4m/0.6m to probe character stepping limits.
    for (int step = 0; step < 3; ++step) {
        CubeInstance s;
        float height = 0.2f * (step + 1);
        s.scale = {2.0f, height, 0.5f};
        s.position = {-3.0f, height * 0.5f, -2.0f + (step * 0.5f)};
        s.color = glm::vec3(0.35f, 0.45f, 0.55f); // tech-blue accent
        m_walls.push_back(s);
    }

    // --- Central Obstacle Suite: variable-angle slope/ramp ---
    // A long block rotated along X to evaluate climb thresholds / slide states.
    {
        CubeInstance ramp;
        ramp.scale = {2.5f, 0.2f, 5.0f};
        ramp.position = {3.0f, 1.0f, 0.0f};
        ramp.rotationX = glm::radians(30.0f);
        ramp.color = glm::vec3(0.4f, 0.5f, 0.4f); // muted green accent
        m_walls.push_back(ramp);
    }

    // --- Procedural Randomized Obstacle Generation (forArena.txt Part 2) ---
    // Scatter random obstacle blocks inside the ring (20%-80% of m_radius) so
    // the arena reads as a "sandbox" rather than an empty ring. They live in
    // m_walls, so they share the single instanced draw call (no extra CPU->GPU
    // cost) and the per-instance normal matrix / winding fix already cover them.
    static bool seeded = false;
    if (!seeded) {
        std::srand(static_cast<unsigned int>(std::time(nullptr)));
        seeded = true;
    }
    for (int i = 0; i < m_obstacleCount; ++i) {
        CubeInstance obs;

        // Random polar placement within the arena's inner radius.
        const float angle = (static_cast<float>(std::rand()) / RAND_MAX) * 2.0f * 3.14159265f;
        const float minRange = m_radius * 0.2f;
        const float maxRange = m_radius * 0.8f;
        const float distance = minRange + (static_cast<float>(std::rand()) / RAND_MAX) * (maxRange - minRange);
        const float x = std::cos(angle) * distance;
        const float z = std::sin(angle) * distance;

        // Keep obstacles out of the central stairs/ramp testing suite so the
        // step-height and climb probes stay accessible. (Coordinates match the
        // suite placed just above.)
        if ((x < 0.0f && x > -5.0f && z < 0.0f && z > -4.0f) ||   // stairs
            (x > 0.0f && x < 5.0f  && z > -2.0f && z < 3.0f)) {   // ramp
            continue;
        }

        // Randomized dimensions (meters).
        const float w = 1.0f + (static_cast<float>(std::rand()) / RAND_MAX) * 2.0f; // 1-3m
        const float h = 0.5f + (static_cast<float>(std::rand()) / RAND_MAX) * 3.5f; // 0.5-4m
        const float d = 1.0f + (static_cast<float>(std::rand()) / RAND_MAX) * 2.0f; // 1-3m
        obs.scale = {w, h, d};

        // Pivot from the base so each block sits flush on the floor plane.
        obs.position = {x, h * 0.5f, z};
        obs.rotationY = (static_cast<float>(std::rand()) / RAND_MAX) * 2.0f * 3.14159265f; // organic yaw
        obs.rotationX = 0.0f; // stay upright

        // Tech-blue -> industrial-grey tint family.
        const float tint = (static_cast<float>(std::rand()) / RAND_MAX);
        obs.color = glm::vec3(0.3f + tint * 0.1f,
                              0.35f + tint * 0.1f,
                              0.45f + tint * 0.1f);

        m_walls.push_back(obs);
    }

    // Bake all instances into a single flat buffer + one instanced draw call.
    uploadInstances();

    std::cout << "[Arena] Built — " << m_walls.size() << " walls + " << m_floor.size()
              << " floor (" << m_instanceData.size() << " instances), radius="
              << m_radius << "m\n";
}

void ArenaScene::uploadInstances() {
    // Rebuild the flat per-instance list (CPU normal matrix computed here).
    m_instanceData.clear();
    m_instanceData.reserve(m_floor.size() + m_walls.size());
    for (const auto& f : m_floor) m_instanceData.push_back(makeInstanceData(f, true));
    for (const auto& w : m_walls) m_instanceData.push_back(makeInstanceData(w, false));

    // Stream the baked payloads into the instance VBO. Guarded so rebuild()
    // (called from the UI) is safe even before the GL buffers exist.
    if (!m_instanceVBO || m_instanceData.empty()) return;
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(m_instanceData.size() * sizeof(InstanceData)),
                 m_instanceData.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
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
    if (m_instanceData.empty()) return;

    m_shader->use();
    m_shader->setMat4("uView", view);
    m_shader->setMat4("uProjection", projection);
    m_shader->setVec3("uViewPos", cameraPos);
    m_shader->setVec3("uLightDir", glm::normalize(glm::vec3(0.3f, 1.0f, 0.4f)));     // warm sun
    m_shader->setVec3("uLightColor", glm::vec3(1.0f, 0.95f, 0.85f));
    m_shader->setFloat("uAmbient", 0.25f);

    glEnable(GL_DEPTH_TEST);
    // FIX (forArena.txt Fix 2): Ensure depth writes are enabled for instanced
    // overlays. A previous pass (e.g. deferred lighting / skybox) may have
    // disabled glDepthMask, which would cause arena cubes to fail the depth
    // test against existing geometry — producing the "missing faces" /
    // shimmering symptom. Re-enabling writes guarantees correct occlusion.
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // forArena Phase 1: the entire arena (walls + floor + props + ramp) is now
    // a SINGLE instanced draw call. The old loop did one glDrawElements per
    // segment (dozens of CPU->GPU transitions per frame); now the per-instance
    // model/normal/color live in m_instanceVBO and only the camera uniforms are
    // set per-frame.
    glBindVertexArray(m_cubeVAO);
    glDrawElementsInstanced(GL_TRIANGLES, m_cubeIndexCount, GL_UNSIGNED_SHORT,
                            nullptr, static_cast<GLsizei>(m_instanceData.size()));
    glBindVertexArray(0);
    glUseProgram(0);
}

} // namespace Arena
