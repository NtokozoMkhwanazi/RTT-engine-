# RTT Engine Editor - Robust Unreal Engine Style UI

## Overview
Professional game engine editor with improved robustness and stability:
- ✅ No static buffers that outlive ECS data
- ✅ Proper resource cleanup with RAII
- ✅ Safe entity selection handling
- ✅ Window resize handling
- ✅ Input validation
- ✅ Framebuffer completeness checks

## ⚠️ Icon Display

If you see squares/boxes instead of icons in menus, install a Nerd Font:

```bash
# Ubuntu/Debian
sudo apt install fonts-font-awesome

# Or download from:
# https://www.nerdfonts.com/font-downloads
```

After installing, restart the editor and icons should display properly.

## Layout

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  File  Edit  GameObject  Window  Help                          [FPS: 60]   │
├─────────────────────────────────────────────────────────────────────────────┤
│  Transform: [Translate] [Rotate] [Scale] | Space: [World]                  │
│  View: [Grid] [Gizmo] | Play: [PLAY] [PAUSE] [STOP]                        │
├──────────────┬──────────────────────────────────────────────┬───────────────┤
│              │                                              │               │
│  WORLD       │              Viewport                        │   DETAILS     │
│  OUTLINER    │              (3D Scene)                      │               │
│  ─────────   │                                              │   ─────────   │
│  Search...   │              ┌─────────┐                     │   Transform   │
│              │              │         │                     │   Position    │
│  □ Cube      │              │  3D     │                     │   Rotation    │
│  □ Sphere    │              │  View   │                     │   Scale       │
│  □ Light     │              │         │                     │               │
│              │              └─────────┘                     │   Mesh        │
│              │                                              │   Albedo      │
│              │                                              │   Visible ✓   │
│              │                                              │               │
├──────────────┴──────────────────────────────────────────────┴───────────────┤
│  CONTENT BROWSER                                                            │
│  ─────────────────                                                          │
│  Path: [/Game/Assets] [Refresh] [Import]                                    │
│  ─────────────────────────────────────────────────────────────────────────  │
│  MESHES                                                                     │
│  ──────                                                                     │
│  [Cube] [Sphere] [Plane] [Cylinder] [Cone] [Torus]                          │
│                                                                             │
│  MATERIALS                                                                  │
│  ─────────                                                                  │
│  [M_Default] [M_Metal] [M_Wood] [M_Stone] [M_Glass]                         │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Key Robustness Improvements

### 1. Resource Management
```cpp
struct Framebuffer {
    void init(int w, int h);    // Proper allocation
    void cleanup();              // Proper deallocation
    void resize(int w, int h);   // Safe resize with bounds checking
};
```

### 2. Entity Safety
- Check `entity.isValid()` before accessing
- Check component pointers for null
- Handle destroyed entities gracefully

### 3. Input Validation
- Bounds checking on all drag sliders
- Null-terminated string buffers
- Safe array indexing

### 4. Cleanup Order
```cpp
// Shutdown in correct order
ImGui_ImplOpenGL3_Shutdown();
ImGui_ImplGlfw_Shutdown();
ImGui::DestroyContext();
g_world.shutdown();
cleanupCube();
g_viewportFB.cleanup();
delete g_camera;
glfwTerminate();
```

## Features

### Menu Bar
| Menu | Options |
|------|---------|
| **File** | New Scene, Open Scene, Save, Save As, Exit |
| **Edit** | Undo, Redo, Preferences |
| **GameObject** | Create Cube, Sphere, Plane, Light, Camera |
| **Window** | Toggle panel visibility |
| **Help** | Documentation, About |

### Toolbar

**Transform Tools:**
- **Translate** (W) - Move objects
- **Rotate** (E) - Rotate objects  
- **Scale** (R) - Scale objects

**Space Toggle:**
- **World** - Transform in global space
- **Local** - Transform in object's local space

**View Options:**
- **Grid** - Show/hide grid
- **Gizmo** - Show/hide transform gizmo

**Play Controls:**
- **PLAY** - Start game
- **PAUSE** - Pause game
- **STOP** - Stop game

### World Outliner (Left Panel)
- Entity hierarchy with search
- Click to select entity
- Selected entities highlighted in orange
- Shows "No entities in scene" when empty

### Viewport (Center)
- Real-time 3D scene preview
- FBO-based rendering with completeness check
- **Right-click + drag** - Look around
- **WASD** - Move camera
- Automatic FBO resize on window change

### Details Panel (Right)
**Entity Name:** Editable text field with null-termination safety

**Transform:**
- Position (X, Y, Z) with Reset button
- Rotation (X, Y, Z degrees) with Reset button
- Scale (X, Y, Z) with Reset button
- Bounds checking on all values

**Mesh:**
- Albedo color picker
- Visible checkbox

### Content Browser (Bottom)
**Path Bar:**
- Current folder path
- Refresh button
- Import button

**Meshes Section:**
- Cube, Sphere, Plane, Cylinder, Cone, Torus
- Dynamic grid layout based on window width

**Materials Section:**
- M_Default, M_Metal, M_Wood, M_Stone, M_Glass
- Visual boxes for each asset

## Color Scheme

| Element | Color | Purpose |
|---------|-------|---------|
| Window Background | Dark gray (0.10) | Main panels |
| Header | Dark gray (0.20) | Collapsible sections |
| Header Hovered | Medium gray (0.30) | Hover states |
| Button | Dark gray (0.20) | Interactive elements |
| Accent (Orange) | (0.60, 0.40, 0.00) | Selections, active states |
| Panel Headers | Orange (1.0, 0.85, 0.2) | Section titles |
| Meshes Label | Blue (0.4, 0.7, 1.0) | Mesh category |
| Materials Label | Orange (1.0, 0.65, 0.4) | Material category |

## Controls

### Viewport Navigation
| Input | Action |
|-------|--------|
| Right Mouse Button (Hold) + Drag | Look around |
| W | Move forward |
| A | Move left |
| S | Move backward |
| D | Move right |

### Entity Selection
| Input | Action |
|-------|--------|
| Left Click (World Outliner) | Select entity |
| Delete | Delete selected entity |

### Transform Tools
| Key | Action |
|-----|--------|
| W | Select Translate |
| E | Select Rotate |
| R | Select Scale |
| X | Toggle World/Local Space |

## Running the Editor

```bash
cd "/home/run-time-terror/Documents/3D GAME ENGINE"
make clean && make
./bin/test
```

## Technical Details

### FBO Rendering
```cpp
struct Framebuffer {
    GLuint fbo, colorTex, rbo;
    int width, height;
    
    void init(int w, int h) {
        // Creates FBO, color texture, depth/stencil RBO
        // Checks framebuffer completeness
    }
    
    void resize(int w, int h) {
        // Bounds checking (w > 0, h > 0)
        // Skip if size unchanged
        // Proper cleanup before reallocation
    }
};
```

### Safe Component Access
```cpp
// Always check entity validity
if (selectedEntity.isValid()) {
    t = g_world.getComponent<TransformComponent>(selectedEntity);
    m = g_world.getComponent<MeshComponent>(selectedEntity);
    n = g_world.getComponent<NameComponent>(selectedEntity);
    entityStillExists = (t || m || n);
}

// Only render if entity exists
if (t && entityStillExists) renderTransformSection(t);
```

### String Buffer Safety
```cpp
// Always null-terminate
strncpy(nameBuffer, n->name.c_str(), sizeof(nameBuffer) - 1);
nameBuffer[sizeof(nameBuffer) - 1] = '\0';

// Use sizeof() not hardcoded values
ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer), ...);
```

### Panel Layout
Fixed coordinate-based positioning:
- Menu Bar: 25px height
- Toolbar: 36px height
- World Outliner: 250px width
- Details: 300px width
- Content Browser: 220px height
- Viewport: Remaining center space (auto-calculated)

## Troubleshooting

### Icons show as squares
Install a Nerd Font (see top of document).

### Viewport not showing
- Check console for "Framebuffer incomplete" error
- Verify OpenGL 4.5 context in console output
- Check FBO texture is bound in ImGui::Image()

### UI not responding
- Ensure `io.WantCaptureMouse` is respected
- Check ImGui initialization completed

### Black viewport
- Verify shaders compiled (check console)
- Check entities exist in World Outliner
- Verify camera position is valid

### Crash on entity deletion
- Fixed: Now checks `entity.isValid()` before access
- Fixed: Component pointers checked for null
- Fixed: Entity existence verified before rendering UI

### Memory leak warnings
- Fixed: RAII-style cleanup in main()
- Fixed: Framebuffer::cleanup() properly deletes GPU resources
- Fixed: ImGui shutdown in correct order

## Code Quality

### Compiler Warnings
- Zero warnings with `-Wall -Wextra`
- Proper type casting (static_cast)
- Unused variable elimination

### Error Handling
- GLFW initialization checks
- GLAD initialization checks
- Framebuffer completeness checks
- Entity validity checks

### Performance
- vsync disabled for max FPS (glfwSwapInterval(0))
- FBO only resized when dimensions change
- Efficient ImGui rendering

---

**Status:** ✅ Complete - Robust UI with proper resource management and error handling

**Last Updated:** March 2026
