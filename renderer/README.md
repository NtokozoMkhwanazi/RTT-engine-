# Renderer

Modern OpenGL 4.5 rendering system — batching, instancing, a dual-path
(forward + deferred) PBR pipeline, shadows, screen-space effects, post-processing,
GPU profiling and debug visualization. Rendering runs through the **RHI**
(`rhi/`) so the same scenes render on the Vulkan backend to identical pixels.

## 📁 Files

```
renderer/
├── Renderer.h/cpp          # Main renderer: draw dispatch, batching, FBO/scene state
├── MeshRegistry.h          # Mesh + material registry / instancing registration
├── DeferredRenderer.h      # Deferred (G-buffer) render path
├── ShadowMapper.h          # Directional shadow map FBO + light-space matrix (CSM)
├── SSAO.h                  # Screen-space ambient occlusion (calc + blur)
├── SSR.h                   # Screen-space reflections (calc + composite)
├── PostProcess.h           # Multi-pass post-FX: bloom (extract/blur), ACES tone map, gamma
├── WaterRenderer.h/cpp     # Water rendering
├── GeoTerrainRenderer.h    # GeoTerrain map viewport + 3D terrain rendering
├── GeospatialPointRenderer.h  # Geospatial entity point rendering
├── GeospatialVisualizer.h  # Geospatial trajectory/line visualization
├── TextureAtlas.h/cpp      # Texture atlas packing
├── DefaultTexture.h        # Shared default/fallback textures (white/black/error)
├── EngineUI.h/cpp          # Engine UI overlay (ImGui integration)
├── DebugRenderer.h/cpp     # Debug drawing: grid, gizmos, skeleton, collision, velocity
├── GPUProfiler.h/cpp       # Basic GPU profiler (timestamp queries)
├── GPUProfilerAdvanced.h/cpp # Hierarchical, persistent profiler with CSV export
├── GPU_PROFILER_UI_SUMMARY.md # Profiler UI / data documentation
└── README.md               # This file
```

Shader assets live in `shaderSystem/` and are consumed by these render paths
(PBR, deferred G-buffer, forward+, shadows, SSAO, SSR, sky/physical sky,
terrain splatting, water, post-composite).

## 🎨 Features

### Rendering Pipeline
- **Cook-Torrance PBR** — metallic/roughness shading (GGX distribution, Smith
  visibility, Fresnel-Schlick), with forward and **deferred (G-buffer)** paths
  (`deferred_lighting.frag`).
- **Forward+ / Clustered** — logarithmic-Z tiled light binning (see
  `lighting/ClusteredForward.h`) for many dynamic lights.
- **Shadow Mapping** — directional light shadow maps with CSM cascades
  (`shadow_depth.vert/frag`).
- **Screen-Space Effects** — SSAO (`ssao_calc`/`ssao_blur`) and SSR
  (`ssr_calc`/`ssr_composite`).
- **Post-Processing** — bloom (bright-pass extract + separable blur), ACES
  tone-mapping and gamma 1/2.2 output (`post_*`, `post_composite`).
- **Physical Sky** — analytic Rayleigh + Mie + sun-disk radiance
  (`lighting/PhysicalSky.h`, `procedural_sky`).
- **Terrain & Vegetation** — splat-based terrain rendering, vegetation draws
  and world-object placement (`world/`, `shaderSystem/terrain_splat*`).
- **Water** — reflective/refractive water rendering (`waterVS/FS`).
- **Distance Fog** — exponential-squared fog (disabled by default to preserve
  GL↔Vulkan pixel parity).
- **HDR Textures** — EXR loading path for HDR assets / lighting probes.

### RHI / Backend Integration
- All rendering is issued through the `rhi/` abstraction (`RHI::IRHI`), so the
  **OpenGL** backend drives the full renderer while the **Vulkan** backend
  renders the scene into the swapchain and (in the editor) composites the
  ImGui UI on top — both verified pixel-identical via offscreen readback.

### GPU Profiling
- **Timestamp queries** — hardware-accurate GPU timing.
- **Hierarchical scopes** — nested profiling regions with RAII guards
  (`PROFILE_GPU_SCOPE`).
- **Persistent history** — per-scope avg/min/max/std-dev and frame graphs.
- **CSV export** — offload to external analysis.

### Debug Visualization
- **Grid** — world-space grid overlay.
- **Gizmos** — translate / rotate / scale transform handles (local & world).
- **Skeleton** — bone hierarchy debug display.
- **Collision** — physics shape debug rendering.
- **Trajectory / velocity** — motion-vector overlay.

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

On Vulkan the viewport content is rendered to the swapchain image and the ImGui
UI is drawn on top via `imgui_impl_vulkan` within the same `beginFrame()/endFrame()`
window.

## 📊 Performance

| Feature | Impact | Usage |
|---------|--------|-------|
| **Batching** | Reduces draw calls | Group by material |
| **Instancing** | 10-100x for many objects | Trees, crowds |
| **Deferred** | O(lights × pixels) decoupled | Many dynamic lights |
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

Lighting, fog and sky params are live-tunable with no rebuild via the **CVar**
console — see `lighting/CVar.h`. Overrides persist to `config/cvars.ini`.

## 🛠️ Build & Test

The renderer compiles as part of the main engine build. From the repository root:

```bash
make            # build bin/test_runner + bin/engine (debug)
make editor     # build bin/editor_app (ImGui editor)
make run        # self-check (731 tests), then boot the engine
make run-headless  # bounded headless engine run (CI-friendly)
```

RHI backends (GL/Vulkan offscreen scenes, depth + instancing, swapchain present,
GL↔Vulkan pixel parity, FSR3 EASU, motion vectors), the terrain GPU pipeline
(heightfield sampling, texel mapping, RVT baking), post-processing and viewport
safety are covered by `make test-rhi`, `make test-terrain-pipeline` and the
`Renderer`/`Viewport*`/`PostProcess` gtest suites. See the
[root README](../README.md#test-suites) for the full target list and prerequisites.

---

**Status:** ✅ Production Ready
**Last Updated:** September 2026
