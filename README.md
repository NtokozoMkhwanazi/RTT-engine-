



# RTT Engine - 3D Game Engine

![GitHub stars](https://img.shields.io/github/stars/NtokozoMkhwanazi/RTT-engine-?style=social)
![GitHub forks](https://img.shields.io/github/forks/NtokozoMkhwanazi/RTT-engine-?style=social)
![GitHub issues](https://img.shields.io/github/issues/NtokozoMkhwanazi/RTT-engine-)
![GitHub license](https://img.shields.io/github/license/NtokozoMkhwanazi/RTT-engine-)
![C++](https://img.shields.io/badge/C++17-ISO-blue)
![OpenGL](https://img.shields.io/badge/OpenGL-4.5-orange)

**Advanced lightweight** 3D game engine project built for **scalability and high-performance systems** for animation systems , 3D software simulation systems and real-time rendering using the **power and speed** of the C++ Language. openGL graphics API for relative ease of use compared to Vulkan. Dear imgui UI/UX editor for its seemless integration with th C++ language and backend API openGL . This project contains comprehensive documentation and a clear codebase structure. This project is **actively under-development and maintained**. This is a project that encourages learn by building, meant for curious minds.  

## 📊 Architecture diagrams (ECS Overview)

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

## 📸 Screenshots

**Dear imGui UI editor**
<img width="1366" height="768" alt="uui2" src="https://github.com/user-attachments/assets/f1a4dd17-d816-4132-953f-731588fec1d4" />
<img width="1366" height="768" alt="uui" src="https://github.com/user-attachments/assets/cb065b90-c4a0-4c64-8faa-54824de23281" />
https://github.com/user-attachments/assets/ccc8782c-8bc2-4a55-a71d-cd9c1a92965d
https://github.com/user-attachments/assets/eaadf42e-e399-4e20-a975-9c7c83d3738b


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

### Expected Display (debug purposes for viewport specifically ) 
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

