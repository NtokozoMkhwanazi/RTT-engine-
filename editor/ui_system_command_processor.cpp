// ============================================================================
// UI_SystemCommandProcessor.cpp
// Implementation of forUI.txt Part 3: the centralized System Command Processor.
//
// Compile note: this file deliberately depends only on the ECS core + glm. It
// does NOT include the renderer or lighting headers -- engine-settings commands
// (AtmosphereCvar / SkyboxSwap) are fulfilled through callbacks installed by the
// editor loop, keeping this translation unit light and unit-testable.
// ============================================================================
#include "ui_system_command_processor.h"

#include "ecs/ECS.h"                         // ecs::World, ecs::Entity, ecs::EntityID, JobSystem
#include "ecs/components/Components.h"      // TransformComponent, MeshComponent,
                                             // NameComponent, LayerComponent, MeshType
#include "lighting/CVar.h"                  // CVar::Instance() -- live atmosphere cvar registry
#include <glm/gtc/quaternion.hpp>           // glm::eulerAngles, glm::quat(vec3)

#include <algorithm>
#include <atomic>
#include <unordered_map>

// ===========================================================================
//  Anonymous helpers
// ===========================================================================
namespace {

// Map a primitive name to ecs::MeshType (defaults to Cube). Kept here so the
// command's stringValue stays artist-facing ("Cube"/"Sphere"/...).
ecs::MeshType meshTypeFromString(const std::string& s) {
    using ecs::MeshType;
    if (s == "Cube")      return MeshType::Cube;
    if (s == "Sphere")    return MeshType::Sphere;
    if (s == "Plane")     return MeshType::Plane;
    if (s == "Cylinder")  return MeshType::Cylinder;
    if (s == "Cone")      return MeshType::Cone;
    if (s == "Torus")     return MeshType::Torus;
    return MeshType::Cube;
}

} // namespace

// ===========================================================================
//  UICommandQueue
// ===========================================================================
namespace UI {

void UICommandQueue::push(const UICommand& cmd) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_queue.push_back(cmd);
}

void UICommandQueue::push(UICommand&& cmd) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_queue.push_back(std::move(cmd));
}

std::vector<UICommand> UICommandQueue::flush() {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::vector<UICommand> local;
    local = std::move(m_queue);        // move out -> queue left empty, local owns the batch
    return local;
}

bool UICommandQueue::empty() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_queue.empty();
}

size_t UICommandQueue::size() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_queue.size();
}

// ---- static conveniences (delegate to the global queue) -------------------
UICommandQueue& CommandQueue() {
    static UICommandQueue q;
    return q;
}

void UICommandQueue::Push(const UICommand& cmd) {
    CommandQueue().push(cmd);
}

void UICommandQueue::Push(UICommand&& cmd) {
    CommandQueue().push(std::move(cmd));
}

std::vector<UICommand> UICommandQueue::Flush() {
    return CommandQueue().flush();
}

// ===========================================================================
//  SystemCommandProcessor
// ===========================================================================
SystemCommandProcessor::Result dispatchEngineCommands(ecs::World& world, UICommandQueue& queue) {
    SystemCommandProcessor proc(world);
    return proc.process(queue);
}

SystemCommandProcessor::Result SystemCommandProcessor::process(UICommandQueue& queue) {
    Result r;
    std::vector<UICommand> cmds = queue.flush();
    if (cmds.empty()) return r;

    // Partition: structural commands apply now on this thread; transform writes
    // are batched and fan out to the JobSystem.
    std::vector<UICommand> xform;
    xform.reserve(cmds.size());
    for (auto& cmd : cmds) {
        switch (cmd.type) {
            case UICommand::CommandType::UpdateTransform:
                xform.push_back(cmd);
                break;
            case UICommand::CommandType::SpawnPrimitive:
                applySpawnPrimitive(cmd, r);
                break;
            case UICommand::CommandType::SetAtmosphereCvar:
                applyAtmosphereCvar(cmd, r);
                break;
            case UICommand::CommandType::HotSwapSkybox:
                applyHotSwapSkybox(cmd, r);
                break;
        }
    }

    applyTransformBatch(std::move(xform), r);
    return r;
}

SystemCommandProcessor::Result SystemCommandProcessor::process() {
    return process(CommandQueue());
}

// ---- hooks ----------------------------------------------------------------
void SystemCommandProcessor::setCvarSetCallback(std::function<bool(const std::string&, float)> cb) {
    m_setCvar = std::move(cb);
}

void SystemCommandProcessor::setSkyboxSwapCallback(std::function<void(const std::string&)> cb) {
    m_skyboxSwap = std::move(cb);
}

void SystemCommandProcessor::setSnapshotExtender(std::function<void(ReflectionSnapshot&)> cb) {
    m_snapshotExtender = std::move(cb);
}

ReflectionSnapshot SystemCommandProcessor::captureSnapshot() const {
    ReflectionSnapshot snap;
    snap.activeEntityCount = m_world.getEntityCount();
    // Engine-specific fields (physics step / forward render flag) are filled by
    // the editor-installed extender; otherwise the safe defaults in the struct
    // (0.0f / true) are used.
    if (m_snapshotExtender) m_snapshotExtender(snap);
    return snap;
}

// ===========================================================================
//  Structural commands (applied on the calling thread -- never inside a worker)
// ===========================================================================
void SystemCommandProcessor::applySpawnPrimitive(const UICommand& cmd, Result& r) {
    // Create in archetype storage with the canonical editable component set.
    ecs::Entity e = m_world.createEntityWithComponents<
        ecs::TransformComponent,
        ecs::MeshComponent,
        ecs::NameComponent,
        ecs::LayerComponent>();

    if (!e.isValid()) { ++r.dropped; return; }

    const glm::vec3 pos(cmd.floatValues[0], cmd.floatValues[1], cmd.floatValues[2]);
    const float uniformScale = cmd.floatValues[3];
    const int   layer        = cmd.subIndex;
    const std::string& typeName = cmd.stringValue;

    if (auto* t = m_world.getComponentArchetype<ecs::TransformComponent>(e)) {
        t->position = pos;
        t->scale    = glm::vec3(uniformScale);
        t->markDirty();
    }
    if (auto* m = m_world.getComponentArchetype<ecs::MeshComponent>(e)) {
        m->meshType       = meshTypeFromString(typeName);
        m->visible        = true;
        m->castShadow     = true;
        m->receiveShadow  = true;
    }
    if (auto* n = m_world.getComponentArchetype<ecs::NameComponent>(e)) {
        n->setName(typeName.empty() ? "Primitive" : typeName);
    }
    if (auto* l = m_world.getComponentArchetype<ecs::LayerComponent>(e)) {
        l->layer = layer;
    }
    ++r.spawnPrimitive;
}

void SystemCommandProcessor::applyAtmosphereCvar(const UICommand& cmd, Result& r) {
    if (cmd.stringValue.empty()) { ++r.dropped; return; }

    if (m_setCvar) {
        // Editor supplied a custom handler (e.g. persists to Config).
        if (m_setCvar(cmd.stringValue, cmd.floatValues[0])) {
            ++r.atmosphereCvar;
        } else {
            ++r.dropped;                      // handler rejected the key/value
        }
        return;
    }

    // Default: write the live lighting CVar registry (lighting/CVar.h). This
    // runs on the editor thread; LightingEnvironment re-reads its cvants every
    // frame in renderSceneOnly(), so the change takes effect immediately. We
    // only touch an already-registered FLOAT slot so we never create junk
    // entries or clobber a Vec3-typed key (CVar refuses type mismatches).
    const CVarHandle h = CVar::Instance().handleOf(cmd.stringValue);
    if (h.valid() && h.type == static_cast<uint8_t>(CVarType::Float)) {
        CVar::Instance().setFloat(cmd.stringValue, cmd.floatValues[0]);
        ++r.atmosphereCvar;
    } else {
        ++r.dropped;                          // unknown / wrong-type cvar
    }
}

void SystemCommandProcessor::applyHotSwapSkybox(const UICommand& cmd, Result& r) {
    if (!m_skyboxSwap) {
        // No renderer binding registered -- skybox loads are renderer-specific.
        ++r.dropped;
        return;
    }
    m_skyboxSwap(cmd.stringValue);
    ++r.skyboxSwap;
}

// ===========================================================================
//  UpdateTransform -- the only command that goes parallel.
// ===========================================================================
void SystemCommandProcessor::applyTransformBatch(std::vector<UICommand> cmds, Result& r) {
    if (cmds.empty()) return;

    // De-dupe per (targetEntity, field): a slider drag on X and a slider drag on
    // Y for the SAME entity must both survive, so the key is (entity, field),
    // with last-push-wins per field (matches RenderBatchPropertyField semantics).
    std::unordered_map<uint64_t, UICommand> seen;
    seen.reserve(cmds.size() * 2);
    for (auto& c : cmds) {
        uint64_t key = (uint64_t(c.targetEntity) << 32)
                     | uint64_t(uint32_t(c.subIndex));
        seen[key] = c;                        // later write overwrites an earlier one
    }

    const size_t n       = seen.size();
    const size_t workers = m_world.getJobSystem().getWorkerCount();

    // Mutator for a single transform command. Each entity is visited by at most
    // one worker (see hash-bucket partitioning below), so these writes are
    // race-free: different workers touch different component instances, and an
    // entity's own multi-field writes run sequentially inside its single worker.
    auto applyOne = [this](const UICommand& cmd) -> bool {
        using F = TransformField;
        ecs::TransformComponent* t =
            m_world.getComponent<ecs::TransformComponent>(cmd.targetEntity);
        if (!t) return false;

        const bool relative = cmd.floatValues[3] != 0.0f;
        const float x = cmd.floatValues[0];
        const float y = cmd.floatValues[1];
        const float z = cmd.floatValues[2];

        switch (static_cast<F>(cmd.subIndex)) {
            case F::PositionX: t->position.x = relative ? t->position.x + x : x; break;
            case F::PositionY: t->position.y = relative ? t->position.y + y : y; break;
            case F::PositionZ: t->position.z = relative ? t->position.z + z : z; break;
            case F::ScaleX:    t->scale.x    = relative ? t->scale.x    + x : x; break;
            case F::ScaleY:    t->scale.y    = relative ? t->scale.y    + y : y; break;
            case F::ScaleZ:    t->scale.z    = relative ? t->scale.z    + z : z; break;
            case F::EulerX: {
                glm::vec3 e = glm::eulerAngles(t->rotation);
                e.x = relative ? e.x + x : x;
                t->rotation = glm::quat(e); break;
            }
            case F::EulerY: {
                glm::vec3 e = glm::eulerAngles(t->rotation);
                e.y = relative ? e.y + y : y;
                t->rotation = glm::quat(e); break;
            }
            case F::EulerZ: {
                glm::vec3 e = glm::eulerAngles(t->rotation);
                e.z = relative ? e.z + z : z;
                t->rotation = glm::quat(e); break;
            }
            default:
                return false;            // unknown field index
        }
        t->markDirty();            // invalidate the cached model matrix
        return true;
    };

    // No worker pool (or a single surviving command): stay on the calling
    // thread. Still correct; we just skip the fan-out.
    if (workers == 0 || n == 1) {
        for (auto& kv : seen) {
            if (applyOne(kv.second)) ++r.updateTransform; else ++r.dropped;
        }
        return;
    }

    // Partition by entity hash: every command for a given entity lands in the
    // SAME bucket, so buckets are pairwise entity-disjoint and no two workers
    // can write the same component. An entity's own multi-field writes stay in
    // that one bucket and are applied sequentially.
    const size_t bucketCount = std::min(workers, n);
    std::vector<std::vector<UICommand>> buckets(bucketCount);
    for (auto& kv : seen) {
        UICommand c = std::move(kv.second);
        buckets[c.targetEntity % bucketCount].push_back(std::move(c));
    }

    std::vector<ecs::JobSystem::JobHandle> handles;
    handles.reserve(bucketCount);
    std::atomic<size_t> applied{0};
    std::atomic<size_t> failed{0};

    for (size_t w = 0; w < bucketCount; ++w) {
        if (buckets[w].empty()) continue;
        const size_t widx = w;
        handles.push_back(m_world.getJobSystem().addJob(
            [this, widx, &buckets, &applied, &failed, applyOne]() {
                size_t a = 0, f = 0;
                for (auto& c : buckets[widx]) {
                    if (applyOne(c)) ++a; else ++f;
                }
                applied.fetch_add(a, std::memory_order_relaxed);
                failed.fetch_add(f, std::memory_order_relaxed);
            }));
    }

    // Barrier -- every transform write completes before the world is read again
    // by renderSceneOnly()/forEach in the same frame (no stale/missing writes).
    for (auto& h : handles) h.wait();

    r.updateTransform = applied.load(std::memory_order_relaxed);
    r.dropped        += failed.load(std::memory_order_relaxed);
}

} // namespace UI
