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
| `bin/engine` | `test.cpp` | **Full engine run**: banner → system inventory → Google Test self-check (731 tests) → engine boot → main loop → summary. Owns `main()`, links all test objects **except** `tests/test_main.cpp`. |
| `bin/editor_app` | `src/editor_main.cpp` | ImGui editor application: GLFW window + ImGui context + `EditorApplication::run()`. |
| `bin/test_runner` | `tests/test_main.cpp` | Plain Google Test runner (`make test`). |
| `bin/bot_viewport_test`, `bin/geoterrain_test` | standalone test mains | Focused demo/test apps. |

### Engine boot sequence (`test.cpp`)

```
main()
 ├─ crash handlers (SIGSEGV/ABRT/FPE → demangled backtrace to crash.log)
 ├─ parse flags (--skip-tests, --headless, --frames N)
 ├─ print system inventory (module → test suite → description)
 ├─ PHASE 1: runTestPhase()  → InitGoogleTest + RUN_ALL_TESTS (731 tests)
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
│ 4. PRESENT ── RHI::endFrame (OpenGL: glfwSwapBuffers; Vulkan: queue        │
│               present)                                                     │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## 🎨 RHI (Render Hardware Interface)

The engine renders through a thin RHI layer (`rhi/RHI.h`) so it can run on
OpenGL (low / budget machines - the default) or Vulkan (high graphics
quality). Selected via `--graphics opengl|vulkan` on the command line (default
opengl).

```
IRHI (rhi/RHI.h)
 ├── RHIGL     (rhi/RHIGL.cpp)   — GLFW window + OpenGL context + present.
 │                                The full engine renderer runs unchanged
 │                                between beginFrame()/endFrame().
 └── RHIVulkan (rhi/RHIVulkan.cpp) — Vulkan instance/device/swapchain +
                                    clear-frame present, PLUS a real offscreen
                                    render path (SPIR-V pipeline, buffers,
                                    render pass, submit, readback) that works
                                    headless - this is the skeleton the scene
                                    renderers port onto.
```

**Runtime state.** `activeGraphicsApi()` reports the backend the session is
running on; the editor's **Graphics menu** (View→Graphics) shows it and lets
the user switch for the next launch, persisted to `graphics_api.cfg` (also
overridable with `--graphics opengl|vulkan`). A live mid-frame hot-swap of two
graphics contexts is deliberately not attempted.

**Headless rendering.** `renderOffscreenTriangle()` renders a fixed triangle
through each backend's real render stack into a CPU buffer. This is how the
Vulkan port is verified on machines with no display: the device is created in
device-only mode (no surface/swapchain needed) and the pixel readback proves
the SPIR-V pipeline, buffers, render pass and command submission actually
produce output. The `RHI.*` tests assert both backends render the SAME
triangle (center green, corner clear).

**3D scene render** (`renderOffscreenScene` + `rhi/RHIMath.h`): the first
"real" ported render path, implemented identically on both backends. It draws
an instanced scene through a perspective camera with three building blocks
every engine renderer needs:

- a **camera uniform buffer** (view-projection matrix, computed with the
  shared `RHI::Mat4` math in `rhi/RHIMath.h`),
- a **depth attachment** so nearer geometry occludes farther
  (depth test on, `GL_LESS` / `VK_COMPARE_OP_LESS`),
- **per-instance model matrices** (the batched renderer's core pattern).

Two geometry paths are supported. `OffscreenScene::instances` is the simple
instanced-CUBE path; `OffscreenScene::meshes` is the GENERIC path the engine's
real renderers port onto: `OffscreenMesh` carries interleaved vertex data
(position/normal/uv, 8 floats per vertex) + 32-bit indices + its own
per-instance transforms, drawn INDEXED-INSTANCED with a lambert-lit mesh
shader (`rhi/shaders/mesh.vert/.frag`). Both backends concatenate all meshes
into shared vertex/index/instance buffers and draw each with
`vkCmdDrawIndexed` offsets (GL: per-mesh `glBufferData` + `glDrawElements-
Instanced`). This is the exact vertex format and draw pattern the engine's
world-object / terrain / character renderers need, so they port to the RHI by
submitting meshes instead of calling GL directly.

Vulkan clip space differs from GL (y down, z in [0,1]); `mat4GlToVulkanProj`
bakes the y-flip + z-remap into the projection so the SAME scene description
produces IDENTICAL pixels on both backends (row 0 = top; the GL backend flips
its bottom-up readback). Shaders compiled to SPIR-V by `glslc` at build time.
The `RHI.*` scene tests verify depth ordering (near cube occludes far),
camera movement (viewing from behind swaps which cube is on top), instancing
(all 4 instances appear), generic meshes (a quad renders at the expected
pixels on both backends, multi-instance meshes too), and exact GL/Vulkan
pixel parity on the same scene.

**Visible window content** (`renderFrameScene`): the Vulkan backend now
renders the scene INTO the swapchain image (between `beginFrame`/`endFrame`)
instead of just clearing - a swapchain-sized depth image, a color+depth
render pass (ending in `COLOR_ATTACHMENT_OPTIMAL` so the UI can draw over
it), and a second instanced pipeline built for it (sharing the camera-UBO
descriptor set). `endFrame` submits the recorded command buffer and presents;
it falls back to the clear pass when nothing was rendered, and transitions
the image to `PRESENT_SRC` itself when no UI pass followed the scene. The
swapchain extent respects the surface's `currentExtent` (requesting a size
outside the surface bounds makes every acquire/present OUT_OF_DATE and leaves
the acquire semaphore signaled).

**Editor UI on Vulkan** (`initializeImGui`/`renderFrameImGui`): the Dear ImGui
overlay renders on the SAME swapchain image, on top of the scene - a color-
only render pass with `loadOp = LOAD` (the scene stays visible under the UI)
ending in `PRESENT_SRC`, driven by `imgui_impl_vulkan` (initialized lazily by
the backend against its own descriptor pool). The GL backend is a no-op here:
the GL editor owns ImGui through `imgui_context`.

**The SAME editor on both backends** (`src/editor_main.cpp`): the full editor
UI - menu bar (File / Edit / Add / View with the **Graphics** toggle / Play /
Camera), toolbar, left/right/bottom panels and status bar - is driven by one
shared function, `RenderSharedEditorUI`, on BOTH backends. The only
backend-specific part is HOW the 3D scene reaches the screen: on OpenGL the
`EditorApplication` renders the real world (terrain, world objects, play-mode
character) into a viewport FBO whose texture `UI::RenderViewport` displays;
on Vulkan the RHI renders the scene INTO the swapchain and the viewport
panel is **swapchain-backed** (`UI::RenderViewport(..., swapchainBacked)`):
fully transparent (no WindowBg, no FBO image), so the live scene shows
through below the 28px viewport header while the toolbar buttons, camera
overlay and hover-rect tracking still work. `./bin/editor_app --graphics
vulkan` therefore shows the identical editor as `--graphics opengl` - the
same menu bar, panels, theming and icons (EditorTheme / Phosphor /
FontAwesome6, fonts uploaded lazily by imgui_impl_vulkan) - and the View →
Graphics menu persists the backend for the next launch (`graphics_api.cfg`).
`RHI.VulkanBackend_RendersImGuiOverScene` drives the full chain in the tests.

**Real world content on Vulkan** (`src/editor_main.cpp`): the Vulkan viewport
no longer shows a demo cube grid - it renders the engine's REAL world-object
assets (quiver_tree trees, boulders, grass/periwinkle/othonna plants)
loaded through the CPU-only assimp path (`Model::LoadModelData` - raw
vertices/indices/textures, no GL, so it works on the GLFW_NO_API window),
merged into `OffscreenMesh`es (one indexed-instanced draw per model) and
scattered over the engine's REAL terrain heightfield. The scatter replicates
the GL `VegetationSystem`/`WorldManager` placements EXACTLY (same
mt19937(42) seed, distributions and draw order), so both backends put
trees/rocks/plants in IDENTICAL world positions.

The terrain is a genuine port: the heightfield lives in the shared GL-free
header `world/TerrainHeight.h` (`terrain::heightAtWorld`, seed-42 perlin at
the engine's fixed scales) that BOTH the GL `Terrain` renderer's master
heightmap and the Vulkan viewport's ground grid sample - so both backends
render the same surface (the master heightmap is just that function on the
integer texel grid, "texel i = world x=i"). The ground grid is built from
`terrain::heightAtWorld(x, z, 25)` (the editor's heightScale) with central-
difference normals.

**Albedo textures on the mesh path** (`OffscreenMesh::texturePixels`, both
backends + `mesh.vert/.frag`): each model's first diffuse texture is attached
CPU-side (from `AsyncModelData::textures`, downscaled to ≤1024 to keep the
editor's GPU footprint sane) and sampled in the fragment shader, modulated
by the per-instance color. Meshes WITHOUT a texture bind a shared 1x1 WHITE
texture, so the shader math - and therefore GL/Vulkan pixel parity - is
identical for every mesh (`RHI.TexturedMeshRendersAndParity` asserts a
checkerboard renders with both checker colors on both backends, to matching
pixels). The world objects therefore keep their authored materials on
Vulkan instead of flat lambert colors. The ground grid is textured too - a
procedural grass/rock albedo (slope + height + hash-noise, world-anchored
to the grid's UVs) so the terrain reads as grassland instead of a flat
brown plane.

**Distance fog + long view distance** (`CameraUBOData`, both backends): the
shared scene UBO grew to 96 bytes (viewProj + camera position + fog
parameters, same std140 layout on GL and Vulkan). The mesh shaders compute
exponential-squared distance fog with IDENTICAL math on both backends and
mix toward the scene's fog color (which matches the clear color, so the
horizon melts into the sky). `fogDensity = 0` disables fog exactly (factor
0, unchanged output), keeping the parity tests pixel-exact. `OffscreenCamera
::farPlane` (default 100) lets the editor push the projection out to 600 so
the 300-unit world is never hard-clipped at the horizon.

**4x MSAA on the visible swapchain** (`RHIVulkan.cpp`): the windowed scene
renders at 4 samples/pixel into a multisampled color target that resolves
into the swapchain image (multisampled depth, `pickSwapchainSamples`
falls back to 1x when the device lacks 4x support). The offscreen readback
path stays 1x so the parity tests stay exact. This is the "leverage the
GPU" part of the dual RHI: crisp edges on real hardware with no
per-pixel cost on the CPU.

**Animated character** (`SkinnedCharacter`, same file): the bot model is
loaded CPU-only with its Skeleton deep-copied, and the WALK clip is loaded
via `AnimatedCharacter::LoadClipFromFile` (Assimp → `AssimpAnimationLoader`,
no GL - the most visible clip, clear limb motion; Idle is the fallback). An
`Animator` drives it: every frame `SkinCharacter` advances the clip, sums
each vertex's weighted `GetFinalBoneMatrices` skinning matrices and rewrites
the mesh's interleaved buffer; the `version` bump makes the Vulkan backend
re-upload ONLY that mesh's slice of the shared buffers (incremental path)
while the multi-hundred-KB static world stays resident. The struct lives on
the heap deliberately: the Animator holds a raw pointer to its skeleton, so
the character must never move after construction. It stands 2m at the world
origin, front and center in the default framing.

The orbit camera starts CLOSE on the character (dist 13, pitch 15) so the
animation is plainly visible, and adds a gentle AUTO-ORBIT showcase: after a
few seconds without input the camera slowly swings around the scene (drag or
scroll to take back control). Right-drag orbits, scroll zooms, WASD pans the
target across the terrain (which it follows, so the camera never drops
underground) - all gated on `UI::IsViewport3DHovered()` / ImGui capture so
panels and menus keep the mouse and keyboard. A bottom-right overlay lists
the bindings.

`available()` probes whether a backend can run on this machine (GL is always
yes; Vulkan checks the loader + driver). The full windowed surface/swapchain
present path runs on machines with a display. One environment note: the
engine runs a single backend per launch, and the Vulkan backend requests a
`GLFW_NO_API` window (never a GL context) so it can never clobber the GL
backend's context.

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
- `UI::RenderPlayModeDebug` — play-mode debug section (state, speed,
  motion-matching diagnostics, foot IK locks) rendered INSIDE the Details
  panel while playing, so the viewport stays unobstructed. Safe to call
  without a loaded character.
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

The engine self-check runs **all of these in process before boot** — the 731-test
suite is the engine's own smoke test.
