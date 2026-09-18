# RTT Engine — 3D Game Engine

![C++](https://img.shields.io/badge/C%2B%2B-17-blue)
![OpenGL](https://img.shields.io/badge/OpenGL-4.5-orange)
![Vulkan](https://img.shields.io/badge/Vulkan-1.0-purple)
![FSR3](https://img.shields.io/badge/FSR3-EASU%2BRCAS-orange)
![Platform](https://img.shields.io/badge/Platform-Linux-6f42c1)
![Tests](https://img.shields.io/badge/Tests-731%20passing%20across%2099%20suites-brightgreen)
![License](https://img.shields.io/badge/License-MIT-green)

**Advanced lightweight 3D game engine(beta)** built for scalability and high-performance
systems: real-time rendering, animation, motion matching, physics and geospatial
simulation — written in **C++17** on a **dual-backend RHI** (OpenGL 4.5 for
low/optimized machines, Vulkan for high-quality rendering) with a **Dear ImGui**
editor. Pick the backend at launch with `--graphics opengl|vulkan` (persisted to
`graphics_api.cfg`). The OpenGL backend drives the full renderer; the Vulkan
backend initializes the device + swapchain and renders the 3D scene (with the
editor UI composited on top) — the two backends render identical scenes to
identical pixels, verified headless via offscreen readback.

The repository ships as a single make target: `make` builds `bin/test_runner`
**and** `bin/engine`. `make run` first runs the **entire test suite (731 tests /
99 suites) as a self-check**, then boots the full engine (renderer, terrain world,
physics, play-mode character with motion matching, follow camera, demo recorder,
GPU profiler and GPS simulation) and runs its main loop.

---

## ✨ Features

### Core Systems
- **Real-time Rendering** — OpenGL 4.5 with batching, instancing, frustum culling and a persistent buffer pool
- **Render Hardware Interface (RHI)** — dual-backend abstraction in `rhi/`: OpenGL (low/optimized, default, full renderer) and Vulkan (high quality — device/swapchain, depth-tested instanced scene rendering, SPIR-V pipelines compiled from GLSL with `glslc`). Both backends render the same scenes to identical pixels, verified headless via offscreen readback. The Vulkan backend also draws the ImGui editor UI into the swapchain (`imgui_impl_vulkan`) over the 3D scene, and supports a swapchain scene + ImGui overlay.
- **FSR3 Upscaling** — AMD FidelityFX Super Resolution spatial upscaling (EASU edge-directed 4-tap upscaler) plus RCAS sharpness, compiled to compute/SPIR-V at build time; a GLSL 3.30 port of the canonical 12-tap filter backs the GL path (Phase 4+).
- **Motion Vectors** — previous-frame model matrices (`OffscreenInstance.prevModel`, offset 80) feed per-pixel screen-space velocity (RG32f) for temporal reprojection (Phase 3).
- **PBR Materials** — Cook-Torrance BRDF (GGX distribution, Smith visibility, Fresnel-Schlick), ACES tone-mapping and gamma-correct output, shared math across both RHI backends.
- **Distance Fog** — exponential-squared fog with an authoring horizon color; disabled by default so the GL↔Vulkan parity tests stay pixel-identical.
- **Clustered / Forward+ Lighting** — logarithmic-Z, uniform-XY-tiled light binning (CPU layer, `lighting/ClusteredForward.h`) over an inward-facing 6-plane cluster test against light bounding spheres; consumes the unified `LightingEnvironment`.
- **Physical Sky** — analytic Rayleigh + Mie + sun-disk radiance model (`lighting/PhysicalSky.h`) feeding the skybox/scatter tile.
- **CVar Console** — dependency-free runtime variable registry (`lighting/CVar.h`) with O(1) handle access and live `config/cvars.ini` persistence for artist iteration.
- **Physics System** — GJK/EPA collision detection, constraints, rigid bodies and a character controller
- **Motion Matching** — Advanced character locomotion with trajectory prediction and a KD-tree pose search
- **Animation System** — Locomotion FSM, GPU skinning, foot planting / foot IK and animation blending
- **World System** — Chunked terrain with LOD, vegetation, RVT-style terrain pipeline and world-object placement
- **Lighting System** — Dynamic + clustered lighting with UBO-based light management and a single canonical `LightingEnvironment`
- **Memory Management** — Custom memory arenas, memory pools, asset management and caching
- **Geospatial System** — GPS tracking, InfluxDB time-series data, Kalman/TFLite predictive models and terrain-projected digital-twin visualization (Phase 5)
- **Virtualized Geometry** — Optional cluster lists per mesh drive GPU-driven culling (`vkCmdDrawIndexedIndirect` on Vulkan) instead of per-instance draw call dispatch (Phase 1+).

### Editor (Dear ImGui)
- **Dark theme** with toolbar, menu bar, and tabbed panels (Outliner / Layers / World / **Geo**)
- **Graphics backend toggle** — **View → Graphics** switches OpenGL ↔ Vulkan for the next launch (persisted to `graphics_api.cfg`); on Vulkan the editor UI itself renders through the swapchain (`imgui_impl_vulkan`) over the 3D scene
- **Geo tracking panel** — live GPS fix, NMEA feeds, Kalman trajectory prediction, InfluxDB storage, all wired into the left panel (F8)
- **Transform gizmos** — Translate / Rotate / Scale tools
- **Click-to-select picking** — Ray-pick entities in the viewport
- **Entity management** — Create, duplicate, delete, rename (with undo/redo)
- **Inspector** — Edit transform, mesh/material and model properties
- **Scene save/load** — JSON serialization (`scene.json`)
- **Camera modes** — FreeFly, ThirdPerson, FirstPerson, Orbit, Cinematic
- **GPU Profiler** — Hierarchical, persistent GPU/CPU timing with frame-history graphs and CSV export (`AdvancedGPUProfiler`)
- **Debug rendering** — Skeleton debug, physics debug, trajectory overlay

### Engine Entry Point (`test.cpp`)
- **Self-check gate** — runs all 731 unit + integration tests (99 suites) before boot; refuses to boot on failure
- **Play mode** — third-person character with locomotion FSM + motion matching + foot IK on live terrain
- **Scripted cinematic demo** — idle → walk → run → jump → return loop (`CinematicDemo`)
- **Demo recorder** — keyframe camera path recording/playback (`DemoRecorder`)
- **Geospatial sim** — simulated GPS walking track wired into the running engine (`GeoAPI`)
- **Headless mode** — bounded, deterministic (fixed 60 Hz) run for CI with a full engine summary
- **GL-free fallback** — logic simulation of the character + matcher when no OpenGL context exists

---

## 🚀 Quick Start

### Prerequisites (Ubuntu 24.04+)

```bash
sudo apt install build-essential git \
  libglfw3-dev libglew-dev libglm-dev libeigen3-dev libassimp-dev \
  libjsoncpp-dev libcurl4-openssl-dev libopenal-dev libgtest-dev
```

**Optional (Vulkan backend & FSR3):** the Vulkan SDK (for `glslc` and the Vulkan headers/loader) — `make` compiles the RHI GLSL shaders to SPIR-V at build time. The Vulkan backend and the FSR3 compute shaders are built regardless; Vulkan is only *used* at runtime if a driver is present or `--graphics vulkan` is passed.

### Build

```bash
cd "3D GAME ENGINE"
make            # Build bin/test_runner + bin/engine (debug, -g -O0 -DDEBUG)
make editor     # Also build bin/editor_app (ImGui editor application)
make engine     # Build just bin/engine (self-check + engine entry point)
```

`make` (default) builds **both** the test runner and the engine app. The engine app's `main` lives in `test.cpp`; it runs the 731-test self-check first, then boots the engine. `make editor` additionally builds `src/editor_main.cpp` → `bin/editor_app`.

### Run the Engine (interactive)

```bash
make run            # build (if needed) + run bin/engine
./bin/engine        # run directly; picks backend from graphics_api.cfg (opengl)
./bin/engine --graphics vulkan   # force high-quality Vulkan backend (next launch)
```

This builds `bin/engine`, runs the full 731-test self-check, and — if everything
passes — opens the engine window. Boot into the scripted cinematic demo; press any
movement key to take control.

| Flag / Env | Effect |
|-----------|--------|
| `--graphics opengl\|vulkan` / `RTT_GRAPHICS_API=` | Select the RHI backend for this run |
| `--skip-tests` / `RTT_SKIP_TESTS=1` | Skip the test self-check and boot straight into the engine |
| `--headless` / `RTT_HEADLESS=1` | Bounded run with a hidden window (auto-exit; no display → GL-free logic sim) |
| `--frames N` / `RTT_FRAMES=N` | Headless frame budget (default 900) |

### Run the Engine (headless / CI)

```bash
make run-headless            # bounded run, 900 frames
make run-headless FRAMES=300 # custom budget
```

### Editor Application

```bash
make editor   # builds bin/editor_app (ImGui editor, own entry point)
./bin/editor_app --graphics opengl   # low/optimized mode (default)
./bin/editor_app --graphics vulkan   # high-quality mode: live ImGui editor UI
                                     # (menu bar + panels) over the RHI scene
```

### Run Tests

```bash
make test        # run all 731 tests across 99 suites
make test-list   # list every test
```

### Build Modes

```bash
make              # Debug build (default, -g -O0 -DDEBUG)
make MODE=release # Release build (-O2 -DNDEBUG)
make MODE=asan    # AddressSanitizer build (memory debugging)
make test-memory-debug  # ASan + LeakSanitizer test run
```

> Switching `MODE` reuses `build/` — run `make clean` first when changing modes.

### Standalone Verification Apps

These are small, self-contained programs built from `tests/` and ship in `bin/`:

| Target | Binary | Purpose |
|--------|--------|---------|
| `make bot-viewport-test` | `bin/bot_viewport_test` | Interactive bot + ImGui viewport harness |
| `make geoterrain-test` | `bin/geoterrain_test` | GeoTerrain Phase 5 terrain-integration demo |
| `make test-terrain-pipeline` | — (gtest filter) | Heightfield sampling + RVT-baking pipeline tests |
| `make repro_static_destruction` | `bin/repro_static_destruction` | Static-destruction crash regression guard (SIGSEGV = regression) |
| `make bench_soa_cache` | `bin/bench_soa_cache` | Headless micro-benchmark (NOT part of the 731-gate) |

The terrain-pipeline regression guard (`geoterrain-test` / `repro_static_destruction`)
covers the terrain heightfield + world-object asset-invariant suites.

---

## 🧪 Test Suites

The Google Test suite covers every engine system (run one at a time with `make <target>`):

| Target | Coverage |
|--------|----------|
| `make test-physics` | Collision (GJK/EPA), gravity, restitution, friction, raycasts, constraints, character controller |
| `make test-motion-matching` | Pose search, KD-tree, gait phase, foot planting, root motion, blending, hybrid FSM, bone SOA |
| `make test-character` | Character controller, terrain snapping |
| `make test-play-mode` | PlayModeController: load, input, jump, crouch, preview, diagnostics |
| `make test-camera` | Third-person follow, modes, collision, smoothing, NaN regression |
| `make test-memory` | Arenas, pools, stack allocators, stats, stress |
| `make test-math` | Vectors, matrices, quaternions, projections, jitter |
| `make test-world` | Terrain heightmaps, chunks, LOD, normals; scene manager |
| `make test-terrain-pipeline` | Heightfield sampling, texel mapping, RVT baking |
| `make test-rhi` | RHI: GL/Vulkan backends, offscreen 3D scenes, depth + instancing, swapchain present, GL↔Vulkan pixel parity, FSR3 EASU, motion vectors |
| `make test-integration` | Input → FSM → motion matching → pose sync, end to end |
| `make test-quick` | Fast subset (skips slow/perf tests) |
| `make test-list` | List all available tests |

Suites without a dedicated target (animation/blending, FBX, ECS, entity manager,
editor, CVar, lighting/Clustered/Forward+/PhysicalSky, Geo, standalone apps) can be
run via gtest filters, e.g. `./bin/test_runner --gtest_filter='RHI*'`. The full list
is available with `make test-list`.

---

## 📁 Project Structure

```
animationSystem/  Locomotion FSM, motion matching integration, GPU skinning, foot IK, retargeting
boneSystem/       Skeleton definitions, bone debug rendering
cameraSystem/     ThirdPersonCamera, CameraController, cinematic demo script
components/       Core entity/component definitions
demo/             Demo recorder/player, demo configuration
docs/             ARCHITECTURE.md, RENDERING_UPGRADES.md, TROUBLESHOOTING.md
ecs/              Entity-Component-System (archetypes, blueprints, relationships, events, jobs)
editor/           ImGui editor, PlayModeController, EditorApplication, render pipeline, world manager
external/         Third-party libraries (Dear ImGui, JetBrains Mono)
geospatial/       GeoAPI: GPS tracking, InfluxDB, Kalman/TFLite prediction, GeoTerrain Phase 5
include/          GLAD OpenGL loader headers (KHR, glad)
lighting/         Clustered/Forward+, Physical Sky, CVar console, LightingEnvironment, RenderFrameContext
memory/           Memory arenas, pools, asset manager
meshSystem/       Mesh loading and optimization
modelSystem/      FBX/GLTF model loading (Assimp), skeleton, skinning
motionMatching/   KD-tree pose search, trajectory prediction, foot planting
physicsSystem/    GJK/EPA collision, rigid bodies, constraints, character controller
playerSystem/     Character controller
renderer/         Renderer, GPU profilers, debug rendering, post-processing, shadows
rhi/              Render Hardware Interface: RHI.h + OpenGL/Vulkan backends, GLSL→SPIR-V shaders
shaderSystem/     Shader loading, skybox, terrain splat, PBR, post-processing
src/              Editor application main (editor_main.cpp), renderer GLAD loader
tests/            Google Test suite (unit + integration; 731 tests / 99 suites)
tools/            Utility scripts (vtf_to_jpg.py)
utils/            FBXLoader, ModelPlacer helpers
world/            Terrain, vegetation, world objects
config/           Runtime configuration (cvars.ini, graphics_api.cfg, scene.json)
assets/           Character FBX files + locomotion clips (bot.fbx, Idle/Walk/Run/Jump/…)
bin/              Built executables (test_runner, engine, editor_app, standalone apps)
build/            Object files + SPIR-V shaders (generated)
```

**Entry points**
- `test.cpp` — full engine entry point (`make run` / `make engine`): runs the 731-test self-check, then boots the engine main loop.
- `src/editor_main.cpp` — ImGui editor application entry point (`make editor`).
- `tests/test_main.cpp` — Google Test `main()` for the test runner (`make test`).

**Runtime config**
- `graphics_api.cfg` — persisted RHI backend toggle (`opengl` | `vulkan`).
- `config/cvars.ini` — persisted CVar overrides (lighting, fog, sun …).
- `scene.json` — last-saved scene (entity/transform layout).
- `imgui.ini` / `engine_ui.ini` — editor UI layout + panel state.

---

## 📊 System Requirements

- **OS:** Linux (Ubuntu 24.04+; other distros supported with the listed packages)
- **OpenGL:** 4.5 Core Profile (4.6+ works; Mesa radeonsi/llvmpipe verified) — the default backend, drives the full renderer
- **Vulkan (optional):** loader + driver for the high-quality backend; `glslc` (Vulkan SDK or `shaderc` package) compiles the RHI shaders to SPIR-V at build time. The OpenGL fallback runs everywhere OpenGL 4.5 is available.
- **Compiler:** GCC 13+ with C++17 support
- **Dependencies:** GLFW, GLAD, GLM, GLEW, Assimp, Dear ImGui, jsoncpp, libcurl, OpenAL, Eigen3, Google Test
- **Optional:** InfluxDB (time-series storage), TensorFlow Lite (ML prediction), Xvfb (headless CI)

---

## 🎮 Engine Controls

| Input | Action |
|-------|--------|
| **WASD** | Move the character |
| **Space** | Jump |
| **Ctrl / C** | Crouch |
| **Shift** | Sprint |
| **F5** | Toggle play mode (character) |
| **F8** | Toggle the Geo tracking panel (GPS / feeds / prediction / storage) |
| **F9** | Toggle the scripted cinematic demo |
| **Esc** | Quit |

---

## 📖 Documentation

- **[Architecture](docs/ARCHITECTURE.md)** — layered system map, entry points, the
  frame loop, per-system internals, and data flow (input → motion matching → render).
- **[Rendering Upgrades](docs/RENDERING_UPGRADES.md)** — the PBR / shadow / post-FX
  pipeline and how it layers onto the RHI.
- **[Troubleshooting](docs/TROUBLESHOOTING.md)** — missing dependencies, self-check
  failures, the `ASSIMP ERROR` asset warnings, headless runs, crashes (`crash.log`),
  and CI issues.

## 📝 License

MIT License — see [LICENSE](LICENSE). Contributions welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).

Copyright (c) 2026 RTT-Dev
