# RTT Engine - 3D Game Engine
![GitHub stars](https://img.shields.io/github/stars/NtokozoMkhwanazi/RTT-engine-?style=social)
![GitHub forks](https://img.shields.io/github/forks/NtokozoMkhwanazi/RTT-engine-?style=social)
![GitHub issues](https://img.shields.io/github/issues/NtokozoMkhwanazi/RTT-engine-)
![GitHub license](https://img.shields.io/github/license/NtokozoMkhwanazi/RTT-engine-)
![C++](https://img.shields.io/badge/C++17-ISO-blue)
![OpenGL](https://img.shields.io/badge/OpenGL-4.5-orange)
![Build](https://img.shields.io/github/actions/workflow/status/NtokozoMkhwanazi/RTT-engine-/build.yml?branch=main)
![Downloads](https://img.shields.io/github/downloads/NtokozoMkhwanazi/RTT-engine-/total)

**simple lightweight** 3D game engine project with  **systems programming patterns and engineering principles** in mind, built for **scalability and high-performance** using the **power and speed** of the C++ Language. openGL graphics API for relative ease of use compared to Vulkan. Dear imgui UI/UX editor for its seemless integration with th C++ language and backend API openGL . This project contains comprehensive documentation, clear Codebase structure and pdf containing a few of the advanced algorithms used in this project. This project is **actively under-development and maintained**

## 📸 Screenshots

**Dear imGui UI editor**
<img width="1366" height="768" alt="viepot" src="https://github.com/user-attachments/assets/b8bd8544-5739-4a15-b81c-fa2bf12d63a9" />
<img width="1366" height="768" alt="viewport empty" src="https://github.com/user-attachments/assets/e0d36883-8889-43ec-bd5b-18d53fac4865" />

**FBX/GLFT etc characters/models**
<img width="1366" height="768" alt="T-pose" src="https://github.com/user-attachments/assets/fb9d43b1-72b3-4a21-82b4-f63889d07845" />
<img width="1366" height="768" alt="snip" src="https://github.com/user-attachments/assets/fb06a506-05c5-4fff-9830-b11a07839c84" />
<img width="1366" height="768" alt="screen3" src="https://github.com/user-attachments/assets/1328b559-cbe9-4242-93af-6f3bc89dea0c" />
<img width="1366" height="768" alt="2" src="https://github.com/user-attachments/assets/f3413e3f-f0d7-4970-8f61-9c3048d2344c" />

## 🎯 Features

### Core Systems
- **ECS Architecture** - Cache-coherent entity-component-system with archetype storage
- **Real-time Rendering** - OpenGL 4.5 with batching and instancing
- **Physics System** - GJK/EPA collision detection, constraints, character controller
- **Animation System** - Hybrid FSM + Motion Matching, GPU skinning, IK
- **Motion Matching** - Advanced character locomotion with trajectory prediction

### Editor
- **Unreal Engine-style UI (I LIKE UNREAL-ENGINE :))** - Dark theme, tabbed panels, maximized viewport 
- **Live Statistics** - FPS, GPU time, entity count in menu bar
- **GPU Profiler** - Hierarchical profiling with timestamp queries
- **Entity Inspector** - Transform, mesh, component editing
- **Asset Browser** - Content management with import/export

### Performance
- **Archetype-based ECS** - Cache-coherent iteration
- **GPU Instancing** - Efficient rendering of multiple objects
- **LOD System** - Level of detail for complex models
- **Batch Rendering** - Minimized draw calls

## 📁 Project Structure

```
3D GAME ENGINE/
├── ecs/                    # Entity-Component-System framework
├── renderer/               # OpenGL rendering, GPU profiler
├── animationSystem/        # Animation, motion matching, IK
├── physicsSystem/          # Collision detection, physics simulation
├── modelSystem/            # Model loading, mesh management
├── boneSystem/             # Skeleton, bone animation
├── cameraSystem/           # Camera controllers
├── world/                  # Terrain, world objects
├── memory/                 # Memory management, asset manager
├── meshSystem/             # Mesh optimization, LOD
├── lighting/               # Lighting system
├── shaderSystem/           # Shader management
├── playerSystem/           # Character controller
├── motionMatching/         # Motion matching algorithms
├── demo/                   # Demo recording/playback
├── tests/                  # Unit tests
├── docs/                   # Documentation
├── assets/                 # Game assets
└── external/               # Third-party libraries (ImGui, etc.)
```

## 🚀 Quick Start

### Build
```bash
cd "3D GAME ENGINE"
make clean && make
```

### Run Editor
```bash
./bin/test
```

### Controls
| Input | Action |
|-------|--------|
| **Right-click + Drag** | Look around viewport |
| **WASD** | Move camera |
| **W/E/R** | Select Translate/Rotate/Scale gizmo |
| **X** | Toggle World/Local space |
| **\** | Toggle Console panel |
| **Ctrl+D** | Duplicate selected entity |
| **Delete** | Delete selected entity |

### Expected Display (see issue ) 
The viewport shows:
- **Red cube** at (0, 1, 0)
- **Green cube** at (2, 2, 0)
- **Blue cube** at (-2, 3, 0)

## 📊 System Requirements

- **OS:** Linux (Ubuntu 24.04+)
- **OpenGL:** 4.5 Core Profile
- **Compiler:** GCC 13+ with C++17 support
- **Dependencies:** GLFW, GLAD, GLM, Assimp, ImGui

## 📖 Documentation

### Getting Started
- [README.md](docs/general/README.md) - Engine overview
- [IMPORT_MODELS_GUIDE.md](docs/general/IMPORT_MODELS_GUIDE.md) - Model import guide
- [TESTING_GUIDE.md](docs/general/TESTING_GUIDE.md) - Testing instructions

### System Documentation
- [ECS Architecture](ecs/README.md) - Entity-Component-System
- [Renderer](renderer/README.md) - Rendering pipeline
- [Animation](docs/animation/) - Animation system docs
- [Physics](docs/physics/) - Physics system docs
- [World](docs/world/) - Terrain and world system

### Technical Guides
- [GPU Profiler](renderer/GPU_PROFILER_UI_SUMMARY.md) - Performance profiling
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
make test                    # Run all tests
make test-animation          # Animation tests
make test-physics            # Physics tests
make test-memory-debug       # Memory debug tests
```

### Code Style
- C++17 standard
- Consistent naming conventions
- RAII resource management
- Smart pointers for ownership

## 📈 Performance Metrics

| Metric | Target | Current |
|--------|--------|---------|
| **FPS** | 60+ | 60+ |
| **Frame Time** | <16ms | ~16ms |
| **Draw Calls** | <100 | Batched |
| **Entity Count** | 1000+ | Supported |

