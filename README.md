# RTT Engine - 3D Game Engine

A professional 3D game engine with ECS architecture, real-time rendering, physics, and animation systems.

## 🎯 Features

### Core Systems
- **ECS Architecture** - Cache-coherent entity-component-system with archetype storage
- **Real-time Rendering** - OpenGL 4.5 with batching and instancing
- **Physics System** - GJK/EPA collision detection, constraints, character controller
- **Animation System** - Hybrid FSM + Motion Matching, GPU skinning, IK
- **Motion Matching** - Advanced character locomotion with trajectory prediction

### Editor
- **Unreal Engine-style UI** - Dark theme, tabbed panels, maximized viewport
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

### Expected Display
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

## 🤝 Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Run tests: `make test`
5. Submit a pull request

## 📄 License

[Your License Here]

## 🙏 Acknowledgments

- **Dear ImGui** - Immediate mode GUI library
- **Assimp** - 3D model import library
- **GLFW** - Window management
- **GLM** - OpenGL mathematics

---

**Status:** ✅ Production Ready (Core Features)
**Last Updated:** March 27, 2025
**Version:** 1.0.0
