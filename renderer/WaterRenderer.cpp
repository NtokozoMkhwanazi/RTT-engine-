#include "renderer/WaterRenderer.h"
#include "shaderSystem/Shader.h"
#include "lighting/LightingEnvironment.h"
#include "lighting/CVar.h"
#include <glm/gtc/matrix_transform.hpp>
#include <chrono>
#include <iostream>
#include <vector>

WaterRenderer::WaterRenderer() {}

WaterRenderer::~WaterRenderer() {
    if (m_ready) {
        glDeleteVertexArrays(1, &m_vao);
        glDeleteBuffers(1, &m_vbo);
        glDeleteBuffers(1, &m_ebo);
    }
    delete m_shader;
}

bool WaterRenderer::init() {
    // Construct the (procedural) water shader. Like the rest of the engine this
    // happens lazily on first render, after the GL context is live.
    m_shader = new Shader("shaderSystem/waterVS.glsl", "shaderSystem/waterFS.glsl");
    createGrid(40, 400.0f);   // 40x40 grid over a 400m square
    m_ready = (m_vao != 0) && (m_shader != nullptr);
    float lvl = CVar::Instance().getFloat("water_level", 0.0f);
    std::cout << "[Water] renderer ready | level=" << lvl
              << " (tune cvar 'water_level') | grid_tris="
              << (m_indexCount / 3) << "\n";
    return m_ready;
}

void WaterRenderer::createGrid(int segments, float size) {
    const int N = segments + 1;
    const float h = size * 0.5f;
    std::vector<float> verts;
    verts.reserve((size_t)N * N * 5);
    for (int z = 0; z < N; ++z) {
        float fz = -h + size * z / (N - 1);
        for (int x = 0; x < N; ++x) {
            float fx = -h + size * x / (N - 1);
            verts.push_back(fx); verts.push_back(0.0f); verts.push_back(fz);
            verts.push_back((float)x / (N - 1));
            verts.push_back((float)z / (N - 1));
        }
    }
    std::vector<unsigned int> idx;
    idx.reserve((size_t)segments * segments * 6);
    for (int gz = 0; gz < segments; ++gz) {
        for (int gx = 0; gx < segments; ++gx) {
            int a = gz * N + gx;
            int b = a + 1;
            int c = a + N;
            int d = c + 1;
            idx.push_back(a); idx.push_back(c); idx.push_back(b);
            idx.push_back(b); idx.push_back(c); idx.push_back(d);
        }
    }

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &m_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(unsigned int), idx.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);  // aPos
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 5, (void*)0);
    glEnableVertexAttribArray(2);  // aTex
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 5, (void*)(3 * sizeof(float)));
    m_indexCount = static_cast<GLuint>(idx.size());
    glBindVertexArray(0);
}

void WaterRenderer::render(const glm::mat4& view, const glm::mat4& projection,
                           const glm::vec3& viewPos, float time,
                           const LightingEnvironment& lighting) {
    if (!m_ready) return;

    // #3: sun/fog come from the env threaded in by the caller (no Instance()).
    const auto& env = lighting;
    // Resolve the water_level handle once (loadFromFile runs during init, so by
    // the first render the handle already points at the configured value); then
    // read it O(1) every frame via the cached id -- no string hashing.
    if (!m_waterLevelHandle.valid())
        m_waterLevelHandle = CVar::Instance().registerFloat("water_level", 0.0f);
    const float waterLevel = m_waterLevelHandle.valid()
        ? CVar::Instance().getFloatFast(m_waterLevelHandle)
        : CVar::Instance().getFloat("water_level", 0.0f);

    glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, waterLevel, 0.0f));
    glm::mat4 mvp   = projection * view * model;

    m_shader->use();
    m_shader->setMat4("uModel",      model);
    m_shader->setMat4("uMVP",        mvp);
    m_shader->setVec3("uViewPos",    viewPos);
    m_shader->setFloat("uTime",      time);
    m_shader->setVec3("uSunDirection", env.sunDirection);
    m_shader->setVec3("uSunColor",     env.sunColor);
    m_shader->setVec3("uFogColor",     env.fogHorizon);

    // Transparent water resting at water_level: blend, depth-test but do NOT
    // write depth, and don't cull (water may be viewed from below). The surface
    // then composites over lower terrain instead of occluding it.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}
