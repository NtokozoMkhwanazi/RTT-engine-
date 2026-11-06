# Editor Module

Modular editor components for the RTT Engine. This directory contains all the editor-related code broken into manageable, focused modules.

## Architecture

```
editor/
├── editor_state.h/cpp      - Global editor state and initialization
├── ui.h/cpp                - ImGui UI rendering (panels, menus, toolbars)
├── mesh_builder.h/cpp      - Procedural mesh generation (cube, sphere, etc.)
├── shader_manager.h/cpp    - Shader compilation and management
├── grid_renderer.h/cpp     - Grid rendering for viewport
├── gizmo_renderer.h/cpp    - Transform gizmos (translate, rotate, scale)
├── entity_manager.h/cpp    - Entity creation and operations
├── scene_manager.h/cpp     - Scene save/load serialization
├── console.h/cpp           - Console/logging system
└── README.md               - This file
```

## Module Responsibilities

### editor_state
- Global editor state (ECS world, camera, renderer, etc.)
- Editor initialization and cleanup
- Viewport framebuffer management
- FPS tracking and debug info

### ui
- Menu bar (File, Edit, GameObject, Window, Help)
- Toolbar (entity creation, transform tools)
- Left panel (Outliner, Details, Geospatial)
- Bottom panel (Content, Console, Profiler)
- Viewport panel with camera controls
- Status bar
- About dialog

### mesh_builder
- Procedural mesh generation:
  - Cube (with normals and UVs)
  - Sphere (UV sphere)
  - Plane
  - Cylinder
  - Cone
  - Torus
- Procedural texture generation (checkerboard)
- Mesh cleanup and resource management

### shader_manager
- Main PBR shader (vertex + fragment)
- Gizmo shader (unlit, color-only)
- Shader compilation and linking
- Uniform setting helpers

### grid_renderer
- Grid initialization and rendering
- Axis highlighting (X=red, Z=blue)
- Visibility toggle

### gizmo_renderer
- Transform gizmos:
  - Translate (arrows)
  - Rotate (circles)
  - Scale (cubes)
- Local/World space toggle
- Visibility control

### entity_manager
- Entity creation:
  - Cube, Sphere, Plane, Cylinder, Cone, Torus
  - Light, Camera
- Entity operations:
  - Delete, Duplicate
  - Name management

### scene_manager
- Scene serialization (JSON-like format)
- Save/Load operations
- Current scene file tracking

### console
- Logging system with levels (info, warning, error)
- Message filtering
- Auto-scroll and message limit (1000)

## Usage

### Building

The editor modules compile as part of the main engine build. From the repository root:

```bash
make            # build the test runner
make editor     # build bin/editor_app (ImGui editor application)
make run        # self-check tests, then boot the engine
make run-headless  # bounded headless engine run (CI-friendly)
```

### Entry Points

The repository has two application entry points that drive these editor modules:

- **`src/editor_main.cpp`** → `bin/editor_app` — the ImGui editor application
  (`EditorApplication` main loop, `make editor`).
- **`test.cpp`** → `bin/engine` — the full engine entry point (`make run`): runs the
  complete test suite as a self-check, then boots the engine, enters play mode with a
  motion-matching character and follows it with the third-person camera.

`EditorApplication` (this module) owns the render pipeline, terrain world, input
manager, play-mode controller and follow camera. `PlayModeController` wraps the
`AnimatedCharacter` so play mode is pure logic and unit-testable headlessly.

See the [root README](../README.md) for prerequisites, engine controls, and flags.

## Design Principles

1. **Single Responsibility**: Each module handles one specific aspect of the editor
2. **Clear Interfaces**: Headers define clean APIs with documentation
3. **Resource Management**: Each module manages its own OpenGL resources
4. **No Global Pollution**: Globals are encapsulated in modules or EditorState
5. **Easy to Test**: Modules can be tested independently

## Design History

The editor was refactored from a single monolithic `test.cpp` (~3100 lines) into focused
modules under `editor/*` plus a slim entry point. Today `test.cpp` is the **engine entry
point** (self-check tests → engine boot), and `src/editor_main.cpp` is the ImGui editor
application entry point. Benefits of the modular split:

- Easier to understand and maintain
- Faster compilation (parallel builds)
- Better code reuse
- Easier to unit-test individual features (editor state, undo/redo, entity ops, UI panels)
- Clear separation of concerns

## Future Improvements

- [ ] Add asset browser with file dialog
- [ ] Implement proper entity naming system
- [ ] Add component-based entity creation UI
- [ ] Implement scene graph view
- [ ] Add material editor
