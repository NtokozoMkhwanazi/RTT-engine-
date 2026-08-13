# RTT Engine — Architecture

A map of the engine's systems, how the entry points wire them together, and the data
flow of a single frame. Read [README.md](../README.md) for build/run instructions and
[TROUBLESHOOTING.md](TROUBLESHOOTING.md) for common problems.

---

## 🗺️ Overview

The engine is organized in four layers. Each layer only depends on the ones below it,
which keeps subsystems testable in isolation.

```
┌────────────────────────────────────────────────────────────────────────────┐
│ ENTRY POINTS        test.cpp (engine)  src/editor_main.cpp (editor app)   │
│                     tests/test_main.cpp (test runner)                     │
├────────────────────────────────────────────────────────────────────────────┤
│ APPLICATION         EditorApplication (singleton)                         │
│                     RenderPipeline · WorldManager · InputManager          │
│                     ResourceManager · PlayModeController · ThirdPersonCam │
├────────────────────────────────────────────────────────────────────────────┤
│ SYSTEMS            ECS · Terrain/World · Physics · Animation · Motion     │
│                     Matching · Camera · Geospatial · Demo · Editor UI     │
├────────────────────────────────────────────────────────────────────────────┤
│ FOUNDATION         Memory arenas/pools · Assimp model loading · Shaders   │
│                     GLM · GLAD/GLFW/OpenGL · Dear ImGui                   │
└────────────────────────────────────────────────────────────────────────────┘
```

> **Pure-logic vs. GL:** every system below the renderer is written **GL-free**
> (headless-testable) — the editor-UI/viewport code is the exception (see the test
> layering table below). Only the application/render layer touches OpenGL. This is
> why the entire test suite runs without a window and the engine can fall back to a
> GL-free logic simulation.

---

## 🚪 Entry Points

| Binary | Entry | What it does |
|--------|-------|--------------|
| `bin/engine` | `test.cpp` | **Full engine run**: banner → system inventory → Google Test self-check (535 tests) → engine boot → main loop → summary. Owns `main()`, links all test objects **except** `tests/test_main.cpp`. |
| `bin/editor_app` | `src/editor_main.cpp` | ImGui editor application: GLFW window + ImGui context + `EditorApplication::run()`. |
| `bin/test_runner` | `tests/test_main.cpp` | Plain Google Test runner (`make test`). |
| `bin/bot_viewport_test`, `bin/geoterrain_test` | standalone test mains | Focused demo/test apps. |

### Engine boot sequence (`test.cpp`)

```
main()
 ├─ crash handlers (SIGSEGV/ABRT/FPE → demangled backtrace to crash.log)
 ├─ parse flags (--skip-tests, --headless, --frames N)
 ├─ print system inventory (module → test suite → description)
 ├─ PHASE 1: runTestPhase()  → InitGoogleTest + RUN_ALL_TESTS (535 tests)
 │            └─ failures?  → print + exit (refuse to boot)
 └─ PHASE 2: runEngine()
      ├─ glfwInit → window (hidden if --headless / no DISPLAY)
      │    └─ no GL context? → runLogicSim() (GL-free character + matcher)
      ├─ gladLoadGLLoader → OpenGL context
      ├─ glctx::setAlive(true)          ← guards GL calls during static destruction
      ├─ Editor::ImGuiContext  (owns ImGui + backend bindings)
      ├─ EditorApplication::initialize()  (render pipeline, world, input)
      ├─ app.enterPlayMode()           ← loads bot.fbx + 7 locomotion clips + GPU mesh
      ├─ DemoRecorder::initialize() + startPlayback()
      ├─ geo::GeoAPI::initialize(Sydney) + setGPSMode(SIMULATED_WALK)
      ├─ main loop  (below)
      └─ cleanup: imgui → app.shutdown() → demo → glctx::setAlive(false)
           → glfwDestroyWindow → glfwTerminate
```

---

## 🔄 The Frame Loop

The engine's frame is driven by `test.cpp`; the editor app uses the same shape inside
`EditorApplication::run()`.

```
┌─ once per frame ────────────────────────────────────────────────────────────┐
│ dt (wall-clock, capped 0.1s — or fixed 1/60s in headless)                   │
│                                                                             │
│ 1. INPUT  ── cinematic script (CinematicDemo::At(t))  OR  keyboard (WASD,   │
│             Space, Ctrl/C crouch, Shift sprint)   [F9 toggles script]       │
│                                                                             │
│ 2. UPDATE ── app.updatePlayMode(dt, input)                                  │
│              ├─ PlayModeController → AnimatedCharacter::update              │
│              │    movement + terrain snap + FSM + motion matching + animator│
│              └─ ThirdPersonCamera::update (from character pose)             │
│              WorldManager::update(cameraPos, dt)  (terrain LOD streaming)   │
│              DemoRecorder::update(dt) · GeoAPI::update(dt)                  │
│                                                                             │
│ 3. RENDER ── app.renderSceneOnly(dt)                                        │
│              ├─ RenderPipeline::beginFrame  (fps stats)                     │
│              ├─ skybox first (z=1.0, GL_LEQUAL)                             │
│              ├─ Renderer: SetCameraMatrices → SubmitBatches → Render        │
│              ├─ WorldManager::render → Terrain::render (chunks + LOD)       │
│              └─ renderPlayCharacter (skinned model w/ animator bone palette)│
│              ImGui: beginFrame → PlayModeHUD + engine status → endFrame     │
│                                                                             │
│ 4. PRESENT ── glfwSwapBuffers + glfwPollEvents                              │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## 🧩 Application Layer

### `Editor::EditorApplication` (singleton)

Owns and coordinates the frame via four singleton managers (all `getInstance()`):

```
EditorApplication
 ├── Render::RenderPipeline     — viewport FBO, skybox, batched renderer, stats
 ├── Input::InputManager        — GLFW callbacks → InputState (isKeyDown, mouse delta)
 ├── World::WorldManager        — Terrain + Floor + PhysicsWorld + WorldObjectManager
 ├── Resources::ResourceManager — shared assets
 ├── PlayModeController         — play-mode character lifecycle (pure logic)
 └── ThirdPersonCamera          — state-aware follow camera (used in play mode)
```

Key public hooks used by the engine entry point:

- `updatePlayMode(float dt, const CharacterInput&)` — drives the character + camera
  with explicit input (scripted demos, headless runs).
- `renderSceneOnly(float dt)` — renders the 3D scene **without swapping**, so ImGui
  HUDs can draw on top before the caller swaps.

### `Render::RenderPipeline`

- Renders everything into an **FBO-backed viewport** (`getFramebufferTexture()` →
  shown in the editor viewport panel).
- Draw order: **skybox first** (far plane, `GL_LEQUAL`), then depth-restore, then the
  batched renderer (material/shader-sorted, instancing), then world/terrain, then the
  play character (skinned).
- `renderUI()`/`endFrame()` are placeholders — UI and swap are handled by the caller.

### `World::WorldManager`

- `Terrain` — chunked heightmap with LOD (`getHeightAt(x,z)` used by the character's
  terrain snap, `render(view, proj, camPos, fov, aspect, near, far)`).
- `Floor` — flat physics floor fallback when terrain isn't initialized.
- `PhysicsWorld` — rigid-body simulation (floor contacts).
- `WorldObjectManager` — pre-placed world objects (loads `assets/World_objects/`).

---

## ⚙️ Core Systems

### ECS (`ecs/`)

Archetype-based ECS for cache-coherent iteration:

- `World` — entity creation, component access, `forEach<...>()` archetype iteration.
- **Archetypes** — entities grouped by component signature (contiguous chunks).
- **Blueprints** — reusable entity templates (`createBlueprintFrom`, spawn).
- **Relationships** — parent/child hierarchies (`RelationshipManager`).
- **Events** — typed event dispatch (`EventSystem`).
- **Jobs** — parallel work (`JobSystem`).
- **Serialization** — scene save/load to JSON.
- `EnhancedWorld` — high-level facade over the above.

### Character pipeline (`animationSystem/` + `motionMatching/` + `editor/`)

The engine's centerpiece — an input-to-pose pipeline that is entirely GL-free:

```
CharacterInput (moveDirection, moveMagnitude, jump, crouch, sprint, grounded)
   │
   ▼
AnimatedCharacter::update(dt, input, terrain)
   ├─ 1. Movement   — face move dir, accelerate to walk/run/crouch speed cap
   ├─ 2. Jump/gravity, integrate position
   ├─ 3. Terrain snap  — position.y = terrain(x, z) (or floor fallback)
   ├─ 4. Locomotion FSM — state = IDLE/WALK/RUN/JUMP/FALL/CROUCH/CROUCH_WALK
   ├─ 5. Animation drive — preview clip  |  MotionMatcher pose search  |  FSM
   │        MotionMatcher: CharacterState (model-space) → KD-tree pose search
   │        → blended poses → foot IK / foot planting (world-space floor)
   └─ 6. Animator::Update(dt) — layer clocks, blend, final bone matrices
   │
   ▼
Render: Model::Draw(shader, *animator)  — GPU skinning via bone palette
```

Key units:

| Unit | Responsibility |
|------|----------------|
| `AnimationStateMachine` | Locomotion FSM + `CharacterInput` definition |
| `Animator` | Layer-based blending, time advance, final bone matrices, foot IK state |
| `MotionMatcher` | `Initialize` → `LoadAnimation` (non-owning clip aliases) → `BuildSearchIndex` → `Update(dt, cs)` KD-tree pose search |
| `MotionDatabase` | Pose index over clips (60 s guard, airborne pose tags) |
| `MotionKDTree` | SAH-optimized KD-tree for pose queries |
| `FootPlantingSystem` | Foot lock/plant state |
| `TrajectoryPredictor` | Future-trajectory features for pose search |
| `PlayModeController` | Thin wrapper: load model + clips, forward updates (the unit-test surface) |

**Scale discipline:** the character moves in *world* units but the pose database is
in *model* units (bot is ~180 units tall, world scale ~0.01). `driveMotionMatching`
divides position/velocity by `scale` before searching, and passes a model-to-world
matrix so foot IK runs in world space. Getting this wrong makes the matcher lock onto
Idle.

**Destruction order matters** (member order in `AnimatedCharacter`): `matcher_` (whose
database aliases clips) must die **before** `clips_`; `animator_`/`skeleton_` before
the matcher.

### Camera (`cameraSystem/`)

- `ThirdPersonCamera` — state-aware follow: `update(dt, CameraInput, aspect)` with
  per-state smoothing (IDLE/WALK/RUN/JUMP/FALL/CROUCH/TRANSITIONING), smooth
  transitions, yaw/pitch orbit, collision avoidance against the ideal position,
  view/projection output.
- `CameraController` — mode presets (THIRD_PERSON/FIRST_PERSON/ORBIT/CINEMATIC), zoom,
  reset.
- `CinematicDemo` — pure math for the scripted idle→walk→run→jump→return character
  loop (`PhaseAt(t)`, `At(t, firstFrame)`); drives the character when the engine boots
  and on F9.

### Physics (`physicsSystem/`)

- **GJK/EPA** narrow phase (convex collision + penetration depth).
- `PhysicsWorld` — rigid bodies, gravity, integration, damping.
- Constraints — point/hinge/slider/6DOF + character constraints.
- `Floor` — the play-mode ground plane.

### Memory (`memory/`)

- `MemoryArena` / `DualArena` — bump allocators with reset/swap.
- `MemoryPool` — fixed-size object pools.
- `StackAllocator` — LIFO scoped allocation.
- `MemoryManager` / `AssetManager` — global stats + asset lifecycle.

### Geospatial (`geospatial/`)

- `GeoAPI` — facade: `initialize(lat, lon, alt)` → `setGPSMode(GPSTracker::Mode)` →
  per-frame `update(dt)`.
- Coordinate pipeline: **WGS84 (double) → ECEF → ENU → engine space (float)** — keeps
  all double precision on the geo side so float systems stay unchanged.
- `GPSTracker` — 5 simulated modes (STATIC/WALK/VEHICLE/AIRCRAFT/DISABLED).
- Optional backends (graceful fallback): InfluxDB time-series storage, TFLite ML
  prediction (Kalman filter is the default predictor).

### Demo (`demo/`)

- `DemoRecorder` — keyframe camera recording/playback over a scripted path
  (`setupDemoPath` → 8 keyframes), segment naming, `update(dt)` loop with restart.
- `DemoConfig` — demo timing/resolution/segment plan.

### Editor UI (`editor/`)

- `Editor::ImGuiContext` — owns the ImGui context + GLFW/OpenGL3 backends
  (`beginFrame`/`endFrame`).
- `UI::RenderPlayModeHUD` — play-mode overlay (state, speed, motion-matching
  diagnostics, foot IK locks). Safe to call without a loaded character.
- Editor panels (outliner, inspector, content, console, profiler, geo config) are
  driven by the `EditorState`/`EditorApplication` machinery in `editor/`.

---

## 🧱 Design Patterns

1. **Singletons via `getInstance()`** — `RenderPipeline`, `InputManager`,
   `WorldManager`, `ResourceManager`, `EditorApplication`. All are headless-safe to
   instantiate; GL work is confined to explicit init/shutdown.
2. **Pure-logic core + GL wrapper** — `AnimatedCharacter`/`PlayModeController`/camera/
   terrain math have no GL calls, so the entire test suite runs windowless. The editor
   layer owns GL resources.
3. **`glctx::setAlive()` guard** — global flag marking the GL context's lifetime;
   destructors that could fire after `glfwTerminate()` (static destruction) early-out
   instead of touching a dead context.
4. **Non-owning aliases with explicit destruction order** — motion database aliases
   clips; matcher dies before clips.
5. **Deterministic headless simulation** — `--headless` uses a fixed 60 Hz dt so the
   scripted cinematic sequence is reproducible regardless of frame rate.
6. **Self-check as a boot gate** — the engine entry point runs the full test suite
   before booting, treating tests as the health check.

---

## 🏗️ Build & Test Architecture

- **Makefile wildcard collections** — each system directory's `*.cpp` is globbed into
  one object set (`OBJS`); ImGui sources and `src/glad.c` are added explicitly.
- **One object tree, three binaries** — `build/` is shared; debug/release/asan change
  the flags, so **always `make clean` when switching `MODE`**.
- **Test objects** are linked into `bin/test_runner` (with `gtest_main`) **and**
  `bin/engine` (with `test.cpp`'s `main`; `tests/test_main.cpp` is excluded so there
  is exactly one `main`).
- **Standalone tests** (`bot_viewport_minimal`, `test_geoterrain`) own their `main`
  and are excluded from the shared test object set.

```
tests/*.cpp ──► build/tests/*.o ──┬──► bin/test_runner   (gtest_main)
                                  └──► bin/engine          (test.cpp main)
engine src + system src ─► OBJS  ──┬──► bin/test_runner
                                  ├──► bin/engine
                                  └──► bin/editor_app      (src/editor_main.cpp)
```

### Test layering

| Level | Examples | Runs |
|-------|----------|------|
| Unit (pure math) | `MathTest`, `CinematicDemo`, `Memory*`, `CameraSystemTest` | ms |
| System | `PhysicsTest`, `TerrainTest`, `AnimationTest`, `MotionMatching*` | s |
| Integration | `IntegrationTest` (input→FSM→MM→pose sync), `PlayModeController` (loads FBX) | s–10s |
| Headless GL | `ImGuiContext`, `ViewportFramebuffer`, `ViewportRenderingSafety` | need an X server (xvfb) |

The engine self-check runs **all of these in process before boot** — the 535-test
suite is the engine's own smoke test.
