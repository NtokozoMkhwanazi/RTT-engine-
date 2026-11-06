#pragma once
#include "modelSystem/Model.h"
#include "lighting/LightingEnvironment.h"  // #3: thread env to WaterRenderer
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include <atomic>
#include <future>
// `JobSystem` (bounded worker pool for prop parsing) is forward-declared here
// and only fully included in SimpleWorldRenderer.cpp, so the heavy
// SystemExecutor inline bodies it drags in don't force ecs::System onto every
// translation unit that pulls in SimpleWorldRenderer.h.
namespace ecs { class JobSystem; }
#include "renderer/WaterRenderer.h"

// Simple model instance
struct SimpleModelInstance {
    std::shared_ptr<Model> model;
    glm::mat4 modelMatrix;
    glm::vec4 color = glm::vec4(1.0f);  // per-instance tint (vegetation variation)
    // verify.txt Module 3: actor layer group for viewport isolation.
    // Mirrors the editor's layerVisible/layerLocked arrays (0 = Default).
    int layer = 0;
};

// Simple renderer for world objects (trees, rocks, etc.)
class SimpleWorldRenderer {
public:
    SimpleWorldRenderer();
    ~SimpleWorldRenderer();
    
    // Load a model. maxTrianglesPerMesh bounds the per-mesh decimation
    // budget (default 12000 for props; ground plants pass a small budget so
    // hundreds of grass tufts stay cheap).
    int loadModel(const std::string& path, size_t maxTrianglesPerMesh = 12000);

    // ---- Async (worker-parse, main-GL) world-object model loading ----
    // These are the async counterparts to loadModel(): the heavy Assimp parse +
    // texture decode run on a worker thread via Model::LoadAsync, while ONLY the
    // GL buffer/texture upload + decimation happen here on the main thread that
    // owns the GL context. Used by the "layers/objects" bulk rebuild so the
    // editor viewport + UI stay responsive and a live "Resolving..." bar can be
    // surfaced. Call beginAsyncModelLoad() per type, then pump
    // commitAsyncModels() every frame until isAsyncLoadFinished().

    // One in-flight (or committed) async parse. `typeKeys` is a list because a
    // single asset path may back several prop types (e.g. all tree variants
    // share quiver_tree/q.gltf) — parse once, hand the one model id to each key.
    struct AsyncLoadSlot {
        std::string path;
        size_t      maxTrianglesPerMesh = 12000;
        float       windSway = 0.0f;
        std::vector<std::string> typeKeys;
        // Cap decoded textures to this long-axis size (default 2048) so a 4K prop
        // texture's GL upload+mipmap no longer stalls the main-thread commit.
        // 0 = uncapped (matches the sync loadModel path). Applied by the worker.
        int maxTextureSize = 2048;

        // Bounded-pool parse. Unlike Model::LoadAsync (which spawns a transient
        // std::async thread PER asset — for a full prop set that is 6-8 OS
        // threads fighting the render thread and pinning the editor to ~2-3
        // FPS), these jobs run on SimpleWorldRenderer::m_jobSystem, a fixed
        // work-stealing pool sized to ~3 workers. The worker runs LoadModelData
        // (Assimp parse + texture decode) and delivers the parsed AsyncModelData
        // through `dataFuture`; `progress` is the same 0..1 atom the bar reads.
        // Only the GL upload + decimate (CreateFromAsync) runs on the main thread.
        std::future<std::unique_ptr<AsyncModelData>> dataFuture; // ready when parse done
        std::shared_ptr<std::atomic<float>> progress;          // 0..1, worker-written
        bool        committed = false;
        bool        failed = false;
        std::string error;
        int         modelId = -1;                   // set once committed (>=0)
    };
    // Result handed back to the WorldObjectManager once a model is committed,
    // so it can run its (main-thread) string/enum bookkeeping: id + windSway
    // + labels. Expanded once per owned typeKey on commit.
    struct WorldModelResult {
        std::string typeKey;
        std::string path;
        int         modelId = -1;
        float       windSway = 0.0f;
    };

    // Dispatch a non-blocking CPU parse for `path`. `typeKey` is the dynamic
    // string key the WorldObjectManager uses to look the model up later; the
    // parser dedups by path (a shared asset parsed once is reused by every key).
    // `maxTextureSize` (0 = uncapped) caps decoded textures on the worker so 4K
    // prop maps don't blow the main-thread GL commit budget.
    void beginAsyncModelLoad(const std::string& typeKey,
                             const std::string& path,
                             size_t maxTrianglesPerMesh = 12000,
                             float windSway = 0.0f,
                             int maxTextureSize = 2048);

    // Pump one commit step on the MAIN (GL) thread: for every parse that
    // finished since the last call, upload GL buffers (Model::CreateFromAsync),
    // decimate, register the model, and apply wind sway. Returns aggregate load
    // progress 0..1. Safe to call every frame; no-ops when idle.
    float commitAsyncModels();

    // After commitAsyncModels(), every finished parse is registered as a model.
    // Drain the per-type results the WorldObjectManager needs to wire up its
    // bookkeeping (string model ids + base scales): returns the (typeKey,modelId)
    // pairs committed since the last call, then clears them. Empty until the
    // load finishes.
    std::vector<WorldModelResult> drainCommittedModelIds();

    bool isAsyncLoading() const { return m_worldAsync.isLoading.load(std::memory_order_relaxed); }
    bool isAsyncLoadFinished() const { return m_worldAsync.loadFinished.load(std::memory_order_relaxed); }
    float getAsyncLoadProgress() const { return m_worldAsync.progress.load(std::memory_order_relaxed); }
    // Abort an in-flight load (drops worker handles; main thread must not be
    // mid-commit). Used when a rebuild supersedes a previous one.
    void cancelAsyncModelLoads();

    // Add an instance. colorTint multiplies the model's albedo (per-instance
    // vegetation color variation); default white = no tint.
    void addInstance(int modelId, const glm::vec3& position,
                     float scale = 1.0f, float rotationY = 0.0f,
                     const glm::vec4& colorTint = glm::vec4(1.0f));

    // Per-model wind sway strength. Plants (grass/flowers/bushes) get a
    // strong value so their tips sway; trees get a small value; rocks 0.
    // Passed to the shared vertex shader as uWindStrength.
    void setWindStrength(int modelId, float strength);
    
    // Remove instances beyond max distance. NON-destructive: instances beyond
    // the radius are parked in a dormant cache and respawn (cache-hit
    // reactivates) when the camera comes back within `respawnDist` (default
    // 85% of maxDist - hysteresis so a boundary object never flickers) - so
    // objects behind/around the camera reappear instead of staying gone.
    void cullDistant(const glm::vec3& cameraPos, float maxDist,
                     float respawnDist = -1.0f);

    // Number of instances parked in the dormant respawn cache.
    size_t getDormantCount() const { return m_dormant.size(); }

    // Toggle frustum culling on/off for all instances. When disabled, every
    // instance is sent to the GPU (useful for debugging or fully-visible
    // small scenes like the arena).
    void setFrustumCulling(bool enabled) { m_frustumCulling = enabled; }
    bool isFrustumCullingEnabled() const { return m_frustumCulling; }

    // Permanently wipe the active instances AND the dormant respawn cache.
    void clearAll();

    // Model dimensions (bounding-box size) for placement scaling. Returns a
    // sane non-zero size even for degenerate models.
    glm::vec3 getModelSize(int modelId) const;

    // Raw model bounding box (model space) - used to place a collider on the
    // VISIBLE mesh: the instance anchors the model origin at its position and
    // scales uniformly, so the visual world-space box is bb.min/max * scale
    // around the origin.
    BoundingBox getModelBounds(int modelId) const;
    
    // Render all instances.
    // #3: `lighting` is threaded from the frame owner to the water renderer.
    void render(const glm::mat4& view, const glm::mat4& projection,
                const LightingEnvironment& lighting = LightingEnvironment::Instance());
    
    // Get count
    size_t getInstanceCount() const;

    // forPlayer.txt Theme 3, Method 2: expose the live prop data so editor UI
    // (and raycastProps below) can reach the instanced scene without coupling
    // to private layout. Returns ACTIVE instances only - props parked in the
    // dormant respawn cache (cullDistant) are intentionally excluded.
    const std::vector<SimpleModelInstance>& getActiveInstances() const { return m_instances; }
    const std::vector<std::shared_ptr<Model>>& getLoadedModels() const { return m_models; }

    // forRenderer.txt Step B (grounded; replaces the spec's invented
    // getInstanceMatrixMutable(modelId, instanceIdx)). Mutable handle to a
    // prop instance's modelMatrix so the viewport gizmo loop can read pos/rot
    // and write translate/rotate/scale deltas in place. `idx` is the flat
    // m_instances index returned by raycastProps()/selectedPropInstance().
    SimpleModelInstance& propInstance(std::size_t idx) {
        static SimpleModelInstance fallback{};
        return (idx < m_instances.size()) ? m_instances[idx] : fallback;
    }
    const SimpleModelInstance& propInstance(std::size_t idx) const {
        static SimpleModelInstance fallback{};
        return (idx < m_instances.size()) ? m_instances[idx] : fallback;
    }

    // Instance raycast against world-object prop AABBs. Mirrors PhysicsWorld::
    // raycast's slab math (physicsSystem/physics.cpp:94), but tests each Model's
    // bounding box (Model::GetBoundingBox) scaled by the instance's uniform
    // scale and translated by its world position - recovered from the instance
    // modelMatrix (col3 = position, column lengths = per-axis scale, matching
    // addInstance's translate*rotateY*scale composition). The prop hit set is
    // intentionally separate from PhysicsWorld's rigid-body raycast: foliage /
    // plants have no collider but should still be pickable. See forPlayer.txt.
    //
    // verify.txt Module 3: `pickableLayerMask` is a bitfield of layers that are
    // currently pickable. Layers whose bit is 0 are raycast-locked — skipped
    // so the viewport picker only hits unlocked / isolated layers. Defaults to
    // all 32 layers enabled (no filtering) for backward compatibility.
    bool raycastProps(const glm::vec3& origin, const glm::vec3& direction,
                      float maxDist, float& outDist, std::size_t& outIndex,
                      glm::vec3& outHitPoint, glm::vec3& outHitNormal,
                      uint32_t pickableLayerMask = 0xFFFFFFFF) const;

private:
    std::vector<SimpleModelInstance> m_instances;
    std::vector<SimpleModelInstance> m_dormant;   // respawn cache (cullDistant)
    std::vector<std::shared_ptr<Model>> m_models;
    std::vector<float> m_modelWindStrength;  // parallel to m_models

    // Reused scratch buffer for the per-frame visible-instance collection
    // (avoids rebuilding a fresh unordered_map + vectors every frame).
    std::vector<InstanceData> m_scratch;

    // Procedural water plane (env-driven sun + fog). Rendered on top of the
    // opaque scene so the canonical sun glint + warm-fog tint read coherently.
    std::unique_ptr<WaterRenderer> m_water;

    // Frustum culling toggle (editor UI: World Settings → Frustum Culling).
    // Defaults on; when off every instance bypasses the frustum test.
    bool m_frustumCulling = true;

    // Path -> model id, so loading the same asset (e.g. all three tree
    // variants sharing quiver_tree/q.gltf) reuses one Model + its textures.
    std::unordered_map<std::string, int> m_modelByPath;

    // ---- Async world-object model loading state (see beginAsyncModelLoad) ----
    // All members below are only mutated by the MAIN thread (dispatch in
    // beginAsyncModelLoad, commit in commitAsyncModels, drain in
    // drainCommittedModelIds) — the worker threads touch only the per-slot
    // `progress` atom and fulfill `dataFuture`.
    struct WorldModelAsyncState {
        std::atomic<bool> isLoading{false};
        std::atomic<bool> loadFinished{false};
        std::atomic<bool> failed{false};
        std::atomic<int>  totalModels{0};
        std::atomic<int>  committedModels{0};
        std::atomic<int>  failedModels{0};
        std::atomic<float> progress{0.0f};
        // typeKeys that need a committed model id but haven't received one yet.
        // Filled by drainCommittedModelIds; cleared after the caller wires up
        // bookkeeping, so a second drain only returns NEWLY committed models.
        std::vector<WorldModelResult> pendingResults;
        std::vector<AsyncLoadSlot> slots;
        std::string failedMessage;
    };
    WorldModelAsyncState m_worldAsync;

    // Bounded prop-parse worker pool (default ~3 workers). Replaces per-call
    // std::async in Model::LoadAsync: a full prop set is 6-8 heavy glTF plants,
    // and spawning one OS thread per asset starved the render thread (2-3 FPS).
    // The pool is reusable (no per-load thread churn) and caps parse concurrency
    // so the main/render thread always has cores to stay at ~60 FPS. Held by
    // unique_ptr so the full JobSystem.h (incl. SystemExecutor) only lives in the
    // .cpp, keeping ecs::System out of this header's include graph.
    std::unique_ptr<ecs::JobSystem> m_jobSystem;

    // Register a Model that has ALREADY been GL-uploaded (via Model's sync ctor
    // or CreateFromAsync on the main thread): dedup by path, decimate to
    // `maxTrianglesPerMesh`, then store in m_models + m_modelByPath. Returns the
    // model id, or the existing id if `path` was already registered, or -1 on
    // failure (caller keeps ownership and must delete `model` on -1). `windSway`
    // is applied as the per-model wind strength.
    int registerParsedModel(const std::string& path, Model* model,
                            size_t maxTrianglesPerMesh, float windSway);
};
