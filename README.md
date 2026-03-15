# 🎮 RTT-Engine

<div align="center">

**C++ 3D game engine**

[![C++](https://img.shields.io/badge/C%2B%2B-17-blue?style=for-the-badge&logo=c%2B%2B)]()
[![OpenGL](https://img.shields.io/badge/OpenGL-3.3+-green?style=for-the-badge&logo=opengl)]()
[![Platform](https://img.shields.io/badge/Platform-Linux-yellow?style=for-the-badge)]()
[![ECS](https://img.shields.io/badge/Architecture-ECS-purple?style=for-the-badge)]()
[![FPS](https://img.shields.io/badge/FPS-60%2B-success?style=for-the-badge)]()

</div>

---

## 📖 Overview

Welcome to **RTT-Engine** — a modern 3D Game Engine built with a strong focus on:
- **Entity Component System (ECS)** architecture for clean, data-oriented design
- **Computer Graphics** and real-time rendering techniques
- **Applied Mathematics** for physics, animation, and spatial algorithms
- **Software Engineering** principles and optimization techniques

---

## ⚡ Performance

| Metric | Value |
|--------|-------|
| **Frame Time** | 8ms (125 FPS) |
| **Draw Calls** | ~50 |
| **Triangles** | ~50K |
| **Bone Upload** | 0.007ms |
| **Motion Search** | 0.02ms |

---

## 🏗️ Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        ecs_test.cpp (Main)                      │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    ecs::World (ECS Container)                   │
│  ┌──────────────────┐  ┌──────────────────┐                     │
│  │  EntityManager   │  │ ComponentManager │                     │
│  └──────────────────┘  └──────────────────┘                     │
│                                                                 │
│  ┌────────────┐ ┌────────────┐ ┌────────────┐ ┌────────────┐    │
│  │  Camera    │ │  Physics   │ │  Render    │ │ Animation  │    │
│  │  System    │ │  System    │ │  System    │ │  System    │    │
│  └────────────┘ └────────────┘ └────────────┘ └────────────┘    │
└─────────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        ▼                     ▼                     ▼
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│  Components  │    │   Systems    │    │   Engine     │
│  (Data)      │    │   (Logic)    │    │   Modules    │
├──────────────┤    ├──────────────┤    ├──────────────┤
│ Transform    │    │ RenderSystem │───►│ ModelManager │
│ Mesh         │    │ PhysicsSys   │───►│ Terrain      │
│ Camera       │    │ CameraSys    │    │ Animation    │
│ RigidBody    │    │ AnimationSys │───►│ (Legacy)     │
│ Animator     │    │ LightSystem  │    │              │
│ Light        │    │              │    │              │
└──────────────┘    └──────────────┘    └──────────────┘
```

### ECS Design Philosophy

| Concept | Description |
|---------|-------------|
| **Entities** | Unique IDs with component signatures |
| **Components** | Pure data (no logic) |
| **Systems** | Logic that operates on entities with specific components |
| **World** | Container managing entities, components, and systems |

---

## 🎮 Features

### Core Systems

**ECS Framework**
- Type-safe component management
- System filtering and priorities
- Entity signature matching
- Cache-friendly component arrays

**Motion Matching**
- SAH-optimized KD-Tree (12 bins)
- 10,980 pose database
- 0.02ms search time
- Continuous pose searching

**Bone Matrix Buffer**
- Auto UBO/SSBO selection
- GPU skinning optimization

**World Rendering**
- Frustum culling
- Distance LOD (4 levels)
- Instanced rendering
- Occlusion culling
- Texture atlasing

**Memory Management**
- Memory arenas (O(1) allocation)
- Object pools (O(1) alloc/free)
- Asset manager with reference counting

**Foot IK System**
- Dynamic terrain height tracking
- Enhanced foot planting detection
- Real-time floor height updates

### Rendering
- OpenGL 3.3+ pipeline
- GPU skinning with bone buffer
- Batch rendering with instancing
- Multiple light types (Directional, Point, Spot)
- Normal mapping and PBR materials
- Skybox with cube mapping
- Atmospheric fog

### World Streaming
- Procedural terrain (Perlin noise)
- Chunk-based LOD (4 levels)
- 800m × 800m explorable areas
- Height-based biomes
- Water system with waves
- Vegetation placement

### Animation
- Skeletal animation with GPU skinning
- Animation blending and layering
- Root motion extraction
- Foot IK with terrain adaptation
- State machines
- Mixamo compatibility
- FBX import via Assimp

### Physics
- Collision detection (boxes, spheres, capsules)
- Rigid body dynamics
- Continuous collision detection
- Raycasting
- Constraints (hinge, slider, springs)
- Character controller

### Character Controller
- Third-person movement with root motion
- WASD locomotion
- Jump, Sprint, Crouch
- Smooth rotation
- Camera modes: Fixed, Orbit, Follow
- Smooth zoom (8-30m)

### Debug Tools
- GPU Profiler (OpenGL timestamp queries)
- Foot IK Debug (Press G)
- Motion Matching Debug (Press H)
- Performance Stats (Press F1)

---

## 📁 Project Structure

```
3D GAME ENGINE/
├── ecs_test.cpp              # Main application (ECS-based)
├── ecs/                      # ECS Framework
│   ├── ECS.h                 # Main include
│   ├── Entity.h              # Entity types and IDs
│   ├── Component.h           # Component base and arrays
│   ├── EntityManager.h       # Entity lifecycle
│   ├── ComponentManager.h    # Component storage
│   ├── System.h              # System base class
│   ├── World.h               # ECS container
│   ├── README.md             # ECS documentation
│   ├── components/           # Component definitions
│   │   ├── TransformComponent.h
│   │   ├── MeshComponent.h
│   │   ├── CameraComponent.h
│   │   ├── RigidBodyComponent.h
│   │   ├── AnimatorComponent.h
│   │   ├── LightComponent.h
│   │   ├── TagComponent.h
│   │   └── ...
│   └── systems/              # System implementations
│       ├── RenderSystem.h
│       ├── PhysicsSystem.h
│       ├── AnimationSystem.h
│       └── ...
├── animationSystem/          # Animation (legacy, integrated)
├── motionMatching/           # Motion matcher, KD-Tree
├── boneSystem/               # Skeleton, bone naming
├── physicsSystem/            # Physics, rigid bodies
├── modelSystem/              # Model loading, meshes
├── playerSystem/             # Character controller
├── cameraSystem/             # Camera implementation
├── shaderSystem/             # Shaders, skybox
├── renderer/                 # Renderer, GPU profiler
├── lighting/                 # Light management
├── memory/                   # Arenas, asset manager
├── world/                    # Terrain, chunks, vegetation
├── meshSystem/               # Mesh handling
├── demo/                     # Demo recording
├── utils/                    # Utilities (FBXLoader, etc.)
├── tests/                    # Unit tests
├── include/                  # GLAD, GLM, KHR
├── assets/                   # FBX models, animations
├── legacy/                   # Old files (backup)
│   ├── ecs_old/              # Previous ECS attempt
│   ├── old_tests/            # Old test files
│   └── old_implementations/  # Deprecated code
├── Makefile                  # Build configuration
├── ECS_ARCHITECTURE.md       # Architecture diagrams
└── bin/                      # Executables
```

---

## 🎮 Controls

| Key | Action |
|-----|--------|
| **W / S** | Move forward / backward |
| **A / D** | Strafe left / right |
| **SPACE** | Jump |
| **LEFT SHIFT** | Sprint |
| **LEFT CTRL** | Crouch |
| **C** | Toggle camera mode |
| **Mouse** | Orbit camera |
| **Scroll** | Zoom (8-30m) |
| **Q / E** | Camera pivot |
| **R** | Reset camera |
| **F** | Toggle wireframe |
| **B** | Bone debug |
| **G** | Foot IK status |
| **H** | Motion matching debug |
| **F1** | GPU stats |
| **ESC** | Exit |

---

## 🛠️ Dependencies

| Library | Installation (Ubuntu) |
|---------|----------------------|
| GLFW | `sudo apt install libglfw3-dev` |
| GLM | `sudo apt install libglm-dev` |
| Assimp | `sudo apt install libassimp-dev` |
| OpenAL | `sudo apt install libopenal-dev` |
| zlib | `sudo apt install zlib1g-dev` |
| Google Test | `sudo apt install libgtest-dev` |

GLAD is included in `include/`.

---

## ⚙️ Building

```bash
# Debug build
make

# Release build
make MODE=release

# Clean build
make clean && make

# Run engine
./bin/ecs_test

# Run with debug mode
make MODE=debug && ./bin/ecs_test
```

### Build Status
```
✅ Compilation: SUCCESS
✅ ECS Integration: COMPLETE
✅ Legacy Systems: INTEGRATED
```

---

## 🧪 Testing

The engine uses Google Test for unit testing.

```bash
# Run all tests
make test

# Run specific test suites
make test-animation    # Animation system tests
make test-physics      # Physics tests
make test-camera       # Camera tests
make test-memory       # Memory management tests
```

### Test Suites

| Suite | Tests | Status |
|-------|-------|--------|
| Animation | 17 | ✅ |
| Animation FSM | 20 | ✅ |
| Camera | 77 | ✅ |
| Character Controller | 24 | ✅ |
| Integration | 19 | ✅ |
| Math | 19 | ✅ |
| Memory Management | 25 | ✅ |
| Motion Matching | 14 | ✅ |
| Physics | 13 | ✅ |
| Terrain | 7 | ✅ |

---

## 📊 Metrics

| Metric | Value |
|--------|-------|
| **World Size** | 800m × 800m |
| **Active Chunks** | 45-70 |
| **Total Vertices** | ~50K (with LOD) |
| **Max Trees/Chunk** | 80 |
| **Frame Time** | 8ms (125 FPS) |
| **Motion Search** | 0.02ms |
| **Bone Upload** | 0.007ms |
| **Entity Count** | Dynamic (ECS) |
| **Component Types** | 15+ |
| **Systems** | 9 |

---

## ✅ Implementation Status

### Complete
- ✅ ECS architecture (Entity, Component, System, World)
- ✅ 15+ component types
- ✅ 9 system implementations
- ✅ Procedural terrain generation
- ✅ Chunk streaming & LOD
- ✅ Frustum & occlusion culling
- ✅ Animated water system
- ✅ Vegetation instanced rendering
- ✅ Skeletal animation
- ✅ Motion matching system
- ✅ Bone matrix buffer (UBO/SSBO)
- ✅ Foot IK with terrain tracking
- ✅ Physics collision detection
- ✅ Rigid body dynamics
- ✅ Character controller
- ✅ Camera system
- ✅ Texture atlasing
- ✅ GPU profiling
- ✅ Memory arenas & pools
- ✅ Asset manager
- ✅ NaN prevention

### In Progress
- 🔄 Grass instanced rendering integration
- 🔄 Rock model rendering
- 🔄 Texture splatting
- 🔄 Water collision
- 🔄 Shadow mapping
- 🔄 Full audio integration

### Planned
- ⏳ Infinite terrain streaming
- ⏳ Day/night cycle
- ⏳ Weather system
- ⏳ Advanced lighting (PBR)
- ⏳ Particle system
- ⏳ UI system
- ⏳ Scripting support (Lua/Python)

---

## 🔧 Configuration

Edit in `ecs_test.cpp`:

```cpp
// Terrain
terrainConfig.chunkSize = 100.0f;
terrainConfig.viewDistance = 8;
terrainConfig.lodDistance = 50.0f;
terrainConfig.heightScale = 80.0f;

// Vegetation
vegConfig.treeDensity = 0.02f;
vegConfig.maxTreesPerChunk = 80;

// Camera
cameraDistance = 15.0f;
cameraHeight = 5.0f;
cameraZoomMin = 8.0f;
cameraZoomMax = 30.0f;
cameraFollowSmooth = 60.0f;

// Motion Matching
mmConfig.maxSearchResults = 10;
mmConfig.searchRadius = 2.0f;
mmConfig.blendDuration = 0.1f;

// Physics
physicsSystem.setGravity(glm::vec3(0.0f, -9.81f, 0.0f));
```

---

## 📖 ECS Usage Example

```cpp
#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "ecs/systems/Systems.h"

// Create world
ecs::World world;
world.init();

// Add systems
auto& renderSystem = world.addSystem<ecs::RenderSystem>();
auto& physicsSystem = world.addSystem<ecs::PhysicsSystem>();
auto& cameraSystem = world.addSystem<ecs::CameraSystem>();

// Create entity
auto entity = world.createEntity();

// Add components
auto& transform = world.addComponent<ecs::TransformComponent>(
    entity, 
    glm::vec3(0.0f, 0.0f, 0.0f)
);

auto& mesh = world.addComponent<ecs::MeshComponent>(entity);
mesh.meshID = 0;
mesh.color = glm::vec3(1.0f, 0.0f, 0.0f);

auto& rigidbody = world.addComponent<ecs::RigidBodyComponent>(entity);
rigidbody.bodyType = ecs::RigidBodyType::DYNAMIC;
rigidbody.initialize();

// Update and render
world.update(deltaTime);
world.render();
```

---

## 🐛 Known Issues

### Minor
- Grass instanced rendering not fully integrated
- Some FBX animations need retargeting
- Rock models need proper LOD

### Workarounds
- Press **G** to check foot IK status
- Press **H** to check motion matching state
- Press **F1** to profile performance

---

## 🤝 Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/AmazingFeature`)
3. Write tests for new features
4. Follow ECS architecture patterns
5. Submit a pull request
6. Open issues for bugs or features

### Code Style
- Follow existing ECS patterns
- Use `ecs::` namespace for ECS code
- Keep components data-only (no logic)
- Systems contain all logic
- Document public APIs

---

## 📄 License

MIT License — See LICENSE file for details.

---

## 🙏 Acknowledgments

| Resource | Purpose |
|----------|---------|
| Mixamo | Character animations |
| Assimp | Model loading |
| GLFW | Windowing |
| OpenGL | Graphics API |
| GLM | Mathematics |
| Google Test | Unit testing |
| Research papers | Algorithms & techniques |

---

## 📞 Support

### Common Issues

**Low FPS**  
Press **F1** to see GPU stats, identify bottleneck

**Character floating**  
Press **G** to check foot IK, verify floor height

**NaN errors**  
Check character/terrain initialization order

**Black screen**  
Check shader compilation, verify asset paths

**ECS entity not rendering**  
1. Verify entity has `TransformComponent` + `MeshComponent`
2. Check `mesh.visible = true`
3. Ensure `mesh.meshID >= 0`

### Debug Workflow
1. Press **F1** - Check GPU stats
2. Press **G** - Check foot IK
3. Press **H** - Check motion matching
4. Check console for errors
5. Run tests: `make test`

### ECS Debug Tips
```cpp
// Check entity count
std::cout << "Entities: " << world.getEntityCount() << "\n";

// Check system count
std::cout << "Systems: " << world.getSystemCount() << "\n";

// Check if entity has component
if (world.hasComponent<ecs::TransformComponent>(entity)) {
    // Entity has transform
}

// Get component
auto* transform = world.getComponent<ecs::TransformComponent>(entity);
if (transform) {
    // Use transform
}
```

---

## 📚 Documentation

| Document | Description |
|----------|-------------|
| `README.md` | This file - project overview |
| `ECS_ARCHITECTURE.md` | ECS architecture diagrams |
| `ecs/README.md` | ECS framework documentation |
| `REFACTOR_SUMMARY.md` | Previous refactor notes |

---

<div align="center">

**(RTT-DEV)**

[![GitHub](https://img.shields.io/badge/GitHub-NtokozoMkhwanazi-black?style=for-the-badge&logo=github)](https://github.com/NtokozoMkhwanazi)

*Version 2.0 | C++17 | OpenGL 3.3+ | Linux | ECS Architecture*

*Last Updated: March 2026*

</div>
