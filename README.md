# RTT Engine - 3D Game Engine

![GitHub stars](https://img.shields.io/github/stars/NtokozoMkhwanazi/RTT-engine-?style=social)
![GitHub forks](https://img.shields.io/github/forks/NtokozoMkhwanazi/RTT-engine-?style=social)
![GitHub issues](https://img.shields.io/github/issues/NtokozoMkhwanazi/RTT-engine-)
![GitHub license](https://img.shields.io/github/license/NtokozoMkhwanazi/RTT-engine-)
![C++](https://img.shields.io/badge/C++17-ISO-blue)
![OpenGL](https://img.shields.io/badge/OpenGL-4.5-orange)

**Advanced lightweight** 3D game engine built for **scalability and high-performance systems**: animation, 3D software simulation and real-time rendering, written in **C++17** with the **OpenGL 4.5** graphics API and a **Dear ImGui** editor for seamless integration with C++ and OpenGL. The project ships with comprehensive documentation, a clean module structure, and is **actively under development**. It is designed for learning by building - made for curious minds.

## 📸 GIFs

**CAM/SKYBOX Test**

<img width="480" height="270" alt="output" src="https://github.com/user-attachments/assets/fb9adf56-fb97-4cd9-b99e-fe698d9a6212" />

**Animation Test 1**

<img width="480" height="270" alt="output" src="https://github.com/user-attachments/assets/e2c13e2a-4fa5-41ad-9679-0c0f924f71bb" />

**Animation Test 2**

<img width="480" height="270" alt="output" src="https://github.com/user-attachments/assets/5ec8a99c-2006-4f65-9dc4-d4b4a7f04fe6" />

**Animation Test 3**

<img width="480" height="270" alt="output" src="https://github.com/user-attachments/assets/098cabf7-6dad-4f10-9938-09c2a255a06e" />

## 📊 Architecture Diagrams (ECS Overview)

- **System**
<img width="2667" height="1074" alt="mermaid-diagram(7)" src="https://github.com/user-attachments/assets/919364eb-e57f-412b-af20-72d82f0e8901" />

---
- **Entity**
<img width="5540" height="364" alt="mermaid-diagram(6)" src="https://github.com/user-attachments/assets/7dce4e1a-8394-4157-9d80-0d1c6ca5c352" />

---
- **Component**
<img width="3123" height="760" alt="mermaid-diagram(5)" src="https://github.com/user-attachments/assets/e5767418-9135-4b92-aec9-4bdf0d0af683" />

---
- **Data Flow**
<img width="1755" height="526" alt="mermaid-diagram(4)" src="https://github.com/user-attachments/assets/f459fe97-bdba-4ce3-8ef8-58019b20cff9" />

---
- **ECS**
<img width="2562" height="824" alt="mermaid-diagram(3)" src="https://github.com/user-attachments/assets/80346fea-d0ce-42d7-bae4-f1ca803893df" />

## 🎯 Features

### Core Systems
- **ECS Architecture** - Cache-coherent entity-component-system with archetype storage, relationships and events
- **Real-time Rendering** - OpenGL 4.5 with batching, instancing, frustum culling and a persistent buffer pool
- **Physics System** - GJK/EPA collision detection, constraints, rigid bodies and a character controller
- **Animation System** - Hybrid FSM + Motion Matching, GPU skinning, foot planting and root motion
- **Motion Matching** - Advanced character locomotion with trajectory prediction and a KD-tree pose search
- **World System** - Chunked terrain with LOD, vegetation and world-object placement
- **Blueprints / Prefabs** - Reusable entity templates with serialization to JSON

### Editor (Dear ImGui)
- **Unreal-style layout** - Dark theme with toolbar, menu bar, tabbed side/bottom panels and a maximized viewport
- **Transform gizmos** - Translate / Rotate / Scale tools (W/E/R) with world/local space, axis hover highlighting and full drag interaction
- **Undo / Redo** - History across gizmo + inspector transform edits, entity create/delete/duplicate and blueprint spawns (Ctrl+Z / Ctrl+Y), persisted in saved scenes; rapid drags coalesce into one step
- **Click-to-select picking** - Ray-pick entities in the viewport; gold selection outline around the selection
- **Name labels** - Entity name tags projected in the viewport, distance-faded (N)
- **Drag-to-spawn** - Drag entities/blueprints from their panels into the viewport to spawn at the terrain point under the cursor, with a wireframe ghost preview
- **Entity management** - Create (GameObject menu: lights, cameras, primitives, model loader), duplicate (Ctrl+D), delete (Del), rename
- **Inspector** - Edit transform, mesh/material and model properties on the selected entity
- **Outliner** - Searchable entity hierarchy with icons and right-click context menu
- **Blueprints panel** - Create a blueprint from the selection, spawn instances, save/load `blueprints/*.json`
- **Scene save/load** - `scene.json` round-trip for entity transforms (Ctrl+S / Ctrl+O)
- **Wireframe mode** - Toggle wireframe rendering of the whole scene (F)
- **Camera modes** - FreeFly, ThirdPerson, FirstPerson, Orbit (auto-orbit), Cinematic intro
- **Play mode** - Drive the playable character with WASD; timeline recording (V), playback (P) and MP4 export (F8)
- **Diagnostics** - GPU profiler, FPS/stat overlays, animation debug skeleton (F6), motion-matching trajectory (T)

### Performance
- **Archetype-based ECS** - Cache-coherent iteration
- **GPU Instancing** - Efficient rendering of multiple objects
- **LOD System** - Level of detail for terrain and complex models
- **Batch Rendering** - Minimized draw calls with shader/VAO sorting

## 🚀 Quick Start

### Prerequisites
```bash
sudo apt install build-essential cmake git \
  libglfw3-dev libglew-dev libassimp-dev libjsoncpp-dev \
  libcurl4-openssl-dev libopenal-dev libgtest-dev
```

### Build
```bash
cd "3D GAME ENGINE"
make            # debug build (produces ./app)
```

### Run Editor
```bash
./app           # or: make run
```

> The editor auto-loads `scene.json` at startup and restores your last camera
> mode from `camera_mode.cfg`.

### Controls
| Input | Action |
|-------|--------|
| **Mouse over viewport** | Look around (FreeFly / Orbit / FirstPerson / ThirdPerson) |
| **WASD** | Move camera (FreeFly / FirstPerson) |
| **Scroll wheel** | Zoom in / out |
| **Middle-drag** | Pan the view |
| **C** | Toggle cursor-lock FPS look (Esc releases) |
| **1 - 5** | Camera mode: FreeFly / ThirdPerson / FirstPerson / Orbit / Cinematic |
| **O** | Toggle auto-orbit (Orbit mode) |
| **W / E / R** | Gizmo tool: Translate / Rotate / Scale |
| **LMB on gizmo + drag** | Move / rotate / scale the selected entity |
| **LMB on entity** | Select (or click empty space to deselect) |
| **F** | Toggle wireframe rendering |
| **N** | Toggle entity name labels in the viewport |
| **Delete** | Delete the selected entity |
| **Ctrl+D** | Duplicate the selected entity |
| **Ctrl+Z / Ctrl+Y** | Undo / redo (gizmo drags, create/delete/duplicate, blueprint spawns) |
| **Drag row → viewport** | Spawn a copy (outliner) or instance (blueprints) at the drop point |
| **F5** | Play / stop (Esc also stops) |
| **M** | Toggle motion-matching locomotion |
| **V** | Record animation timeline |
| **P** | Playback recorded timeline |
| **F8** | Save timeline + encode MP4 |
| **T** | Toggle motion-matching trajectory overlay |
| **F6** | Toggle animation debug skeleton overlay |

### Editor Layout
```
┌──────────────────────┬────────────────────────────┬──────────────┐
│  Menu bar            │                            │              │
│  Toolbar             │        VIEWPORT            │   Details    │
│  ┌───────┬───────────┤   (grid + gizmos +         │   (inspector)│
│  │       │           │    selection outline)      │              │
│  │ Scene │           │                            │              │
│  │Outliner│          │                            │              │
│  ├───────┤           ├────────────────────────────┤              │
│  │       │           │ Content / Console /        │              │
│  │       │           │ Profiler / Blueprints      │              │
│  └───────┴───────────┴────────────────────────────┴──────────────┘
│  Status bar                                                        │
└────────────────────────────────────────────────────────────────────┘
```

## 📊 System Requirements

- **OS:** Linux (Ubuntu 24.04+)
- **OpenGL:** 4.5 Core Profile
- **Compiler:** GCC 13+ with C++17 support
- **Dependencies:** GLFW, GLAD, GLM, GLEW, Assimp, Dear ImGui, jsoncpp, libcurl, OpenAL (Google Test for `make test`)

## 📖 Documentation

### Getting Started
- [Engine overview](docs/general/README.md)
- [Controls reference](docs/general/CONTROLS.md) - Full keyboard/mouse reference
- [Model import guide](docs/general/IMPORT_MODELS_GUIDE.md)
- [Testing guide](docs/general/TESTING_GUIDE.md)
- [Contributing](CONTRIBUTING.md) - Build modes, tests, code style
- [Changelog](CHANGELOG.md)

### System Documentation
- [ECS Architecture](ecs/README.md) - Entity-Component-System
- [Renderer](renderer/README.md) - Rendering pipeline
- [Editor module](editor/README.md) - Editor architecture
- [Animation](docs/animation/) - Animation system docs
- [Physics](docs/physics/) - Physics system docs
- [World](docs/world/) - Terrain and world system

### Technical Guides
- [GPU Profiler](docs/GPU_PROFILER_UI_SUMMARY.md) - Performance profiling
- [Memory Management](docs/memory/) - Memory optimization
- [Motion Matching](docs/animation/MOTION_MATCHING_COMPLETE.md) - Character locomotion

## 🛠️ Development

### Build Modes
```bash
make              # Debug build
make MODE=release # Release build (optimized)
make MODE=asan    # AddressSanitizer build
```

### Testing
```bash
make test                    # Build + run all tests
make test-animation          # Animation system tests
make test-physics            # Physics system tests
make test-camera             # Camera system tests
make test-character          # Character controller tests
make test-quick              # Fast subset (skips slow/perf tests)
make test-memory-debug       # ASan + LeakSanitizer test run
```

### Project Structure
```
ecs/            Entity-Component-System (archetypes, blueprints, systems)
animationSystem/  FSM + motion matching, GPU skinning, IK
modelSystem/    FBX loading (Assimp), model registry, animation playback
physicsSystem/  Collision, rigid bodies, constraints, raycasting
renderer/       Renderer, debug renderer, GPU profiler, engine UI
editor/         ImGui editor: panels, gizmos, entity/scene/blueprint management
shaderSystem/   Shader loading and skybox
world/          Terrain, vegetation, world objects
motionMatching/ KD-tree pose search, trajectory prediction, foot planting
tests/          Google Test suite (unit + integration)
```

### Code Style
- C++17 standard
- Consistent naming conventions
- RAII resource management
- Smart pointers for ownership

## 📈 Performance Targets

| Metric | Target | Current |
|--------|--------|---------|
| **FPS** | 60+ | 60+ |
| **Frame Time** | <16ms | ~16ms |
| **Draw Calls** | <100 | Batched |
| **Entity Count** | 1000+ | Supported |
