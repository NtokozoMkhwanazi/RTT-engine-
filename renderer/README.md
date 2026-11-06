# Renderer

OpenGL 4.5 rendering system with batching, instancing, and GPU profiling.

## 📁 Files

```
renderer/
├── Renderer.h                # Main renderer class
├── Renderer.cpp              # Renderer implementation
├── GPUProfiler.h             # Basic GPU profiler
├── GPUProfiler.cpp           # Profiler implementation
├── GPUProfilerAdvanced.h     # Advanced hierarchical profiler
├── GPUProfilerAdvanced.cpp   # Advanced profiler implementation
├── EngineUI.h                # Engine UI system
├── EngineUI.cpp              # UI implementation
├── DebugRenderer.h           # Debug visualization
├── DebugRenderer.cpp         # Debug rendering
├── TextureAtlas.h            # Texture atlas system
├── TextureAtlas.cpp          # Atlas implementation
├── DefaultTexture.h          # Default textures
├── README.md                 # This file
└── GPU_PROFILER_UI_SUMMARY.md # Profiler documentation
```

## 🎨 Features

### Rendering Pipeline
- **Batch Rendering** - Groups objects by material/shader
- **Instancing** - Efficient rendering of multiple objects
- **Depth Testing** - Proper occlusion handling
- **Face Culling** - Back-face culling for performance
- **Viewport Control** - FBO-based viewport rendering

### GPU Profiling
- **Timestamp Queries** - Hardware-accurate timing
- **Hierarchical Scopes** - Nested profiling regions
- **Persistent Stats** - Frame history and averages
- **CSV Export** - External analysis support

### Debug Visualization
- **Grid Rendering** - World grid overlay
- **Gizmos** - Transform handles (translate/rotate/scale)
- **Bone Visualization** - Skeleton debug display
- **Collision Shapes** - Physics debug rendering

## 📖 Usage

### Basic Rendering

```cpp
// Initialize
Renderer renderer;
renderer.Initialize();

// Set camera
renderer.SetCameraMatrices(view, projection);
renderer.SetViewport(0, 0, width, height);

// Add renderable
renderer.AddRenderable(
    VAO, VBO, EBO,
    vertexCount,
    GL_TRIANGLES,
    shaderProgram,
    transforms  // For instancing
);

// Render all batches
renderer.Render();
```

### GPU Profiling

```cpp
// Initialize
AdvancedGPUProfiler::getInstance().initialize();

// In main loop
BEGIN_GPU_FRAME();

{
    PROFILE_GPU_SCOPE("Render Scene");
    renderScene();
}

{
    PROFILE_GPU_SCOPE("Update Physics");
    physicsWorld.step(dt);
}

END_GPU_FRAME();

// Get stats
float frameTime = AdvancedGPUProfiler::getInstance().getFrameTimeMs();
float fps = AdvancedGPUProfiler::getInstance().getFPS();
```

### Debug Rendering

```cpp
// Render grid
DebugRenderer::renderGrid(cameraPos, size, subdivisions);

// Render gizmo at position
DebugRenderer::renderGizmo(position, gizmoType, space);

// Render skeleton
DebugRenderer::renderSkeleton(skeleton, modelMatrix);
```

## 🎯 Viewport Integration

The renderer is integrated with the editor UI through an FBO (Framebuffer Object):

```cpp
void renderScene() {
    // Bind viewport FBO
    g_viewportFB.bind();
    
    // Set renderer state
    g_renderer.SetViewport(0, 0, width, height);
    g_renderer.SetCameraMatrices(view, proj);
    
    // Render through ECS
    g_renderSystem.render();
    
    // Unbind FBO
    g_viewportFB.unbind();
}
```

## 📊 Performance

| Feature | Impact | Usage |
|---------|--------|-------|
| **Batching** | Reduces draw calls | Group by material |
| **Instancing** | 10-100x for many objects | Trees, crowds |
| **GPU Profiling** | <1% overhead | Always enabled |
| **FBO Rendering** | Single render pass | Editor viewport |

## 🔧 Configuration

### Renderer Settings
```cpp
renderer.SetDepthTesting(true);
renderer.SetFaceCulling(true);
renderer.SetClearColor(0.05f, 0.05f, 0.08f, 1.0f);
```

### Profiler Settings
```cpp
auto& profiler = AdvancedGPUProfiler::getInstance();
profiler.SetHistorySize(120);  // 2 seconds at 60 FPS
profiler.SetExportPath("profile.csv");
```

## 🛠️ Build & Test

The renderer compiles as part of the main engine build. From the repository root:

```bash
make            # build the test runner
make test       # run the full unit-test suite (494 tests)
make run        # self-check tests, then boot the engine
make run-headless  # bounded headless engine run (CI-friendly)
```

See the [root README](../README.md) for prerequisites, build modes, and engine controls.

## 🐛 Known Issues

See [VIEWPORT_STATUS.md](../VIEWPORT_STATUS.md) for current integration status.

---

**Status:** ✅ Production Ready
**Last Updated:** August 2026
