#pragma once
// ============================================================================
// UI_SystemCommandProcessor.h
// ----------------------------------------------------------------------------
// forUI.txt Part 3: the centralized System Command Processor.
//
// forUI.txt Part 1 proposed decoupling UI state from the engine core with a
// message-driven queue but left the *consumer* as an exercise. This header is
// that consumer: a single, message-driven choke point through which every
// editor-originated mutation of the ECS world flows.
//
//   ImGui panels (UI thread)
//        │  push(UICommand)  -- lock-guarded, transient structs only --
//        ▼
//   UI::CommandQueue()  ──flushed each frame by──►  UI::SystemCommandProcessor
//                                                        │
//                              ┌─────────────────────────┴────────────────────────┐
//                              ▼                                                   ▼
//                 structural commands                            per-entity field writes
//           (SpawnPrimitive / AtmoCvar / Skybox)           (UpdateTransform)
//           applied on the calling thread                    fanned out across the
//           (no worker races on archetype                     world's ECS JobSystem
//           storage / renderer globals)                       in ENTITY-DISJOINT
//                                                              batches, barrier-waited
//                                                              before renderSceneOnly()
//                                                              iterates the world)
//
// Safety model
//  - The UI touches ONLY UICommandQueue (a lock-guarded vector of POD-ish
//    structs) and the read-only ReflectionSnapshot. It never reaches into the
//    ECS World directly.
//  - Structural commands mutate archetype storage / renderer globals, so they
//    are applied synchronously on the editor thread, never inside a worker.
//  - UpdateTransform is the only command dispatched to workers. It is de-duped
//    per targetEntity and the surviving commands are partitioned into contiguous
//    ranges, so a given entity lives in exactly ONE worker and no two workers
//    ever write the same component instance. The per-batch barrier drains the
//    pool before the world is read again, killing read/write and write/write
//    races with the render/forEach passes.
// ============================================================================
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <mutex>

namespace ecs { class World; }

namespace UI {

// Target field encoded in UICommand::subIndex for CommandType::UpdateTransform.
enum class TransformField : int {
    PositionX = 0, PositionY, PositionZ,
    ScaleX,    ScaleY,    ScaleZ,
    EulerX,    EulerY,    EulerZ
};

struct UICommand {
    enum class CommandType : int {
        UpdateTransform,     // targetEntity=id ; subIndex=TransformField ;
                             // floatValues[0..2]=value ; floatValues[3]!=0 => RELATIVE offset
        SpawnPrimitive,      // stringValue=MeshType name ("Cube"/"Sphere"/...) ;
                             // floatValues[0..2]=pos ; floatValues[3]=uniform scale ;
                             // subIndex=layer index
        SetAtmosphereCvar,   // stringValue=cvar name ("sun_intensity","fog_far",...) ;
                             // floatValues[0]=new value
        HotSwapSkybox        // stringValue=cubemap base dir (6 faces: px/nx/py/ny/pz/nz)
    };

    CommandType type            = CommandType::UpdateTransform;
    uint32_t    targetEntity    = 0;
    int         subIndex        = 0;
    float       floatValues[4]  = {0.0f, 0.0f, 0.0f, 0.0f};
    std::string stringValue;

    // Tiny builder so ImGui call-sites stay readable.
    static UICommand MakeTransform(uint32_t ent, TransformField field,
                                   float x, float y, float z, bool relative = false) {
        UICommand c;
        c.type = CommandType::UpdateTransform;
        c.targetEntity = ent;
        c.subIndex = static_cast<int>(field);
        c.floatValues[0] = x; c.floatValues[1] = y; c.floatValues[2] = z;
        c.floatValues[3] = relative ? 1.0f : 0.0f;
        return c;
    }
};

// ---------------------------------------------------------------------------
// UICommandQueue -- the ONLY structure ImGui ever touches. Thread-safe: panels
// push from the UI thread; the processor flushes from the editor run-loop.
// (forUI.txt's blueprint omitted the mutex; this is the hardened version.)
// ---------------------------------------------------------------------------
class UICommandQueue {
public:
    void push(const UICommand& cmd);
    void push(UICommand&& cmd);
    std::vector<UICommand> flush();   // atomic drain; returns a local copy

    bool   empty() const;
    size_t size()  const;

    // Static conveniences matching forUI.txt's proposed API (operate on the
    // global CommandQueue()).
    static void        Push(const UICommand& cmd);
    static void        Push(UICommand&& cmd);
    static std::vector<UICommand> Flush();

private:
    mutable std::mutex m_mtx;
    std::vector<UICommand> m_queue;
};

// Global singleton populated by ImGui panels.
UICommandQueue& CommandQueue();

// Read-only engine view the UI renders from (forUI.txt Part 1: decoupling).
struct ReflectionSnapshot {
    size_t activeEntityCount{0};
    float  currentPhysicsStepTimeMs{0.0f};
    bool   isForwardRendererActive{true};
};

// ---------------------------------------------------------------------------
// SystemCommandProcessor -- drains a UICommandQueue and applies commands to the
// ECS world. Per-entity transform writes fan out across the world's ECS
// JobSystem (entity-disjoint, barrier-waited); engine-settings commands route
// through callbacks so this header never depends on the renderer/lighting.
// ---------------------------------------------------------------------------
class SystemCommandProcessor {
public:
    explicit SystemCommandProcessor(ecs::World& world) : m_world(world) {}

    struct Result {
        size_t updateTransform = 0;
        size_t spawnPrimitive  = 0;
        size_t atmosphereCvar  = 0;
        size_t skyboxSwap      = 0;
        size_t dropped         = 0;   // malformed / target missing
    };

    // Drain `queue` and apply every command. Returns telemetry for an HUD.
    Result process(UICommandQueue& queue);
    Result process();                       // shorthand: process the global CommandQueue()

    // Engine-settings hooks. Both command types have sensible fall-backs:
    //   * SetAtmosphereCvar defaults to the live CVar registry (lighting/CVar.h)
    //   * HotSwapSkybox has NO safe default (renderer-specific) and is counted
    //     dropped unless the editor installs a handler.
    void setCvarSetCallback(std::function<bool(const std::string& name, float value)> cb);
    void setSkyboxSwapCallback(std::function<void(const std::string& baseDir)> cb);

    // Optional hook that fills the engine-specific snapshot fields the world
    // cannot answer (physics step time, forward-renderer flag).
    void setSnapshotExtender(std::function<void(ReflectionSnapshot&)> cb);

    // Snapshot the live engine state for the details/skybox/ToD panels.
    ReflectionSnapshot captureSnapshot() const;

private:
    void applySpawnPrimitive  (const UICommand& cmd, Result& r);
    void applyAtmosphereCvar  (const UICommand& cmd, Result& r);
    void applyHotSwapSkybox   (const UICommand& cmd, Result& r);
    void applyTransformBatch  (std::vector<UICommand> cmds, Result& r);

    ecs::World& m_world;
    std::function<bool(const std::string& name, float value)> m_setCvar;
    std::function<void(const std::string& baseDir)>           m_skyboxSwap;
    std::function<void(ReflectionSnapshot&)>                  m_snapshotExtender;
};

// One-liner the editor run-loop calls each frame (see forUI.txt Part 3 §Integration).
SystemCommandProcessor::Result dispatchEngineCommands(ecs::World& world, UICommandQueue& queue = CommandQueue());

} // namespace UI
