# Performance Optimization Plan

## Critical Bottlenecks Found

### 1. SimpleWorldRenderer - MAJOR (1000x slowdown)
**File:** `world/SimpleWorldRenderer.cpp`

**Problems:**
```cpp
// ❌ Logging EVERY frame in render loop!
std::cout << "[SimpleWorldRenderer] Attempting to render " << m_instances.size() << " instances\n";
std::cout << "[SimpleWorldRenderer] First instance at (" << ... << ")\n";
std::cout << "[SimpleWorldRenderer] Rendered " << rendered << " meshes...\n";

// ❌ Setting uniforms for EVERY instance (O(n) shader calls)
for (size_t i = 0; i < m_instances.size(); i++) {
    simpleShader->setMat4("model", inst.modelMatrix);  // BAD!
    mesh.Draw(*simpleShader);  // Separate draw call per mesh!
}
```

**Impact:**
- 50-100 instances = 50-100 shader binds + 50-100 uniform sets + 50-100 draw calls
- Console I/O is EXTREMELY slow (blocks rendering)
- Should be: 1 shader bind + 1 VAO bind + 1 instanced draw call

**Fix Priority:** 🔴 CRITICAL

---

### 2. Animator Bone Matrix Upload - MAJOR
**File:** `test.cpp` (main loop)

**Current:**
```cpp
// Every frame: Upload 65+ bone matrices to texture
glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, boneCount, 1, GL_RGBA, GL_FLOAT, pixels.data());
```

**Problem:**
- 65 bones × 4 matrices = 260 vec4 uploads per frame
- Texture upload is slow (CPU → GPU transfer)
- Should use uniform buffer object (UBO) or shader storage buffer (SSBO)

**Fix Priority:** 🟡 HIGH

---

### 3. Terrain Chunk Rendering - MEDIUM
**File:** `world/Terrain.cpp`, `world/TerrainChunk.cpp`

**Current:**
- 246 active chunks (from logs)
- Each chunk renders separately
- No frustum culling
- No LOD distance checks in render

**Fix Priority:** 🟡 HIGH

---

### 4. Vegetation System - MEDIUM
**File:** `world/VegetationSystem.cpp`

**Current:**
- 80 trees per chunk × 9 chunks = 720 trees
- Each tree is a separate draw call
- No instanced rendering

**Fix Priority:** 🟡 HIGH

---

### 5. Debug Output Spam - MINOR but Annoying
**Files:** Multiple

**Current:**
```cpp
// In render loops!
std::cout << "[World] Rendering 0 objects\n";  // Called every frame!
```

**Impact:**
- Console I/O blocks rendering pipeline
- Should only log on state changes

**Fix Priority:** 🟢 MEDIUM

---

## Optimization Strategy

### Phase 1: Quick Wins (1-2 hours)
1. **Remove console I/O from render loops**
   - Delete all `std::cout` from `SimpleWorldRenderer::render()`
   - Delete all `std::cout` from main render loop
   - Only log on initialization/state changes

2. **Batch shader uniform updates**
   - Move `setMat4("model", ...)` outside loop where possible
   - Use instanced rendering for world objects

### Phase 2: Medium Optimizations (2-4 hours)
3. **Implement instanced rendering for SimpleWorldRenderer**
   - Store model matrices in VBO
   - Single draw call for all instances
   - Expected speedup: 50-100x

4. **Optimize animator bone upload**
   - Use uniform buffer object (UBO) for bone matrices
   - Upload once per frame, not per mesh
   - Expected speedup: 2-5x

### Phase 3: Advanced (4-8 hours)
5. **Terrain frustum culling**
   - Don't render chunks outside camera view
   - Expected speedup: 2-10x (depends on view distance)

6. **Vegetation instancing**
   - Batch tree rendering
   - Expected speedup: 10-50x

---

## Expected Performance Gains

| Optimization | Current | After | Speedup |
|--------------|---------|-------|---------|
| Remove render logging | ~5 FPS | ~15 FPS | 3x |
| Instanced world objects | ~15 FPS | ~45 FPS | 3x |
| Bone matrix UBO | ~45 FPS | ~60 FPS | 1.3x |
| Terrain culling | ~60 FPS | ~90+ FPS | 1.5x |
| **TOTAL** | **~5 FPS** | **~90+ FPS** | **18x** |

---

## Implementation Order

1. ✅ **Remove console I/O from render loops** (5 min)
2. ✅ **Fix SimpleWorldRenderer** (30 min)
3. ⏳ **Optimize bone matrix upload** (1 hour)
4. ⏳ **Add terrain frustum culling** (2 hours)
5. ⏳ **Vegetation instancing** (2 hours)

---

## Test Metrics

Before optimization:
- Log shows: "Rendering 0 objects" spam every frame
- Camera: Stuck/laggy
- FPS: ~5-15 (estimated)

After Phase 1:
- No render loop logging
- Camera: Smooth
- FPS: ~30-45

After Phase 2:
- Instanced rendering active
- Camera: Very smooth
- FPS: ~60+
