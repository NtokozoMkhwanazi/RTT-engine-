# 🎮 RTT-Engine — Custom 3D Game Engine

<div align="center">

**A modular, low-level C++ game engine built from scratch**

[![C++](https://img.shields.io/badge/C%2B%2B-17-blue?style=for-the-badge&logo=c%2B%2B)]()
[![OpenGL](https://img.shields.io/badge/OpenGL-3.3+-green?style=for-the-badge&logo=opengl)]()
[![Platform](https://img.shields.io/badge/Platform-Linux-yellow?style=for-the-badge)]()
[![Status](https://img.shields.io/badge/Status-Alpha-orange?style=for-the-badge)]()

</div>

---

## 📖 Overview

**RTT-Engine** is a **general-purpose 3D game engine** designed for building games, simulations, and interactive experiences. Built entirely from scratch in modern C++, the engine emphasizes **modularity**, **performance**, and **deep understanding** of graphics programming and engine architecture.

> 🔧 **This is an engine, not a game.** It provides the foundation and systems you need to build your own projects — from open-world games to physics simulations to architectural visualizations.

---
![Uploading screen2.png…]()


## 🎯 Design Philosophy

| Principle | Description |
|-----------|-------------|
| **Modularity** | Every system is independent and replaceable. Swap components without breaking the engine. |
| **Low-Level Control** | No black boxes. You control memory, rendering, physics — everything. |
| **Performance First** | Optimized for real-time rendering and simulation. Every millisecond counts. |
| **Educational** | Built to understand how game engines work under the hood. |
| **Extensible** | Add new systems, components, and features without rewriting core code. |

---

## 🏗️ Engine Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        GAME / SIMULATION                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │   Renderer   │  │   Physics    │  │  Animation   │          │
│  └──────────────┘  └──────────────┘  └──────────────┘          │
│                                                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │    World     │  │    Audio     │  │    Input     │          │
│  │   Streaming  │  │   System     │  │   Manager    │          │
│  └──────────────┘  └──────────────┘  └──────────────┘          │
│                                                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │    Entity    │  │   Memory     │  │    Shader    │          │
│  │   Component  │  │   Manager    │  │   Manager    │          │
│  └──────────────┘  └──────────────┘  └──────────────┘          │
│                                                                  │
├─────────────────────────────────────────────────────────────────┤
│                    OpenGL 3.3+ | GLSL | GLFW                     │
└─────────────────────────────────────────────────────────────────┘
```

---

## ⚙️ Core Systems

### 🎨 Rendering System
- **OpenGL 3.3+** pipeline with custom shader management
- **GPU Skinning** for skeletal animation
- **Batch rendering** with instancing support
- **Multiple light types** (directional, point, spot)
- **Normal mapping** and PBR-ready materials
- **Skybox** with cube mapping
- **Atmospheric fog** with exponential distance

### 🌍 World Streaming System
- **Procedural terrain** generation (Perlin noise)
- **Chunk-based LOD** streaming (4 levels)
- **800m × 800m** explorable areas (expandable)
- **Height-based biomes** (Sand → Grass → Rock → Snow)
- **Water system** with animated waves and Fresnel effects
- **Vegetation placement** (trees, rocks, grass)
- **Distance culling** and occlusion

### 🦴 Animation System
- **Skeletal animation** with GPU skinning
- **Animation blending** and layering
- **Root motion** extraction
- **Foot IK** and inverse kinematics
- **State machines** for character states
- **Mixamo compatibility** with auto-retargeting
- **FBX import** via Assimp

### 🧠 Physics System
- **Collision detection** (boxes, spheres, capsules)
- **Rigid body dynamics** with mass, friction, restitution
- **Continuous collision detection** (CCD)
- **Raycasting** with hit information
- **Constraints** (hinge, slider, springs)
- **Character controller** with grounded detection
- **Time of Impact** (TOI) calculation

### 🎮 Character Controller
- **Third-person** movement with root motion
- **WASD** locomotion
- **Jump, Sprint, Crouch** mechanics
- **Smooth rotation** (configurable speed)
- **Camera modes**: Fixed, Orbit, Follow
- **Smooth zoom** (8-30m range)

### 🔊 Audio System
- **OpenAL** integration (ready for 3D spatial audio)
- **Sound effect** playback
- **Positional audio** support

### 🧱 Entity Component System
- **Component-based** architecture
- **Entity management** with custom ECS-style design
- **Memory pooling** for performance
- **System updates** with delta time

### 💾 Memory Management
- **Custom memory tracker** for debugging
- **Object pooling** to reduce allocations
- **Memory profiling** tools

---

## 📁 Project Structure

```
3D GAME ENGINE/
├── world/                      # World streaming & terrain
│   ├── Terrain.h/cpp           # Procedural terrain generation
│   ├── TerrainChunk.h/cpp      # Chunk management & LOD
│   ├── VegetationSystem.h/cpp  # Tree/rock/grass placement
│   └── *.glsl                  # Terrain, water, vegetation shaders
│
├── animationSystem/            # Character animation
│   ├── Animator.h/cpp          # Animation playback & blending
│   ├── Animation.h/cpp         # Animation data structures
│   ├── AnimationStateMachine   # State machine logic
│   ├── AnimationRetargeting    # Mixamo compatibility
│   └── AssimpAnimationLoader   # FBX animation loading
│
├── boneSystem/                 # Skeleton system
│   ├── Skeleton.h              # Bone hierarchy
│   ├── BoneName.h              # Bone naming utilities
│   └── DebugSkeleton.h/cpp     # Bone visualization
│
├── physicsSystem/              # Physics simulation
│   ├── Physics.h/cpp           # Physics core
│   ├── RigidBody.h             # Rigid body component
│   ├── Constraint.h/cpp        # Joints & constraints
│   ├── TOI.h                   # Time of impact
│   └── AdvancedConstraints     # Complex constraint types
│
├── modelSystem/                # 3D model loading
│   ├── Model.h/cpp             # Model management
│   └── meshSystem/             # Mesh data structures
│
├── playerSystem/               # Character controller
│   ├── CharacterController     # Player movement & logic
│   └── Player.h                # Player component
│
├── cameraSystem/               # Camera management
│   ├── Camera.h                # Camera interface
│   └── flyCamera.h             # Free-fly camera
│
├── shaderSystem/               # Shader management
│   ├── Shader.h/cpp            # Shader wrapper
│   ├── Skybox.h/cpp            # Skybox rendering
│   └── *.glsl                  # All shader programs
│
├── renderer/                   # Core rendering
│   ├── Renderer.h/cpp          # Main renderer
│
├── lighting/                   # Lighting system
│   ├── LightingSystem.h/cpp    # Light management
│
├── memory/                     # Memory management
│   ├── MemoryManager.h/cpp     # Tracking & pooling
│
├── components/                 # ECS components
│   ├── Component.h             # Base component
│   ├── Entity.h                # Entity type
│   ├── ColliderComponent.h     # Collision component
│   └── ColliderType.h          # Collider definitions
│
├── assets/                     # Game assets
│   ├── *.fbx                   # 3D models & animations
│   └── skybox/                 # Skybox textures
│
├── include/                    # External libraries
│   ├── glad/                   # OpenGL loader
│   └── KHR/                    # Khronos headers
│
├── Makefile                    # Build configuration
├── test.cpp                    # Main application entry
└── bin/                        # Compiled executables
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
| **C** | Toggle camera mode (Fixed / Orbit) |
| **Mouse** | Orbit camera (in orbit mode) |
| **Scroll Wheel** | Zoom camera (8-30m) |
| **Q / E** | Camera pivot up / down |
| **R** | Reset camera position |
| **F** | Toggle wireframe / solid |
| **B** | Cycle bone debug modes |
| **G** | Print foot IK status |
| **H** | Print animation state |
| **ESC** | Exit application |

---

## 🛠️ Dependencies

| Library | Purpose | Installation (Ubuntu/Debian) |
|---------|---------|------------------------------|
| **GLFW** | Windowing & Input | `sudo apt install libglfw3-dev` |
| **GLAD** | OpenGL Loader | Included in `include/` |
| **GLM** | Mathematics | `sudo apt install libglm-dev` |
| **Assimp** | Model Import | `sudo apt install libassimp-dev` |
| **OpenAL** | Audio | `sudo apt install libopenal-dev` |
| **zlib** | Compression | `sudo apt install zlib1g-dev` |

---

## ⚙️ Building

```bash
# Clone the repository
git clone <repository-url>
cd 3D-GAME-ENGINE

# Debug build (default)
make

# Release build (optimized)
make release

# Clean build artifacts
make clean

# Full rebuild
make rebuild

# Run the engine
./bin/run
```

### Build Modes

| Mode | Flags | Use Case |
|------|-------|----------|
| **debug** | `-g -O0 -DDEBUG` | Development & debugging |
| **release** | `-O2 -DNDEBUG` | Performance testing & deployment |

---

## 📊 Performance Metrics

| Metric | Value |
|--------|-------|
| **World Size** | 800m × 800m |
| **Active Chunks** | 50-70 |
| **Total Vertices** | ~250,000 (with LOD) |
| **Water Plane** | 2,601 vertices |
| **Max Trees/Chunk** | 80 |
| **Max Rocks/Chunk** | 50 |
| **Memory (Terrain)** | ~5MB |
| **Frame Time** | ~8-12ms (80-120 FPS) |

---

## 🚀 What You Can Build

This engine is designed to support:

### 🎮 Games
- Open-world exploration games
- Third-person action games
- Platformers
- Adventure games
- Survival games

### 🧪 Simulations
- Physics simulations
- Architectural walkthroughs
- Training simulators
- Scientific visualizations
- Virtual environments

### 🛠️ Tools
- Level editors
- Terrain generators
- Animation viewers
- Model viewers
- Prototyping tools

---

## ✅ Implementation Status

### Fully Functional
- [x] Procedural terrain generation
- [x] Chunk streaming & LOD
- [x] Animated water system
- [x] Vegetation placement
- [x] Skeletal animation
- [x] Animation blending & IK
- [x] Root motion extraction
- [x] Physics collision detection
- [x] Rigid body dynamics
- [x] Character controller
- [x] Camera system (multiple modes)
- [x] Skybox rendering
- [x] Atmospheric fog
- [x] Shader management

### In Progress
- [ ] Grass instanced rendering
- [ ] Rock model rendering
- [ ] Texture splatting
- [ ] Water collision
- [ ] Shadows
- [ ] Full audio integration

### Planned
- [ ] Infinite terrain streaming
- [ ] Day/night cycle
- [ ] Weather system
- [ ] Wildlife AI
- [ ] Advanced lighting (shadows, GI)
- [ ] Particle system
- [ ] UI system
- [ ] Scripting support

---

## 📖 Documentation

| Document | Description |
|----------|-------------|
| **[FEATURES_COMPLETE.md](FEATURES_COMPLETE.md)** | Complete feature list |
| **[OPEN_WORLD_GUIDE.md](OPEN_WORLD_GUIDE.md)** | World streaming technical guide |
| **[TERRAIN_SYSTEM.md](TERRAIN_SYSTEM.md)** | Terrain implementation details |
| **[WHATS_NEW.md](WHATS_NEW.md)** | Latest updates |
| **[ENHANCED_ENGINE_SUMMARY.md](ENHANCED_ENGINE_SUMMARY.md)** | Engine enhancements |
| **[MESH_SYSTEM_IMPROVEMENTS.md](MESH_SYSTEM_IMPROVEMENTS.md)** | Mesh system updates |
| **[MODEL_SYSTEM_IMPROVEMENTS.md](MODEL_SYSTEM_IMPROVEMENTS.md)** | Model system updates |
| **[IMPORT_MODELS_GUIDE.md](IMPORT_MODELS_GUIDE.md)** | Model import guide |

---

## 🔧 Configuration

Edit in `test.cpp`:

```cpp
// Terrain Configuration
terrainConfig.chunkSize = 100.0f;      // Chunk size in meters
terrainConfig.viewDistance = 4;        // Chunks loaded per direction
terrainConfig.heightScale = 80.0f;     // Maximum terrain height

// Vegetation Configuration
vegConfig.treeDensity = 0.03f;         // Trees per square meter
vegConfig.maxTreesPerChunk = 80;       // Maximum trees per chunk
vegConfig.rockDensity = 0.02f;         // Rocks per square meter

// Water Configuration
float waterLevel = 5.0f;               // Water height

// Camera Configuration
cameraDistance = 15.0f;                // Default camera distance
cameraHeight = 5.0f;                   // Camera height offset
cameraZoomMin = 8.0f;                  // Minimum zoom
cameraZoomMax = 30.0f;                 // Maximum zoom
cameraFollowSmooth = 3.0f;             // Follow smoothing factor
```

---

## 🤝 Contributing

Contributions are welcome!

- **Fork** the repository
- **Create** a feature branch
- **Submit** a pull request
- **Open** issues for bugs or feature requests

---

## 📄 License

MIT License — See [LICENSE](LICENSE) file for details.

---

## 🙏 Acknowledgments

| Resource | Purpose |
|----------|---------|
| **Mixamo** | Character animations |
| **Assimp** | Model loading library |
| **GLFW** | Cross-platform windowing |
| **OpenGL** | Graphics API |
| **GLM** | Mathematics library |

---

<div align="center">

**Built with ❤️ by Ntokozo (RTT-DEV)**

[![GitHub](https://img.shields.io/badge/GitHub-NtokozoMkhwanazi-black?style=for-the-badge&logo=github)](https://github.com/NtokozoMkhwanazi)

*Version 0.4.0-alpha | C++17 | OpenGL 3.3+ | Linux*

</div>
