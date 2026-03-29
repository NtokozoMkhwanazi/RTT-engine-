# World Performance Optimization Guide

## Current Issues (From Debug Output)

```
[FPS] 161 (31.5674ms/frame)  ← Should be ~60 FPS (16ms/frame)
[CAMERA] Pos=(1.88813e+09, -8.14989e+16, 1.00673e+09)  ← NaN/explosion
[CHARACTER] Pos=(0, 40, 0)  ← Floating in air
[DISTANCE] Camera-to-character=8.14989e+16 units  ← Way too far
```

**Root Cause:** NaN propagation causing camera explosion, which then causes rendering issues.

---

## ✅ Immediate Fixes Applied

### 1. NaN Prevention
- Added `std::isfinite()` checks for all positions
- Added `glm::clamp()` for smooth factors (prevent >1.0)
- Added zero-length vector checks before normalize()

### 2. Terrain Spawn Fix
- Character now spawns at terrain height
- Fallback to Y=0 if terrain is null or returns NaN

### 3. Camera Safety
- Validated all camera target positions
- Clamped interpolation factors to [0, 1]

---

## World Performance Optimizations (Recommended)

### Priority 1: Frustum Culling (HIGH IMPACT)

**Problem:** Rendering all terrain chunks even when not visible.

**Solution:**
```cpp
// In Terrain::Render()
for (auto& chunk : chunks) {
    if (camera.FrustumIntersects(chunk.bounds)) {
        chunk.Render();
    }
}
```

**Expected Gain:** 40-60% fewer draw calls

---

### Priority 2: LOD System (HIGH IMPACT)

**Problem:** Rendering high-res terrain at all distances.

**Solution:**
```cpp
// Distance-based LOD
float distance = glm::length(chunk.position - camera.Position);

if (distance < 50.0f) {
    chunk.SetLOD(0);  // Full resolution
} else if (distance < 150.0f) {
    chunk.SetLOD(1);  // 50% triangles
} else if (distance < 300.0f) {
    chunk.SetLOD(2);  // 25% triangles
} else {
    chunk.SetLOD(3);  // 10% triangles
}
```

**Expected Gain:** 50-70% fewer triangles rendered

---

### Priority 3: Occlusion Culling (MEDIUM IMPACT)

**Problem:** Rendering objects behind terrain/mountains.

**Solution:**
```cpp
// Hardware occlusion queries
GLuint query;
glGenQueries(1, &query);

glBeginQuery(GL_SAMPLES_PASSED, query);
// Render bounding box
glEndQuery(GL_SAMPLES_PASSED);

GLuint samples;
glGetQueryObjectuiv(query, GL_QUERY_RESULT, &samples);

if (samples > 0) {
    // Object is visible, render it
    RenderObject();
}
```

**Expected Gain:** 20-30% fewer pixels rendered

---

### Priority 4: Instanced Rendering (MEDIUM IMPACT)

**Problem:** Rendering trees/rocks with individual draw calls.

**Solution:**
```cpp
// Instead of:
for (auto& tree : trees) {
    modelMatrix = tree.transform;
    shader.SetMatrix("model", modelMatrix);
    treeModel.Draw();  // 1000 draw calls!
}

// Use instancing:
std::vector<glm::mat4> transforms;
for (auto& tree : trees) {
    transforms.push_back(tree.transform);
}

glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
glBufferData(GL_ARRAY_BUFFER, transforms.size() * sizeof(glm::mat4), 
             transforms.data(), GL_STATIC_DRAW);

// Set up instanced attribute
for (int i = 0; i < 4; i++) {
    glEnableVertexAttribArray(3 + i);
    glVertexAttribPointer(3 + i, 4, GL_FLOAT, GL_FALSE, 
                         sizeof(glm::mat4), (void*)(sizeof(float) * 4 * i));
    glVertexAttribDivisor(3 + i, 1);  // Per-instance
}

// Single draw call for all instances!
glDrawElementsInstanced(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, 0, 
                        transforms.size());
```

**Expected Gain:** 10-50x fewer draw calls for vegetation

---

### Priority 5: Texture Atlasing (MEDIUM IMPACT)

**Problem:** Multiple texture binds per frame.

**Solution:**
```cpp
// Combine multiple textures into one atlas
// UV coordinates remapped to atlas regions

// Before: 10 texture binds
for (auto& rock : rocks) {
    glBindTexture(GL_TEXTURE_2D, rock.texture);
    rock.Draw();
}

// After: 1 texture bind
glBindTexture(GL_TEXTURE_2D, textureAtlas);
for (auto& rock : rocks) {
    rock.Draw();  // Uses different UV region
}
```

**Expected Gain:** 5-10x fewer texture binds

---

### Priority 6: Batch Rendering (LOW-MEDIUM IMPACT)

**Problem:** Multiple shader program switches.

**Solution:**
```cpp
// Group objects by material/shader
struct RenderBatch {
    Shader* shader;
    std::vector<Mesh*> meshes;
};

std::map<Shader*, RenderBatch> batches;

// Sort objects into batches
for (auto& obj : objects) {
    batches[obj.shader].meshes.push_back(&obj.mesh);
}

// Render by batch (minimal shader switches)
for (auto& [shader, batch] : batches) {
    shader->use();
    for (auto& mesh : batch.meshes) {
        mesh->Draw();
    }
}
```

**Expected Gain:** 2-5x fewer shader switches

---

### Priority 7: Multithreaded Loading (LOW IMPACT)

**Problem:** Loading stalls main thread.

**Solution:**
```cpp
// Async loading thread
std::future<Model*> future = std::async(std::launch::async, []() {
    return new Model("heavy_model.fbx");
});

// Main thread continues...
// ...
// When needed:
Model* model = future.get();  // Wait if not ready
```

**Expected Gain:** No loading stalls

---

## Performance Targets

| Optimization | Current | Target | Gain |
|-------------|---------|--------|------|
| **FPS** | 30-60 | 60+ | 2x |
| **Draw Calls** | ~1000 | ~200 | 5x |
| **Triangles** | ~500K | ~100K | 5x |
| **Texture Binds** | ~50 | ~10 | 5x |
| **Load Time** | 5s | 2s | 2.5x |

---

## Implementation Order

### Phase 1: Critical Fixes (DONE ✅)
- [x] NaN prevention
- [x] Terrain spawn fix
- [x] Camera safety checks

### Phase 2: High Impact (4-8 hours)
- [ ] Frustum culling for terrain chunks
- [ ] Distance-based LOD
- [ ] Instanced rendering for vegetation

### Phase 3: Medium Impact (4-6 hours)
- [ ] Occlusion culling
- [ ] Texture atlasing
- [ ] Batch rendering

### Phase 4: Polish (2-4 hours)
- [ ] Multithreaded loading
- [ ] GPU profiling
- [ ] Memory optimization

---

## Debug Commands

Add these for performance debugging:

```cpp
// Press F1: Show performance stats
if (glfwGetKey(window, GLFW_KEY_F1) == GLFW_PRESS) {
    std::cout << "\n=== PERFORMANCE STATS ===\n";
    std::cout << "FPS: " << fps << "\n";
    std::cout << "Draw calls: " << drawCalls << "\n";
    std::cout << "Triangles: " << triangles << "\n";
    std::cout << "Visible chunks: " << visibleChunks << "/" << totalChunks << "\n";
}

// Press F2: Toggle wireframe
if (glfwGetKey(window, GLFW_KEY_F2) == GLFW_PRESS) {
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
}

// Press F3: Show LOD debug
if (glfwGetKey(window, GLFW_KEY_F3) == GLFW_PRESS) {
    ShowLODColors();  // Different colors for different LODs
}
```

---

## Profiling Tools

### Built-in Profiler
```cpp
class ScopedTimer {
    std::chrono::high_resolution_clock::time_point start;
    std::string name;
public:
    ScopedTimer(const std::string& n) : name(n) {
        start = std::chrono::high_resolution_clock::now();
    }
    ~ScopedTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        std::cout << name << ": " << ms << "ms\n";
    }
};

// Usage:
void Render() {
    ScopedTimer timer("Render");
    // ...
}
```

### External Tools
- **RenderDoc:** GPU debugging
- **NVIDIA Nsight:** Performance profiling
- **Intel GPA:** Frame analysis

---

## Conclusion

**Immediate action:** Test the NaN fixes first. Once stable, implement optimizations in priority order.

**Expected result:** 60+ FPS with large open world, smooth camera, no NaN crashes.
