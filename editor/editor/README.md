# Editor Module

Modular editor components for the RTT Engine. All editor code lives under `editor/`
and is driven by two entry points: `src/editor_main.cpp` → `bin/editor_app`
(the ImGui editor application) and `test.cpp` → `bin/engine` (which boots the
editor's `PlayModeController` + third-person camera after the test self-check).

The editor renders through the **RHI**, so the same scene renders on OpenGL
(default) and Vulkan (high quality — editor UI composited over the swapchain
scene) via `--graphics opengl|vulkan`.

## Architecture

```
editor/
├── editor_application.cpp/h  - EditorApplication main loop + lifecycle
├── editor_state.cpp/h        - Global editor state, init/cleanup, viewport FBO, FPS
├── editor_theme.cpp/h        - Dark ImGui theme
├── gl_context_lifecycle.cpp/h - GL context create/destroy (RHI-backed)
├── config.h                  - Editor compile-time/runtime config
├── arena.cpp/h               - Fast arena allocator for editor temporaries
│
├── input_manager.cpp/h       - Input routing (keys, mouse, gizmos)
├── camera_controls.h         - Viewport/camera orbit/pan controls
├── render_pipeline.cpp/h     - Render pipeline orchestration
│
├── ui.cpp/h                  - ImGui UI: menu bar, toolbar, tabbed panels
├── ui_cache.cpp/h            - UI widget caching / drawlist reuse
├── ui_config.h               - Layout + setting persistence (imgui.ini etc.)
├── ui_helpers.cpp/h          - Reusable ImGui helper widgets
├── console.cpp/h             - Logging console (levels, filter, auto-scroll)
├── scene_manager.cpp/h       - Scene JSON save/load
├── scene_panel_config.h      - Panel layout config
│
├── entity_manager.cpp/h      - Create primitives/lights/cameras, dup/delete/rename
├── mesh_builder.cpp/h        - Procedural primitives (cube/sphere/plane/cyl/cone/torus)
├── model_loader.cpp/h        - Assimp model loading
├── shader_manager.cpp/h      - PBR + gizmo shader compile/link + uniforms
├── grid_renderer.cpp/h       - World grid + axis highlight
├── gizmo_renderer.cpp/h      - Translate/Rotate/Scale gizmos (local/world)
├── viewport_framebuffer.cpp/h - Viewport render target (FBO) + readback
├── resource_manager.cpp/h    - Texture/mesh/material resource lifetime
├── undo_redo.cpp/h           - Command stack (create/dup/delete/rename transforms)
├── primitives.cpp/h          - Primitive vertex/index data + default textures
│
├── AnimatedCharacter.h       - Play-mode character wrapper (GL-free, testable)
├── PlayModeController.h      - Play-mode input → animation/motion matching glue
├── playback_recorder.cpp/h   - Demo keyframe camera record/playback
│
├── geo_config_panel.cpp/h    - Geo tracking configuration panel
├── geo_terminal.h            - Geo NMEA command terminal (F8)
├── phosphor_imgui.cpp/h      - Phosphor icon font integration (icon labels)
├── phosphor_icons.h          - Icon codepoint table
└── README.md                 - This file
```

## Features

- **Modular UI** — menu bar (File/Edit/GameObject/Window/Help), toolbar, tabbed
  panels (Outliner / Layers / World / **Geo**), viewport panel and status bar.
- **Graphics backend toggle** — **View → Graphics** switches OpenGL ↔ Vulkan for
  the next launch (persisted to `graphics_api.cfg`); on Vulkan the editor UI
  renders through `imgui_impl_vulkan` over the RHI 3D scene.
- **Geo tracking panel** — live GPS fix, NMEA feeds, Kalman trajectory
  prediction, InfluxDB storage and panel state persistence (F8 to toggle;
  includes a command terminal for NMEA feeds).
- **Transform gizmos** — Translate / Rotate / Scale with local/world space toggle.
- **Click-to-select picking** — Ray-pick entities in the viewport.
- **Entity management** — Create primitives (cube, sphere, plane, cylinder, cone,
  torus), lights and cameras; duplicate, delete, rename — all undoable/redoable.
- **Inspector** — Edit transform, mesh/material and model properties.
- **Scene save/load** — JSON serialization into `scene.json`.
- **Viewport** — FBO-backed viewport with framebuffer readback for selection.
- **Playback recorder** — Keyframe camera path recording and playback.
- **CVar-driven look** — lighting/fog/sky tune live via `lighting/CVar.h`
  (`config/cvars.ini`) without recompiling.
- **Phosphor icon font** — Icon labels throughout the UI.

## Module Responsibilities

### editor_state / editor_application
- Global editor state (ECS world, camera, renderer, input)
- Editor initialization, cleanup and GL lifecycle
- Viewport framebuffer management
- FPS tracking and debug info

### ui / ui_cache / ui_config / ui_helpers / console
- Menu bar (File, Edit, GameObject, Window, Help)
- Toolbar (entity creation, transform tools)
- Left panel (Outliner, Details, Geospatial)
- Bottom panel (Content, Console, Profiler)
- Viewport panel with camera controls
- Status bar, About dialog
- Layout + settings persistence (`imgui.ini`, `engine_ui.ini`)

### mesh_builder / shader_manager / primitives / resource_manager
- Procedural mesh generation: Cube, Sphere (UV), Plane, Cylinder, Cone, Torus
- Procedural checkerboard texture + default textures
- PBR shader (vertex + fragment) + gizmo (unlit, color-only) compile/link
- Uniform-setting helpers and resource lifetime management

### grid_renderer / gizmo_renderer / viewport_framebuffer
- World grid with X=red / Z=blue axis highlight and visibility toggle
- Translate / Rotate / Scale gizmos; local/world space toggle
- Viewport FBO bind/unbind + readback for picking

### entity_manager / model_loader / scene_manager / undo_redo
- Entity creation (primitives, lights, cameras) and operations (delete, duplicate, name)
- Model loading via Assimp
- Scene serialization (JSON)
- Command stack for undo/redo

### input_manager / camera_controls / AnimatedCharacter / PlayModeController / playback_recorder
- Input routing for keys/mouse/gizmo interaction
- Viewport camera orbit/pan controls
- Play-mode character wrapper (GL-free, headless-testable)
- Demo keyframe camera record/playback

### geo_config_panel / geo_terminal / phosphor_imgui / phosphor_icons
- Geo tracking configuration panel (GPS mode, origin)
- NMEA command terminal for the Geo feeds
- Phosphor icon-font integration

## Usage

### Building

The editor modules compile as part of the main engine build. From the repository root:

```bash
make            # build bin/test_runner + bin/engine (debug)
make editor     # build bin/editor_app (ImGui editor application)
make run        # self-check tests (731), then boot the engine
make run-headless  # bounded headless engine run (CI-friendly)
```

### Entry Points

The repository has three application entry points that drive these editor modules:

- **`src/editor_main.cpp`** → `bin/editor_app` — the ImGui editor application
  (`make editor`). `EditorApplication` owns the render pipeline, terrain world,
  input manager, play-mode controller and follow camera.
- **`test.cpp`** → `bin/engine` — the full engine entry point (`make run`): runs
  the complete 731-test self-check, then boots the engine, enters play mode with a
  motion-matching character and follows it with the third-person camera.
- **`tests/test_main.cpp`** → `bin/test_runner` — the Google Test runner
  (`make test`).

`PlayModeController` (this module) wraps the `AnimatedCharacter` so play mode is pure
logic and unit-testable headlessly. See the [root README](../README.md) for
prerequisites, engine controls, and flags.

## Design Principles

1. **Single Responsibility**: Each module handles one specific aspect of the editor
2. **Clear Interfaces**: Headers define clean APIs with documentation
3. **Resource Management**: Each module manages its own GL resources
4. **No Global Pollution**: Globals are encapsulated in modules or EditorState
5. **Easy to Test**: Modules can be tested independently

## Design History

The editor was refactored out of the monolithic `test.cpp` entry point into focused
modules under `editor/*` plus slim entry points. Today `test.cpp` is the **engine
entry point** (~1300 lines: self-check tests → engine boot / play mode), and
`src/editor_main.cpp` is the ImGui editor application entry point (~1900 lines).
Benefits of the modular split:

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

---

**Status:** ✅ Production Ready
**Last Updated:** September 2026
