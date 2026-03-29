# Viewport Integration Status

## ✅ Completed

### UI Systems
- Tabbed panel layout (Outliner/Details, Content/Console/Profiler)
- Live statistics in menu bar (FPS, Entities, GPU time)
- Profiler tab with engine diagnostics
- GPU profiler integration
- Proper FBO viewport rendering

### Renderer Integration
- Renderer and RenderSystem added to editor
- Camera matrices passed to renderer
- RenderSystem iterates entities via World::forEach
- Shader program set for rendering

## ⚠️ In Progress / Issues

### Archetype System Integration
The ECS has TWO component storage systems:
1. **ComponentManager** (old) - Used by `getComponent()`
2. **ArchetypeManager** (new) - Used by `getComponentArchetype()` and `createEntityWithComponents()`

**Problem:** Our entities are created with `createEntityWithComponents<>()` which stores them in ArchetypeManager, but then `getComponentArchetype()` returns invalid pointers.

**Root Cause:** The ArchetypeManager's `getComponent()` might not be properly initialized or there's a mismatch between how entities are created and how components are retrieved.

### Current Status
- Entities ARE created (getEntityCount() returns 3)
- Entities ARE in archetypes (forEach finds them)
- BUT getComponentArchetype() returns pointers that crash when dereferenced

## 🔧 Next Steps to Fix

### Option 1: Fix ArchetypeManager
Investigate why `getComponentArchetype()` returns invalid pointers:
- Check if ArchetypeManager::addComponent() properly initializes memory
- Verify entity locations are stored correctly
- Check chunk allocation in ArchetypeData

### Option 2: Use Old Component System
Switch back to `createEntity()` + `addComponent<>()` pattern:
```cpp
auto e = g_world.createEntity();
g_world.addComponent<TransformComponent>(e, ...);
g_world.addComponent<MeshComponent>(e, ...);
```

But this defeats the purpose of archetype-based cache-coherent iteration.

### Option 3: Hybrid Approach
Use `createEntityWithComponents<>()` but access components through the ArchetypeManager's internal storage directly in RenderSystem's forEach, avoiding getComponentArchetype() entirely.

## 📝 Code Changes Made

### Files Modified
1. **test.cpp**
   - Added Renderer, RenderSystem, Model globals
   - Initialize renderer and connect to RenderSystem
   - Set shader program
   - Use `createEntityWithComponents<>()` for archetype storage
   - Use `getComponentArchetype<>()` for component access

2. **ecs/systems/RenderSystem.h**
   - Added `m_world` pointer
   - Added `setWorld()` method
   - Modified `render()` to use `m_world->forEach<>()`
   - Added debug output

3. **ecs/World.h**
   - Added `addSystem(T* system)` overload for existing instances

4. **renderer/Renderer.h**
   - Made `batches` public for debug

## 🎯 Expected Behavior (Once Fixed)

```
./bin/test

=== RTT Engine Editor ===
OpenGL: 4.6 ...
Initializing Renderer...
Initializing GPU Profiler...
[GPUProfiler] Advanced mode initialized
Loading font: ...
Icon font loaded successfully!
Created 3 entities
Ready!

[RenderSystem] Visible: 3, Batches: 3
[RenderSystem] Rendered 3 batches
```

**Viewport should show:**
- Red cube at (0, 1, 0)
- Green cube at (2, 2, 0)  
- Blue cube at (-2, 3, 0)

## 📊 Test Commands

```bash
# Build
cd "/home/run-time-terror/Documents/3D GAME ENGINE"
make clean && make

# Run
./bin/test

# Check output for:
# - "Created 3 entities"
# - "[RenderSystem] Visible: 3, Batches: 3"
# - 3 colored cubes in viewport
```

---

**Status:** ⚠️ Debugging ArchetypeManager component access
**Date:** March 27, 2025
**Next:** Fix getComponentArchetype() or use alternative component access pattern
