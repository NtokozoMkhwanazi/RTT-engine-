#include "SimpleWorldRenderer.h"
#include "shaderSystem/Shader.h"
#include "lighting/LightingEnvironment.h"   // authoritative sun / lighting standard
#include "TerrainOptimizations.h"
#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>  // for glm::inverse (viewPos derivation)
#include <chrono>
#include <iostream>
#include <unordered_map>

SimpleWorldRenderer::SimpleWorldRenderer() {
}

SimpleWorldRenderer::~SimpleWorldRenderer() {
}

int SimpleWorldRenderer::loadModel(const std::string& path, size_t maxTrianglesPerMesh) {
    // Dedupe by path: several object types reference the same asset (e.g. all
    // three tree variants use quiver_tree/q.gltf). Loading it once instead of
    // 3x avoids re-decoding/re-compressing the same 4K textures - a ~45s
    // startup win with the heavy glTF plants. The first caller's budget wins
    // (plant types pass a small budget; everything else uses the default).
    auto existing = m_modelByPath.find(path);
    if (existing != m_modelByPath.end()) {
        return existing->second;
    }

    auto model = std::make_shared<Model>(path);

    if (!model || model->GetMeshCount() == 0) {
        std::cerr << "[SimpleWorldRenderer] Failed to load: " << path << "\n";
        return -1;
    }

    // The prop assets are heavy (quiver_tree ~82k tris, othonna ~118k, boulder
    // ~98k) and ~675 instances are scattered across the world - at full draw
    // that is ~47M triangles per frame, which pinned horizontal camera views
    // to 15-18fps on integrated GPUs while top-down (few visible instances)
    // ran at 60. Decimate each mesh once at load to a sane budget so the whole
    // scene fits comfortably in a 60fps frame. The bot and other small meshes
    // are untouched (below the budget).
    for (size_t i = 0; i < model->GetMeshCount(); ++i) {
        MeshUtils::DecimateStaticMesh(model->GetMesh(i), maxTrianglesPerMesh);
    }

    const int id = (int)m_models.size();
    m_models.push_back(model);
    m_modelWindStrength.push_back(0.0f);
    m_modelByPath[path] = id;
    return id;
}

void SimpleWorldRenderer::setWindStrength(int modelId, float strength) {
    if (modelId < 0 || modelId >= (int)m_modelWindStrength.size()) return;
    m_modelWindStrength[modelId] = strength;
}

void SimpleWorldRenderer::addInstance(int modelId, const glm::vec3& position,
                                       float scale, float rotationY,
                                       const glm::vec4& colorTint) {
    if (modelId < 0 || modelId >= (int)m_models.size()) {
        std::cerr << "[SimpleWorldRenderer] Invalid model ID: " << modelId << "\n";
        return;
    }

    SimpleModelInstance inst;
    inst.model = m_models[modelId];
    inst.color = colorTint;

    // Build model matrix
    inst.modelMatrix = glm::translate(glm::mat4(1.0f), position);
    inst.modelMatrix = glm::rotate(inst.modelMatrix, glm::radians(rotationY),
                                   glm::vec3(0, 1, 0));
    inst.modelMatrix = glm::scale(inst.modelMatrix, glm::vec3(scale));

    m_instances.push_back(inst);
}

void SimpleWorldRenderer::cullDistant(const glm::vec3& cameraPos, float maxDist,
                                      float respawnDist) {
    // Park instances beyond the radius in the dormant cache (non-destructive).
    auto it = m_instances.begin();
    while (it != m_instances.end()) {
        const glm::vec3 pos(it->modelMatrix[3]);
        if (glm::distance(cameraPos, pos) > maxDist) {
            m_dormant.push_back(std::move(*it));
            it = m_instances.erase(it);
        } else {
            ++it;
        }
    }
    // Respawn cached instances the camera has come back within the respawn
    // radius of (default 85% of maxDist - hysteresis so an object straddling
    // the boundary never flickers in and out).
    if (respawnDist < 0.0f) respawnDist = maxDist * 0.85f;
    auto d = m_dormant.begin();
    while (d != m_dormant.end()) {
        const glm::vec3 pos(d->modelMatrix[3]);
        if (glm::distance(cameraPos, pos) <= respawnDist) {
            m_instances.push_back(std::move(*d));
            d = m_dormant.erase(d);
        } else {
            ++d;
        }
    }
}

void SimpleWorldRenderer::clearAll() {
    // Permanently wipe the active instances AND the dormant respawn cache
    // (the old cull-everything hack only emptied the active list and leaked
    // the cached instances).
    m_instances.clear();
    m_dormant.clear();
}

void SimpleWorldRenderer::render(const glm::mat4& view, const glm::mat4& projection,
                                 const LightingEnvironment& lighting) {
    static int s_diagCount = 0;
    if (++s_diagCount % 120 == 1 && !m_instances.empty()) {
        glm::vec3 camPos = glm::inverse(view) * glm::vec4(0, 0, 0, 1);
        int nearObj = 0;
        float minDist = 1e9f;
        glm::vec3 minPos(0.0f);
        for (const auto& inst : m_instances) {
            glm::vec3 pos = glm::vec3(inst.modelMatrix[3]);
            float d = glm::distance(camPos, pos);
            if (d < minDist) { minDist = d; minPos = pos; }
            if (d < 100.0f) nearObj++;
        }
        std::cout << "[WorldObjects] instances=" << m_instances.size()
                  << " cam=(" << (int)camPos.x << "," << (int)camPos.y
                  << "," << (int)camPos.z << ") nearest=("
                  << (int)minPos.x << "," << (int)minPos.y << ","
                  << (int)minPos.z << ") dist=" << (int)minDist
                  << " within100m=" << nearObj << "\n";
    }
    if (m_instances.empty()) {
        return;
    }

    // Use robust frustum culler for optimizations
    TerrainOptimizations::FrustumCuller culler;
    culler.update(projection * view);

    // Create a simple shader for untextured models
    static Shader* simpleShader = nullptr;
    if (!simpleShader) {
        simpleShader = new Shader("shaderSystem/VS.glsl", "shaderSystem/FS.glsl");
    }
    // Lazy-init the water renderer (GL context is live this frame). Renders a
    // transparent plane at `water_level` (CVar) lit by the canonical env sun.
    if (!m_water) {
        m_water = std::make_unique<WaterRenderer>();
        m_water->init();
    }

    simpleShader->use();
    simpleShader->setMat4("projection", projection);
    simpleShader->setMat4("view", view);
    simpleShader->setVec3("color", glm::vec3(0.4f, 0.35f, 0.3f));
    // Vegetation light direction: a distant point along the canonical sun
    // (LightingEnvironment::Instance().sunDirection = surface->sun, the single
    // value shared with the terrain splat + deferred meshes), so trees/plants/
    // grasses catch light from the same angle as the ground. Far placement
    // makes normalize(lightPos - FragPos) ~ constant => a directional sun.
    simpleShader->setVec3("lightPos",
        lighting.sunDirection * 10000.0f);  // #3: env threaded, not Instance()

    // Camera position for the view-dependent foliage subsurface term in FS.glsl.
    // Derive it from the view matrix (inverse-view translation) so we don't
    // have to thread cameraPos through the render signature.
    simpleShader->setVec3("viewPos", glm::vec3(glm::inverse(view)[3]));

    // Wind animation time (seconds since first render).
    static const auto s_windStart = std::chrono::steady_clock::now();
    const float windTime = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - s_windStart).count();
    simpleShader->setFloat("uTime", windTime);
    simpleShader->setVec3("uWindDir", glm::vec3(0.5f, 0.0f, 0.3f));

    // Group instances by model and render each group instanced. The old
    // version rebuilt a fresh std::unordered_map (with per-frame allocations
    // keyed by shared_ptr<Model>) every frame for ~600 instances; iterate the
    // fixed model list with a reusable scratch vector instead - the per-frame
    // work is now just the frustum cull + an in-place instance-buffer update
    // (Mesh::DrawInstanced uses glBufferSubData, see mesh.cpp).
    for (size_t mi = 0; mi < m_models.size(); ++mi) {
        const auto& model = m_models[mi];
        m_scratch.clear();
        for (const auto& inst : m_instances) {
            if (inst.model != model) continue;
            // Frustum culling per instance (skip the test entirely when the
            // editor has disabled frustum culling globally).
            const glm::vec3 pos = glm::vec3(inst.modelMatrix[3]);
            if (m_frustumCulling && !culler.isSphereInFrustum(pos, 5.0f)) continue;
            InstanceData data;
            data.ModelMatrix = inst.modelMatrix;
            data.Color = inst.color;
            m_scratch.push_back(data);
        }
        if (m_scratch.empty()) continue;

        // Per-model wind strength (grass sways, rocks stay put).
        float wind = (mi < m_modelWindStrength.size()) ? m_modelWindStrength[mi] : 0.0f;
        simpleShader->setFloat("uWindStrength", wind);
        // Subsurface scattering: only leafy plants (wind-enabled) get sunlit-back
        // translucency; rocks/trunks report wind 0 so they stay opaque.
        simpleShader->setFloat("uSubsurface", wind > 0.0f ? 0.6f : 0.0f);

        for (size_t meshIdx = 0; meshIdx < model->GetMeshCount(); meshIdx++) {
            Mesh& mesh = const_cast<Mesh&>(model->GetMesh(meshIdx));
            mesh.DrawInstanced(*simpleShader, m_scratch);
        }
    }

    // Water plane (transparent, depth-tested no-write) rendered on top of the
    // opaque scene. Gated on m_renderWater (default false) so the placeholder
    // flat water no longer hides the terrain once the camera is properly framed.
    if (m_water && m_renderWater) {
        m_water->render(view, projection,
                        glm::vec3(glm::inverse(view)[3]), windTime, lighting);
    }
}

size_t SimpleWorldRenderer::getInstanceCount() const {
    return m_instances.size();
}

glm::vec3 SimpleWorldRenderer::getModelSize(int modelId) const {
    if (modelId < 0 || modelId >= (int)m_models.size() || !m_models[modelId]) {
        return glm::vec3(1.0f);
    }
    const glm::vec3 size = m_models[modelId]->GetSize();
    return (size.y > 0.01f) ? size : glm::vec3(1.0f);
}

BoundingBox SimpleWorldRenderer::getModelBounds(int modelId) const {
    if (modelId < 0 || modelId >= (int)m_models.size() || !m_models[modelId]) {
        return BoundingBox{};
    }
    return m_models[modelId]->GetBoundingBox();
}
