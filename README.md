# 🎮 Custom C++ Game Engine

A lightweight, low-level **C++ game engine** built from scratch with a focus on **full control**, **performance**, and **deep understanding of real-time systems**.
This engine is designed as a **foundation for building real 3D games**, simulations, and interactive applications..

> ⚠️ This is an **active work-in-progress engine**, developed primarily for learning and experimentation.

---

## ✨ Features

### 🧱 Core Systems

* Custom **Entity / Component-style architecture** (engine-managed objects)
* Cross-platform foundation (Linux-first, extensible to Windows)
* Modern **C++17** codebase

### 🎨 Rendering

* OpenGL 3.3+ rendering pipeline
* Shader-based rendering (GLSL)
* Model loading via **Assimp**
* GPU skinning using **bone palette textures**
* Support for skeletal meshes

### 🦴 Animation System

* Skeletal animation playback
* Bone hierarchy & inverse bind pose handling
* GPU skinning (vertex shader driven)
* Multiple animation loading (Mixamo-compatible)
* Root motion extraction
* Animation blending support (WIP)
* Foot locking & IK groundwork (in progress)

### 🧠 Physics

* Custom physics layer
* Rigid bodies
* Gravity handling
* Character controller integration
* Collision groundwork (floor, simple shapes)

### 🎮 Gameplay

* Character controller (movement, jump, grounding)
* Camera system
* Input handling

### 🔊 Audio

* Audio playback via **OpenAL** Disabled for now not needed as yet 

---

## 📁 Project Structure

```text
src/            Core engine source
link/           Engine subsystems (animation, physics, rendering), shaers,
assets/         Models, animations, shaders
third_party/    External libraries
```
---

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
g++ -std=c++17 src/glad.c link/*.cpp test.cpp \
    -o run \
    -lassimp -lopenal -lz -ldl -lglfw -lGL -lX11 -pthread -g
```
## 🚧 Current Status

* ✅ Rendering pipeline working
* ✅ Skeletal meshes load correctly
* ✅ Bone hierarchy & skinning validated
* ⚠️ Animation playback under active development
* ⚠️ Physics/animation synchronization ongoing
* 🚧 Collision system still basic

This engine is **is not ready to build games yet**, but core systems are functional and evolving.

---

## 🎯 Goals & Vision

This engine is built to:

* Provide **full low-level control** over game systems
* Avoid bloated, black-box engines
* Serve as the foundation for:

  * Indie PC games
  * Simulation & training software
  * Custom interactive experiences

The long-term vision is to build to do **gaming and simulation projects** powered by this engine.

## 📌 Roadmap (High Level)

* [ ] Stable animation playback
* [ ] Animation blending
* [ ] Collision resolution
* [ ] Physics-based character grounding
* [ ] Tooling improvements
* [ ] First complete game built on the engine

---
