# 🎮 RTT-Engine — Custom 3D Game Engine

<div align="center">

**A high-performance C++ game engine built from scratch**

[![C++](https://img.shields.io/badge/C%2B%2B-17-blue?style=for-the-badge&logo=c%2B%2B)]()
[![OpenGL](https://img.shields.io/badge/OpenGL-3.3+-green?style=for-the-badge&logo=opengl)]()
[![Platform](https://img.shields.io/badge/Platform-Linux-yellow?style=for-the-badge)]()
[![Tests](https://img.shields.io/badge/Tests-230/235%20passing-success?style=for-the-badge)]()
[![FPS](https://img.shields.io/badge/FPS-60%2B-success?style=for-the-badge)]()

</div>

---

## 📖 Overview

**RTT-Engine** is a professional-grade 3D game engine for building games, simulations, and interactive experiences. Built from scratch in modern C++ with focus on modularity, performance, and deep understanding of graphics programming.

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

![Uploading ChatGPT Image Feb 23, 2026, 08_58_08 PM.png…]()
![Uploading ChatGPT Image Feb 23, 2026, 08_58_08 PM.png…]()
![Uploading ChatGPT Image Feb 23, 2026, 08_58_08 PM.png…]()
![Uploading ChatGPT Image Feb 23, 2026, 08_58_08 PM.png…]()


---

## 🎮 Features

### Core Systems

**Motion Matching**
- SAH-optimized KD-Tree (12 bins)
- 10,980 pose database
- 0.02ms search time
- Continuous pose searching

**Bone Matrix Buffer**
- Auto UBO/SSBO selection
- 9.3x faster bone uploads
- Supports 1000+ bones

**World Rendering**
- Frustum culling (40-60% reduction)
- Distance LOD (4 levels, 50-70% reduction)
- Instanced rendering (10-50x fewer calls)
- Occlusion culling (20-30% reduction)
- Texture atlasing (5-10x fewer binds)

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
- Multiple light types
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
- Foot IK
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
├── world/                      # Terrain, chunks, vegetation
├── animationSystem/            # Animation, bone buffer, foot IK
├── motionMatching/             # Motion matcher, KD-Tree, database
├── boneSystem/                 # Skeleton, bone naming
├── physicsSystem/              # Physics, rigid bodies, constraints
├── modelSystem/                # Model loading, meshes
├── playerSystem/               # Character controller
├── cameraSystem/               # Camera management
├── shaderSystem/               # Shaders, skybox
├── renderer/                   # Renderer, texture atlas, GPU profiler
├── lighting/                   # Light management
├── memory/                     # Arenas, asset manager
├── components/                 # ECS components
├── tests/                      # Unit tests (235 total)
├── assets/                     # FBX models, animations, skybox
├── include/                    # GLAD, GLM, KHR
├── Makefile                    # Build configuration
├── test.cpp                    # Main entry point
└── bin/                        # Executables
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
make release

# Run tests
make test

# Run engine
./bin/run
```

### Build Status
```
✅ Compilation: SUCCESS
✅ Tests: 230/235 PASSING (98%)
```

---

## 🧪 Testing

```
Total Tests: 235
Passed: 230 (98%)
Disabled: 5 (2%)
Failed: 0 (0%)
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
| Memory Management | 25 | ✅ 22 passing |
| Motion Matching | 14 | ✅ 12 passing |
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

---

## ✅ Implementation Status

### Complete
- Procedural terrain generation
- Chunk streaming & LOD
- Frustum & occlusion culling
- Animated water system
- Vegetation instanced rendering
- Skeletal animation
- Motion matching system
- Bone matrix buffer (UBO/SSBO)
- Foot IK with terrain tracking
- Physics collision detection
- Rigid body dynamics
- Character controller
- Camera system
- Texture atlasing
- GPU profiling
- Memory arenas & pools
- Asset manager
- Unit tests (235 tests)
- NaN prevention

### In Progress
- Grass instanced rendering
- Rock model rendering
- Texture splatting
- Water collision
- Shadows
- Full audio integration

### Planned
- Infinite terrain streaming
- Day/night cycle
- Weather system
- Advanced lighting
- Particle system
- UI system
- Scripting support

---

## 🔧 Configuration

Edit in `test.cpp`:

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
```

---

## 📚 Documentation

1. `ENGINE_STATUS_REPORT.md` - Complete engine status
2. `WORLD_OPTIMIZATIONS_COMPLETE.md` - World optimizations
3. `BONE_MATRIX_BUFFER_UBO_SSBO.md` - Bone buffer system
4. `KDTREE_SAH_OPTIMIZATION.md` - SAH KD-Tree
5. `MEMORY_MANAGEMENT_COMPLETE.md` - Memory systems
6. `FOOT_IK_CHARACTER_GROUNDING_FIX.md` - Foot IK
7. `DEBUG_FLOOR_VISUALIZATION.md` - Debug floor
8. `MOTION_ANIMATOR_BUG_REPORT.md` - Motion matching fixes

Total: 500+ pages

---

## 🐛 Known Issues

### Minor
- Grass instanced rendering not fully integrated
- Some FBX animations need retargeting
- Rock models need proper LOD

### Workarounds
- Press G to check foot IK status
- Press H to check motion matching state
- Press F1 to profile performance

---

## 🤝 Contributing

1. Fork the repository
2. Create a feature branch
3. Write tests for new features
4. Submit a pull request
5. Open issues for bugs or features

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

---

## 📞 Support

### Common Issues

**Low FPS**  
Press F1 to see GPU stats, identify bottleneck

**Character floating**  
Press G to check foot IK, verify floor height

**NaN errors**  
Check character/terrain initialization order

**Black screen**  
Check shader compilation, verify asset paths

### Debug Workflow
1. Press F1 - Check GPU stats
2. Press G - Check foot IK
3. Press H - Check motion matching
4. Check console for errors
5. Run tests: `make test`

---

<div align="center">

**Built by Ntokozo (RTT-DEV)**

[![GitHub](https://img.shields.io/badge/GitHub-NtokozoMkhwanazi-black?style=for-the-badge&logo=github)](https://github.com/NtokozoMkhwanazi)

*Version 2.0 | C++17 | OpenGL 3.3+ | Linux*

*Last Updated: February 2026*

</div>
