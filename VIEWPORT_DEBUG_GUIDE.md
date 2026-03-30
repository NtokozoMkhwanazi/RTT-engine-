# Viewport Debugging Guide

## Problem: Viewport Appears Empty/Blank

The viewport in `test.cpp` was not displaying cubes/models due to several rendering issues.

## Fixes Applied

### 1. Shader Update (test.cpp)
- Added support for **instanced rendering** via `layout(location=7) in mat4 instanceMatrix`
- Shader now properly handles both instanced and non-instanced rendering
- Falls back to `model` uniform when instancing is not active

### 2. Renderer.cpp Fixes
- **Changed draw call**: `glDrawElements` → `glDrawArrays` for VAOs without EBO (like our cube)
- **Changed instanced call**: `glDrawElementsInstanced` → `glDrawArraysInstanced`
- **Added model uniform setting** for non-instanced rendering
- **Fixed instance buffer setup** to use `GL_STATIC_DRAW` instead of `GL_DYNAMIC_DRAW`

### 3. RenderSystem.h
- Ensured transforms are properly passed to `Renderer::AddRenderable()`
- Each entity's model matrix is now passed as instance data

### 4. Debug Visual Indicator
- Added a **green rectangle** in the bottom-right corner of the viewport
- This indicates the FBO is rendering correctly
- If you see the green box but no cubes, the issue is with scene rendering (not FBO)

## Debug Test Tools

### Tool 1: Simple Viewport Test
```bash
make viewport-test
./bin/viewport_test
```

**Tests:**
- OpenGL context initialization
- Framebuffer creation and completeness
- Shader compilation
- Cube mesh creation
- Basic rendering (single/multiple cubes)
- Depth testing
- Camera matrices
- Animated rotation (60 frames)

**Output:** Pass/fail for each test with timing information

### Tool 2: Comprehensive Debug Test (Recommended)
```bash
make viewport-debug-test
./bin/viewport_debug_test
```

**Features:**
- Real-time ImGui debug UI
- Live statistics (entities, batches, draw calls, FPS)
- FBO status verification
- Shader compilation status
- VAO validation
- Test mode selection (0-3)
- Screenshot capability (F3)

**Debug UI Panels:**
1. **Viewport Debug Info** - Shows all rendering statistics
2. **Debug Controls** - Test mode selection and options

**Keyboard Controls:**
- **F1** - Toggle debug UI
- **F2** - Re-run all tests
- **F3** - Screenshot FBO (future feature)

### Tool 3: Main Editor (test.cpp)
```bash
make
./bin/test
```

**What to look for:**
1. **Green rectangle** in bottom-right of viewport = FBO is working
2. **3 colored cubes** should appear (red, green, blue) at different positions
3. **ImGui panels** should show entity count = 3

## Diagnostic Checklist

If viewport is still blank, check these in order:

### 1. FBO Activity (Green Box Test)
- Look for green rectangle in bottom-right corner
- ✅ **Visible** = FBO is working correctly
- ❌ **Not visible** = FBO or viewport rendering is broken

### 2. Entity Count
```cpp
// In test.cpp, check console output:
"Total entities: X"  // Should be 3
```
- ✅ **3 entities** = ECS is creating cubes
- ❌ **0 entities** = Entity creation failed

### 3. Renderer Batches
Check in `viewport_debug_test`:
- **Batches Submitted** should be > 0
- **Draw Calls** should be > 0

### 4. Shader Uniforms
In debug UI, check:
- **Uniforms Set** counter increases when rendering
- Shader compiled = OK (green text)

### 5. Camera Position
Default camera is at `(0, 5, 10)` looking at `(0, 0, 0)`
- Cubes are at: `(0,1,0)`, `(2,2,0)`, `(-2,3,0)`
- Use **WASD** to move camera if cubes are off-screen

### 6. OpenGL Errors
Run with verbose output:
```bash
./bin/viewport_debug_test --verbose
```
Look for `[ERROR]` or `[WARN]` messages

## Common Issues and Solutions

### Issue: "Framebuffer Incomplete"
**Solution:** Check OpenGL version and extensions
```bash
glxinfo | grep "OpenGL version"
```
Need OpenGL 3.3+ or OpenGL 4.5 for this engine

### Issue: "Shader Compilation Failed"
**Solution:** Check shader logs in console output
- Vertex shader requires GLSL 330+
- Fragment shader requires GLSL 330+

### Issue: "VAO is 0"
**Solution:** Cube mesh creation failed
- Check `initCube()` is called before rendering
- Verify GLAD is initialized (`gladLoadGLLoader`)

### Issue: "No Draw Calls"
**Solution:** Renderer batches are empty
- Check `RenderSystem::render()` is being called
- Verify entities have both `TransformComponent` and `MeshComponent`
- Check `mesh.visible = true`

### Issue: "Black Screen but Draw Calls > 0"
**Possible causes:**
1. Clear color is black and cubes are off-screen
2. Depth test is failing (try disabling with `glDisable(GL_DEPTH_TEST)`)
3. Face culling is backwards (try `glCullFace(GL_FRONT)`)
4. Camera is inside a cube or looking away

## Performance Metrics

Expected values for 3 cubes:
- **FPS:** 60+ (vsync enabled)
- **Draw Calls:** 3-6 per frame
- **Batches:** 1-3 per frame
- **Entities:** 3

## Architecture Notes

### Rendering Flow
```
test.cpp main loop
  ↓
renderScene()
  ↓
g_viewportFB.bind()  ← Bind FBO
  ↓
g_renderSystem.render()
  ↓
forEach(Transform, Mesh)
  ↓
Renderer::AddRenderable()
  ↓
Renderer::Render()
  ↓
glDrawArraysInstanced()  ← Actual draw call
  ↓
g_viewportFB.unbind()
  ↓
ImGui render FBO texture to screen
```

### Instance Matrix Setup
```
Model Matrix (per entity)
  ↓
Renderer::SetupBatch()
  ↓
Instance Buffer (attr 7-10)
  ↓
Shader: layout(location=7) in mat4 instanceMatrix
  ↓
Vertex Shader uses instanceMatrix for transformation
```

## Next Steps for Debugging

1. **Run `viewport_debug_test`** - Most comprehensive diagnostics
2. **Check console output** - Look for initialization messages
3. **Verify green box** - Confirms FBO is working
4. **Move camera** - Use WASD to ensure cubes aren't off-screen
5. **Check entity count** - Should be 3 in console/ImGui
6. **Profile with GPU profiler** - Check for rendering bottlenecks

## Contact/Support

If issues persist after following this guide:
1. Check console for error messages
2. Run with `--verbose` flag
3. Capture screenshot of debug UI
4. Note OpenGL version from console output
