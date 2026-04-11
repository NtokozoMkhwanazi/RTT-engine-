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

The editor modules are automatically included in the main build:

```bash
make clean
make
./bin/test
```

### Using the Modular Version

To use the modular version instead of the original test.cpp:

```bash
# Backup original
cp test.cpp test_original.cpp

# Use modular version
cp test_modular.cpp test.cpp
make clean
make
./bin/test
```

## Design Principles

1. **Single Responsibility**: Each module handles one specific aspect of the editor
2. **Clear Interfaces**: Headers define clean APIs with documentation
3. **Resource Management**: Each module manages its own OpenGL resources
4. **No Global Pollution**: Globals are encapsulated in modules or EditorState
5. **Easy to Test**: Modules can be tested independently

## Migration from Original test.cpp

The original `test.cpp` (3101 lines) has been refactored into:

- **test_modular.cpp** (~500 lines) - Main entry point, just wires everything together
- **editor/*.cpp** (~2500 lines total) - All the functionality in modular form

Benefits:
- Easier to understand and maintain
- Faster compilation (parallel builds)
- Better code reuse
- Easier to test individual features
- Clear separation of concerns

## Future Improvements

- [ ] Add proper JSON library for scene serialization
- [ ] Implement undo/redo system
- [ ] Add asset browser with file dialog
- [ ] Implement proper entity naming system
- [ ] Add component-based entity creation UI
- [ ] Implement scene graph view
- [ ] Add material editor
- [ ] Implement animation preview
- [ ] Add physics debug visualization
