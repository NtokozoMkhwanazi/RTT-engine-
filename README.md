# 🎮 Custom C++ Game Engine

A lightweight, low-level **C++ game engine** built from scratch with a focus on **full control**, **performance**, and **deep understanding of real-time systems**.
This engine is designed as a **foundation for building real 3D games**, simulations, and interactive applications.

> ⚠️ This is an **active work-in-progress engine**, developed primarily for learning and experimentation.
> Screenshot
<img width="1366" height="768" alt="pallete" src="https://github.com/user-attachments/assets/30f8652a-f8b2-44a5-b21f-2b208e6cf45d" />
<img width="1366" height="768" alt="pallete2" src="https://github.com/user-attachments/assets/cec64cad-2acf-4771-bc89-a50595bb45b1" />
<img width="1366" height="768" alt="snipp" src="https://github.com/user-attachments/assets/32d6cdda-3640-4682-bb8c-622e8dd5900c" />
<img width="1366" height="768" alt="snippets" src="https://github.com/user-attachments/assets/1882dea7-07f7-4a4b-8d8b-686fc336265a" />
<img width="1366" height="768" alt="snip" src="https://github.com/user-attachments/assets/bae7d0ae-2bc3-4a7a-8e50-8a3417b57eee" />

---

## ✨ Features

### 🧱 Core Systems

* Custom **Entity / Component-style architecture** (engine-managed objects)
* Cross-platform foundation (Linux-first, extensible to Windows)
* Modern **C++17** codebase
* Advanced memory management with object pooling
* Performance profiling and optimization tools

### 🎨 Rendering

* OpenGL 3.3+ rendering pipeline
* Shader-based rendering (GLSL)
* Model loading via **Assimp**
* GPU skinning using **bone palette textures**
* Support for skeletal meshes
* Batched rendering with instancing
* Advanced lighting system with multiple light types

### 🦴 Animation System

* **Professional-grade skeletal animation playback**
* Bone hierarchy & inverse bind pose handling
* GPU skinning (vertex shader driven)
* **Advanced animation retargeting with Mixamo compatibility**
* **Automatic T-pose to A-pose conversion**
* **Multi-rig detection and bone mapping**
* **Animation blending and layering system**
* **Animation events and callbacks**
* **Foot locking & IK with ground adaptation**
* **Root motion extraction and synchronization**
* **Animation compression and optimization**
* **Advanced blending techniques (linear, additive, directional, masked)**

### 🧠 Physics

* Custom physics layer with **advanced collision detection**
* **Support for multiple collision shapes** (boxes, spheres, capsules)
* Rigid bodies with mass, friction, and restitution
* **Constraint system** (joints, springs, distance constraints)
* Gravity handling
* Character controller integration
* **Raycasting and ground detection**
* **Continuous collision detection (CCD)**
* Collision resolution with penetration correction

### 🎮 Gameplay

* Advanced **character controller** (movement, jump, crouch, slide, sprint)
* **Third-person camera system** with smooth following
* **Camera shake effects** and cinematic modes
* Input handling with action mapping
* **Animation state machine** for locomotion
* **Foot-locking IK** for realistic ground contact

### 🔊 Audio

* Audio playback via **OpenAL** Disabled for now not needed as yet 

---

## 📁 Project Structure

```text
3D GAME ENGINE/
├── animationSystem/          # Advanced animation system
│   ├── Animator.cpp/h        # Animation blending and playback
│   ├── Animation.cpp/h       # Animation data structures
│   ├── AnimationRetargeting.cpp/h # Mixamo compatibility
│   └── AssimpAnimationLoader.cpp/h # Animation loading
├── boneSystem/              # Bone hierarchy and skeleton
├── meshSystem/              # Mesh rendering
├── modelSystem/             # Model loading and management
├── physicsSystem/           # Physics simulation
│   ├── physics.cpp/h        # Core physics
│   └── Constraint.cpp/h     # Physics constraints
├── playerSystem/            # Character controller
│   └── CharacterController.cpp/h
├── cameraSystem/            # Camera management
├── shaderSystem/            # Shaders and materials
├── renderer/                # Rendering system
├── lighting/                # Lighting system
├── memory/                  # Memory management
├── components/              # ECS components
├── systems/                 # ECS systems
├── assets/                  # Game assets
├── include/                 # External headers
├── src/                     # Source files
├── build/                   # Build artifacts
├── bin/                     # Executables
├── Makefile                 # Build system
└── test.cpp                 # Main application
```

## 🛠️ Dependencies

This engine relies on the following libraries:

* **GLFW** – Windowing & input
* **GLAD** – OpenGL loader
* **GLM** – Math library
* **Assimp** – Model & animation import
* **OpenAL** – Audio
* **zlib** – Compression (Assimp dependency)

Make sure all dependencies are installed before building.

---

## ⚙️ Build Instructions (Linux)

```bash
# Clone and build
git clone <repository-url>
cd 3D-GAME-ENGINE

# Build the project
make

# Clean build artifacts
make clean

# Rebuild everything
make rebuild

# Build release version
make release

# Run with release mode
make MODE=release run
```

## 🚧 Current Status

* ✅ **Stable rendering pipeline**
* ✅ **Advanced skeletal animation system** with Mixamo compatibility
* ✅ **Bone hierarchy & GPU skinning validated**
* ✅ **Professional animation retargeting system**
* ✅ **Advanced physics with multiple collision shapes**
* ✅ **Constraint system with joints and springs**
* ✅ **Character controller with advanced movement**
* ⚠️ Animation blending under active refinement
* ⚠️ Advanced collision resolution ongoing
* 🚧 Advanced tooling and editor support

This engine is **ready for serious game development**, with core systems fully functional and production-ready.

---

## 🎯 Goals & Vision

This engine is built to:

* Provide **full low-level control** over game and simulation systems
* Avoid bloated, black-box engines
* Serve as the foundation for:

  * **AAA-quality indie PC games**
  * **Simulation & training software**
  * **Custom interactive experiences**
  * **Animation-heavy applications**

The long-term vision is to build **professional gaming and simulation projects** powered by this engine.

## 📌 Roadmap (High Level)

* [x] Stable animation playback with Mixamo compatibility
* [x] Advanced animation blending and layering
* [x] Multi-shape collision detection
* [x] Constraint system with joints
* [x] Advanced character controller
* [x] Animation retargeting system
* [x] Performance optimization
* [ ] Advanced rendering (shadows, PBR)
* [ ] Audio system integration
* [ ] Complete tooling suite
* [x] First complete demo built on the engine

---

## 🏗️ Architecture Highlights

### Animation System
The animation system now features:
- **Automatic rig detection** for Mixamo, Epic Mannequin, and generic rigs
- **Pose correction** for T-pose to A-pose conversion
- **Enhanced bone mapping** with fuzzy matching
- **Real-time retargeting** between different skeleton structures
- **Advanced blending** with multiple techniques
- **Performance optimizations** with caching and LOD

### Physics System
The physics system includes:
- **Multi-shape collision** (boxes, spheres, capsules)
- **Constraint system** for joints and connections
- **Advanced raycasting** with detailed hit information
- **Continuous collision detection** for fast-moving objects
- **Flexible rigid body** dynamics

### Memory Management
- **Object pooling** for performance-critical objects
- **Memory tracking** and profiling tools
- **Automatic resource** management

---

## 🤝 Contributing

Contributions are welcome! Feel free to fork, submit pull requests, or open issues for bugs and feature requests.

## 📄 License

This project is licensed under the MIT License - see the LICENSE file for details.
