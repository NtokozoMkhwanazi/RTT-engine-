# RTT Engine — 3D Game Engine

![C++](https://img.shields.io/badge/C%2B%2B-17-blue)
![OpenGL](https://img.shields.io/badge/OpenGL-4.5-orange)
![Vulkan](https://img.shields.io/badge/Vulkan-1.0-purple)
![Platform](https://img.shields.io/badge/Platform-Linux-6f42c1)
![Tests](https://img.shields.io/badge/Tests-604%20passing-brightgreen)
![License](https://img.shields.io/badge/License-MIT-green)

**Advanced lightweight 3D game engine** built for scalability and high-performance
systems: real-time rendering, animation, motion matching, physics and geospatial
simulation — written in **C++17** on a **dual-backend RHI** (OpenGL 4.5 for
low/optimized machines, Vulkan for high-quality rendering) with a **Dear ImGui**
editor. Pick the backend at launch with `--graphics opengl|vulkan`.

The repository ships as a single entry point: `make run` first runs the **entire
test suite (604 tests / 66 suites) as a self-check**, then boots the full engine
(renderer, terrain world, physics, play-mode character with motion matching, follow
camera, demo recorder and GPS simulation) and runs its main loop.

---

## ✨ Features

### Core Systems
- **Real-time Rendering** — OpenGL 4.5 with batching, instancing, frustum culling and a persistent buffer pool
- **Render Hardware Interface (RHI)** — dual-backend abstraction in `rhi/`: OpenGL (low/optimized, default) and Vulkan (high quality — device/swapchain, depth-tested instanced scene rendering, SPIR-V pipelines compiled from GLSL with `glslc`). Both backends render the same scenes to identical pixels, verified headless via offscreen readback
- **Physics System** — GJK/EPA collision detection, constraints, rigid bodies and a character controller
- **Motion Matching** — Advanced character locomotion with trajectory prediction and a KD-tree pose search
- **Animation System** — Locomotion FSM, GPU skinning, foot planting / foot IK and animation blending
- **World System** — Chunked terrain with LOD, vegetation and world-object placement
- **Lighting System** — Dynamic lighting with UBO-based light management
- **Memory Management** — Custom memory arenas, memory pools, asset management and caching
- **Geospatial System** — GPS tracking, InfluxDB time-series data, Kalman/TFLite predictive models

### Editor (Dear ImGui)
- **Dark theme** with toolbar, menu bar, and tabbed panels (Outliner / Layers / World / **Geo**)
- **Graphics backend toggle** — **View → Graphics** switches OpenGL ↔ Vulkan for the next launch (persisted to `graphics_api.cfg`); on Vulkan the editor UI itself renders through the swapchain (imgui_impl_vulkan) over the 3D scene
- **Geo tracking panel** — live GPS fix, NMEA feeds, Kalman trajectory prediction, InfluxDB storage, all wired into the left panel (F8)
- **Transform gizmos** — Translate / Rotate / Scale tools
- **Click-to-select picking** — Ray-pick entities in the viewport
- **Entity management** — Create, duplicate, delete, rename (with undo/redo)
- **Inspector** — Edit transform, mesh/material and model properties
- **Scene save/load** — JSON serialization
- **Camera modes** — FreeFly, ThirdPerson, FirstPerson, Orbit, Cinematic
- **GPU Profiler** — Performance diagnostics and frame analysis
- **Debug rendering** — Skeleton debug, physics debug, trajectory overlay

### Engine Entry Point (`test.cpp`)
- **Self-check gate** — runs all 604 unit + integration tests before boot; refuses to boot on failure
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

### Build

```bash
cd "3D GAME ENGINE"
make            # Build the test runner (debug)
```

### Run the Engine (interactive)

```bash
make run
```

This builds `bin/engine`, runs the full 604-test self-check, and — if everything
passes — opens the engine window. Boot into the scripted cinematic demo; press any
movement key to take control.

| Flag / Env | Effect |
|-----------|--------|
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
make test        # run all 604 unit + integration tests
make test-list   # list every test
```

---

## 🧪 Test Suites

The Google Test suite covers every engine system (run one at a time with `make <target>`):

| Target | Coverage |
|--------|----------|
| `make test-physics` | Collision (GJK/EPA), gravity, restitution, friction, raycasts |
| `make test-motion-matching` | Pose search, gait phase, foot planting, root motion |
| `make test-character` | Character controller, terrain snapping |
| `make test-play-mode` | PlayModeController: load, input, jump, crouch, preview, diagnostics |
| `make test-camera` | Third-person follow, modes, collision, smoothing, NaN regression |
| `make test-memory` | Arenas, pools, stack allocators, stats, stress |
| `make test-math` | Vectors, matrices, quaternions, projections |
| `make test-world` | Terrain heightmaps, chunks, LOD, normals; scene manager |
| `make test-integration` | Input → FSM → motion matching → pose sync, end to end |
| `make test-quick` | Fast subset (skips slow/perf tests) |
| `make test-rhi` | RHI: GL/Vulkan backends, offscreen 3D scenes, depth + instancing, swapchain present, GL↔Vulkan pixel parity |
| `make test-list` | List all available tests |

Suites without a dedicated target (animation/blending, FBX, ECS, entity manager, geo)
can be run via gtest filters, e.g. `./bin/test_runner --gtest_filter='Animation*'`.
The full list is available with `make test-list`.

## 🛠️ Build Modes

```bash
make              # Debug build (default, -g -O0 -DDEBUG)
make MODE=release # Release build (-O2 -DNDEBUG)
make MODE=asan    # AddressSanitizer build (memory debugging)
make test-memory-debug  # ASan + LeakSanitizer test run
```

> Switching `MODE` reuses `build/` — run `make clean` first when changing modes.

---

## 📁 Project Structure

```
animationSystem/  Locomotion FSM, motion matching integration, GPU skinning, foot IK, retargeting
boneSystem/       Skeleton definitions, bone debug rendering
cameraSystem/     ThirdPersonCamera, CameraController, cinematic demo script
components/       Core entity/component definitions
demo/             Demo recorder/player, demo configuration
ecs/              Entity-Component-System (archetypes, blueprints, relationships, events, jobs)
editor/           ImGui editor, PlayModeController, EditorApplication, render pipeline, world manager
geospatial/       GeoAPI: GPS tracking, InfluxDB, Kalman/TFLite prediction
lighting/         Dynamic lighting system
memory/           Memory arenas, pools, asset manager
meshSystem/       Mesh loading and optimization
modelSystem/      FBX/GLTF model loading (Assimp), skeleton, skinning
motionMatching/   KD-tree pose search, trajectory prediction, foot planting
physicsSystem/    GJK/EPA collision, rigid bodies, constraints, character controller
playerSystem/     Character controller
renderer/         Renderer, GPU profilers, debug rendering
rhi/              Render Hardware Interface: RHI.h + OpenGL/Vulkan backends, SPIR-V shaders
shaderSystem/     Shader loading, skybox rendering
src/              Editor application main, GLAD OpenGL loader
tests/            Google Test suite (unit + integration; 604 tests)
world/            Terrain, vegetation, world objects
external/         Third-party libraries (Dear ImGui, JetBrains Mono)
assets/           Character FBX files + locomotion clips (bot.fbx, Idle/Walk/Run/Jump/…)
bin/              Built executables (test_runner, engine, editor_app)
build/            Object files (generated)
```

**Entry points**
- `test.cpp` — full engine entry point (self-check tests → engine main loop)
- `src/editor_main.cpp` — ImGui editor application entry point (`make editor`)

---

## 📊 System Requirements

- **OS:** Linux (Ubuntu 24.04+; other distros supported with the listed packages)
- **OpenGL:** 4.5 Core Profile (4.6+ works; Mesa radeonsi/llvmpipe verified) — the default backend
- **Vulkan (optional):** loader + driver for the high-quality backend; `glslc` (Vulkan SDK or `shaderc` package) compiles the RHI shaders to SPIR-V at build time
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
- **[Troubleshooting](docs/TROUBLESHOOTING.md)** — missing dependencies, self-check
  failures, the `ASSIMP ERROR` asset warnings, headless runs, crashes (`crash.log`),
  and CI issues.

## 📝 License

MIT License — see [LICENSE](LICENSE). Contributions welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).

Copyright (c) 2026 RTT-Dev
