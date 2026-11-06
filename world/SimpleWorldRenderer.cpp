#include "SimpleWorldRenderer.h"
#include "ecs/System.h"        // complete ecs::System (must precede JobSystem.h —
                               // JobSystem.h's SystemExecutor inline bodies need it)
#include "ecs/JobSystem.h"     // bounded worker pool + SystemExecutor
#include "shaderSystem/Shader.h"
#include "lighting/LightingEnvironment.h"   // authoritative sun / lighting standard
#include "TerrainOptimizations.h"
#include <glad/glad.h>
#include <limits>
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>  // for glm::inverse (viewPos derivation)
#include <chrono>
#include <iostream>
#include <unordered_map>

SimpleWorldRenderer::SimpleWorldRenderer() {
    // Lazy-construct the bounded prop-parse pool on first render-system init.
    // Worker threads spawn here; they are joined on destruction (see dtor).
    m_jobSystem = std::make_unique<ecs::JobSystem>();
    m_jobSystem->init();
}

SimpleWorldRenderer::~SimpleWorldRenderer() {
    // Join parse workers before teardown so no background thread touches the GL
    // context or slot state after destruction begins.
    if (m_jobSystem) {
        m_jobSystem->shutdown();
    }
}

int SimpleWorldRenderer::loadModel(const std::string& path, size_t maxTrianglesPerMesh) {
    // Guard against directory paths before Assimp ever sees them: an empty or
    // "assets/"-style world-object entry (a cleared Model field in the World
    // Settings panel saves as just the asset dir) is a DIRECTORY, and Assimp
    // answers it with the misleading
    //   ASSIMP ERROR: No suitable reader found for the file format of file "assets/".
    // A directory can never be a model, so reject it with one accurate line.
    if (path.empty() || path.back() == '/' || path.back() == '\\') {
        std::cerr << "[SimpleWorldRenderer] Ignoring model path \"" << path
                  << "\" (empty or a directory, not a model file)\n";
        return -1;
    }

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

int SimpleWorldRenderer::registerParsedModel(const std::string& path, Model* model,
                                             size_t maxTrianglesPerMesh, float windSway) {
    if (!model || model->GetMeshCount() == 0) {
        delete model;
        std::cerr << "[SimpleWorldRenderer] registerParsedModel: empty model " << path << "\n";
        return -1;
    }

    // Dedup by path: several prop types share one asset (e.g. the three tree
    // variants all use quiver_tree/q.gltf). Reuse the already-registered model
    // and drop the duplicate instead of re-decimating + re-uploading. The caller
    // (WOM commit) keeps the wind/base-scale bookkeeping keyed on the returned id.
    auto existing = m_modelByPath.find(path);
    if (existing != m_modelByPath.end()) {
        delete model;
        return existing->second;
    }

    // Decimate each mesh to the budget. DecimateStaticMesh calls
    // UpdateVertexBuffer/UpdateIndexBuffer (GL buffer re-submission), so this
    // MUST run on the main thread that owns the GL context — i.e. inside the
    // commit pump, not on the parse worker.
    for (size_t i = 0; i < model->GetMeshCount(); ++i) {
        MeshUtils::DecimateStaticMesh(model->GetMesh(i), maxTrianglesPerMesh);
    }

    const int id = (int)m_models.size();
    m_models.push_back(std::shared_ptr<Model>(model));
    m_modelWindStrength.push_back(windSway);
    m_modelByPath[path] = id;
    return id;
}

void SimpleWorldRenderer::beginAsyncModelLoad(const std::string& typeKey,
                                              const std::string& path,
                                              size_t maxTrianglesPerMesh,
                                              float windSway,
                                              int maxTextureSize) {
    // Path-level dedup: parse each unique asset once; every typeKey sharing
    // that path reuses the SAME handle so the GL upload + bookkeeping happen
    // once and the single model id is handed back to each key on commit.
    for (auto& slot : m_worldAsync.slots) {
        if (slot.path == path) {
            // Reuse: append this key to the path's slot (idempotent on re-add).
            slot.typeKeys.push_back(typeKey);
            return;
        }
    }

    m_worldAsync.isLoading.store(true, std::memory_order_relaxed);
    m_worldAsync.loadFinished.store(false, std::memory_order_relaxed);

    // Dispatch the heavy Assimp parse + texture decode onto the bounded JobSystem
    // pool (NOT std::async per-model). The worker writes progress into a shared
    // atom and delivers the parsed AsyncModelData through a std::promise whose
    // future lives in the slot; on the main thread we only poll JobHandle::
    // isComplete() (non-blocking) and then consume dataFuture.get() (already
    // ready, so also non-blocking). This caps parse concurrency to ~3 workers
    // instead of one OS thread per asset, which is what pinned the editor to
    // ~2-3 FPS when a whole prop set parsed at once.
    auto dataPromise = std::make_shared<std::promise<std::unique_ptr<AsyncModelData>>>();
    auto progressAtom = std::make_shared<std::atomic<float>>(0.0f);

    AsyncLoadSlot slot;
    slot.path = path;
    slot.maxTrianglesPerMesh = maxTrianglesPerMesh;
    slot.windSway = windSway;
    slot.maxTextureSize = maxTextureSize;
    slot.typeKeys.push_back(typeKey);
    slot.dataFuture = dataPromise->get_future();
    slot.progress = progressAtom;

    const std::string assetPath = path;           // copy for the worker
    const int texCap = maxTextureSize;            // copy for the worker
    // Dispatch to the bounded pool; the returned JobHandle is intentionally
    // discarded — completion is observed via `slot.dataFuture` (the promise is
    // fulfilled inside the job, before the pool's own completion future).
    m_jobSystem->addJob([assetPath, dataPromise, progressAtom, texCap]() {
        try {
            dataPromise->set_value(Model::LoadModelData(assetPath, progressAtom.get(), texCap));
        } catch (const std::exception& e) {
            auto result = std::make_unique<AsyncModelData>();
            result->success = false;
            result->error = std::string("Exception: ") + e.what();
            dataPromise->set_value(std::move(result));
        } catch (...) {
            auto result = std::make_unique<AsyncModelData>();
            result->success = false;
            result->error = "Unknown exception during async loading";
            dataPromise->set_value(std::move(result));
        }
    }, /*priority=*/0);
    m_worldAsync.slots.push_back(std::move(slot));
    m_worldAsync.totalModels.store((int)m_worldAsync.slots.size(),
                                   std::memory_order_relaxed);
}

float SimpleWorldRenderer::commitAsyncModels() {
    if (!m_worldAsync.isLoading.load(std::memory_order_relaxed)) return 1.0f;
    if (m_worldAsync.loadFinished.load(std::memory_order_relaxed)) return 1.0f;

    // TIME-SLICE the main-thread GL work (Model::CreateFromAsync uploads the
    // textures/buffers for a model + DecimateStaticMesh re-submits GL buffers).
    // Without this, every worker parse that finishes together commits in ONE
    // frame -> for the real prop assets (quiver_tree 82k tris, othonna 118k,
    // boulder 98k, ...) that is a multi-hundred-ms frame and the editor drops
    // to ~2 FPS while the "Resolving..." bar is up. Each call only does as
    // much GL work as fits in a small CPU budget (~4 ms), committing at least
    // one ready model per frame so it always makes forward progress; the rest
    // rides over to the next frame and the bar advances smoothly at 60 FPS.
    static constexpr double kBudgetMs = 4.0;
    int committedThisFrame = 0;
    auto frameStart = std::chrono::steady_clock::now();
    const auto budgetElapsed = [&]() {
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now() - frameStart).count();
    };

    for (auto& slot : m_worldAsync.slots) {
        if (slot.committed || slot.failed) continue;
        // Still parsing on the worker pool: poll the promise (non-blocking; the
        // JobSystem completion future is strictly later than our promise).
        if (slot.dataFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;

        // Budget gate: once we've committed at least one model this frame, stop
        // as soon as the CPU budget is exhausted (keeps the render thread free).
        if (committedThisFrame > 0 && budgetElapsed() > kBudgetMs) continue;

        // ---- HEAVY MAIN-THREAD GL work: upload buffers/textures + decimate ----
        // dataFuture is guaranteed ready (the JobSystem promise is fulfilled
        // before the JobHandle's own completion future, which we just tested).
        auto parsedData = slot.dataFuture.get();
        if (!parsedData || !parsedData->success) {
            slot.failed = true;
            slot.error = (!parsedData || parsedData->error.empty())
                ? "CPU parse reported failure" : parsedData->error;
            m_worldAsync.failedMessage = slot.path + ": " + slot.error;
            continue;
        }

        std::string err;
        Model* glModel = Model::CreateFromAsync(std::move(parsedData), &err);
        if (!glModel) {
            slot.failed = true;
            slot.error = err.empty() ? "GPU upload reported failure" : err;
            m_worldAsync.failedMessage = slot.path + ": " + slot.error;
            continue;
        }

        const int id = registerParsedModel(slot.path, glModel,
                                           slot.maxTrianglesPerMesh, slot.windSway);
        if (id < 0) {
            slot.failed = true;
            slot.error = "registration rejected";
            m_worldAsync.failedMessage = slot.path + ": " + slot.error;
            continue;
        }

        slot.modelId = id;
        slot.committed = true;
        ++committedThisFrame;
        // Hand the single model id to EVERY typeKey backed by this asset.
        for (const auto& tk : slot.typeKeys) {
            m_worldAsync.pendingResults.push_back(
                WorldModelResult{tk, slot.path, id, slot.windSway});
        }
        std::cout << "[SimpleWorldRenderer] async model committed: " << slot.path
                  << " (id=" << id << ")\n";
    }

    // Recompute aggregates from flags so the per-frame budget doesn't skew the
    // committed/failed totals (some parse-complete models may still be queued).
    int committed = 0, failed = 0, parseReady = 0;
    float progressSum = 0.0f;   // live worker progress for in-flight parses
    for (const auto& slot : m_worldAsync.slots) {
        if (slot.committed) { ++committed; }
        else if (slot.failed) { ++failed; }
        else if (slot.dataFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) { ++parseReady; }   // parsed, GL upload queued
        else {
            // In-flight: contribute its live worker parse progress (0..1).
            float p = slot.progress
                ? slot.progress->load(std::memory_order_relaxed) : 0.0f;
            progressSum += p;
        }
    }

    const int total = (int)m_worldAsync.slots.size();
    m_worldAsync.committedModels.store(committed, std::memory_order_relaxed);
    m_worldAsync.failedModels.store(failed, std::memory_order_relaxed);

    // Aggregate progress: committed/failed = 1.0 each; parse-complete-but-
    // awaiting GL upload = 0.9 (almost done — worker finished, upload queued);
    // in-flight = its live worker progress (0..1).
    float frac = 0.0f;
    if (total > 0) {
        frac = (float)(committed + failed) / (float)total;
        frac += 0.9f * (float)parseReady / (float)total;
        if (total > (committed + failed + parseReady)) {
            frac += progressSum / (float)total;
        }
        if (frac > 1.0f) frac = 1.0f;
    }
    m_worldAsync.progress.store(frac, std::memory_order_relaxed);

    if (committed + failed >= total) {
        m_worldAsync.loadFinished.store(true, std::memory_order_relaxed);
        m_worldAsync.isLoading.store(false, std::memory_order_relaxed);
        if (failed > 0) {
            m_worldAsync.failed.store(true, std::memory_order_relaxed);
        }
        std::cout << "[SimpleWorldRenderer] async model load complete: "
                  << committed << " ok, " << failed << " failed\n";
    }
    return frac;
}

std::vector<SimpleWorldRenderer::WorldModelResult> SimpleWorldRenderer::drainCommittedModelIds() {
    auto out = std::move(m_worldAsync.pendingResults);
    m_worldAsync.pendingResults.clear();
    return out;
}

void SimpleWorldRenderer::cancelAsyncModelLoads() {
    m_worldAsync.slots.clear();
    m_worldAsync.totalModels.store(0, std::memory_order_relaxed);
    m_worldAsync.committedModels.store(0, std::memory_order_relaxed);
    m_worldAsync.failedModels.store(0, std::memory_order_relaxed);
    m_worldAsync.isLoading.store(false, std::memory_order_relaxed);
    m_worldAsync.loadFinished.store(false, std::memory_order_relaxed);
    m_worldAsync.failed.store(false, std::memory_order_relaxed);
    m_worldAsync.pendingResults.clear();
    m_worldAsync.progress.store(0.0f, std::memory_order_relaxed);
    m_worldAsync.failedMessage.clear();
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
    // opaque scene. Sun direction/color + fog are pulled from the same
    // LightingEnvironment the terrain and deferred meshes use, so the water
    // catches the one canonical sun instead of a separate, mismatched light.
    if (m_water) {
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

// forPlayer.txt Theme 3, Method 1: instanced-prop raycast. Slab AABB-ray test
// per instance - mirrors PhysicsWorld::raycast (physicsSystem/physics.cpp:94),
// but tests each Model's bounding box (Model::GetBoundingBox) scaled by the
// instance's uniform scale and translated by its world position, recovered
// from the instance modelMatrix (col3 = position, column lengths = per-axis
// scale, matching addInstance's translate*rotateY*scale composition). The prop
// hit set is intentionally separate from PhysicsWorld's rigid-body raycast:
// foliage / plants have no collider but must still be pickable.
bool SimpleWorldRenderer::raycastProps(
    const glm::vec3& origin, const glm::vec3& direction, float maxDist,
    float& outDist, std::size_t& outIndex,
    glm::vec3& outHitPoint, glm::vec3& outHitNormal,
    uint32_t pickableLayerMask) const {
    outIndex = 0;
    outDist = maxDist;
    bool hitAnything = false;

    for (std::size_t i = 0; i < m_instances.size(); ++i) {
        const SimpleModelInstance& inst = m_instances[i];
        if (!inst.model) continue;

        // verify.txt Module 3: skip instances whose layer is not in the
        // pickable-layer mask (locked / isolated layers are raycast-locked).
        if (!((pickableLayerMask >> inst.layer) & 1u)) continue;

        const BoundingBox& bb = inst.model->GetBoundingBox();
        if (!bb.IsValid()) continue;  // degenerate model - cannot be picked

        // Instance world placement (see addInstance: translate * rotateY * scale).
        const float sx = glm::length(inst.modelMatrix[0]);
        const float sy = glm::length(inst.modelMatrix[1]);
        const float sz = glm::length(inst.modelMatrix[2]);
        const glm::vec3 instPos(inst.modelMatrix[3]);
        const glm::vec3 halfExtents =
            glm::vec3(bb.max - bb.min) * 0.5f * glm::vec3(sx, sy, sz);
        const glm::vec3 bmin = instPos - halfExtents;
        const glm::vec3 bmax = instPos + halfExtents;

        // Slab AABB-ray test (PhysicsWorld::raycast style, physics/physics.cpp:94).
        float tmin = -std::numeric_limits<float>::max();
        float tmax =  std::numeric_limits<float>::max();
        bool outside = false;
        for (int a = 0; a < 3; ++a) {
            if (std::abs(direction[a]) < 1e-8f) {
                if (origin[a] < bmin[a] || origin[a] > bmax[a]) { outside = true; break; }
            } else {
                float t1 = (bmin[a] - origin[a]) / direction[a];
                float t2 = (bmax[a] - origin[a]) / direction[a];
                if (t1 > t2) std::swap(t1, t2);
                if (t1 > tmin) tmin = t1;
                if (t2 < tmax) tmax = t2;
                if (tmin > tmax || tmax < 0.0f) { outside = true; break; }
            }
        }
        if (outside) continue;
        if (tmin < 0.0f) continue;       // box is behind the ray origin
        if (tmin >= outDist) continue;   // not the closest hit so far

        outDist = tmin;
        outIndex = (std::size_t)i;
        outHitPoint = origin + direction * tmin;
        outHitNormal = (instPos != outHitPoint)
            ? glm::normalize(outHitPoint - instPos)   // radial, matches PhysicsWorld::raycast
            : glm::vec3(0.0f, 1.0f, 0.0f);
        hitAnything = true;
    }
    return hitAnything;
}
