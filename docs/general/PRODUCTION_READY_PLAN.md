# Production-Ready Optimization Plan

## Philosophy
- **Development:** Verbose logging for debugging
- **Production:** Minimal logging, efficient memory management
- **Tests:** Comprehensive coverage replaces runtime logging

## Phase 1: Remove Excessive Logging ✅

### Logging Policy
**KEEP (Important for users):**
- ✅ FPS counter (every 2-5 seconds)
- ✅ Critical errors (file not found, initialization failures)
- ✅ Loading progress (once per asset, not per frame)
- ✅ Motion matching debug (on key press 'H' only)

**REMOVE (Developer-only):**
- ❌ Per-frame debug prints
- ❌ Asset loading details (unless error)
- ❌ Animation state changes
- ❌ Render loop statistics
- ❌ Bone/vertex processing details

### Implementation
Use compile-time debug flags:
```cpp
#ifdef DEBUG
    std::cout << "[Debug] Info...\n";
#endif
```

Or runtime verbosity levels:
```cpp
if (verbosity >= Verbosity::DEBUG) {
    log("Detailed info");
}
```

---

## Phase 2: Memory & Asset Management

### Current Problems
1. **All assets load at startup** → Long initial load time
2. **No asset streaming** → High memory usage
3. **No loading screen** → User stares at black screen
4. **Frequent allocations** → Performance hitches

### Game-Standard Solutions

#### 1. Loading Screen System
```cpp
class LoadingScreen {
    void begin();
    void updateProgress(float percent);
    void end();
};

// Usage:
loadingScreen.begin();
loadingScreen.updateProgress(0.2f);  // Loading models...
loadingScreen.updateProgress(0.5f);  // Loading animations...
loadingScreen.updateProgress(0.8f);  // Building KD-Tree...
loadingScreen.end();
```

#### 2. Async Asset Loading
```cpp
class AssetManager {
    std::future<Model*> loadModelAsync(const std::string& path);
    void preloadAssets(const std::vector<std::string>& paths);
};

// Load in background while showing loading screen
auto modelFuture = assetManager.loadModelAsync("character.fbx");
// ... do other loading ...
Model* model = modelFuture.get();  // Wait when needed
```

#### 3. Level of Detail (LOD) Streaming
```cpp
class AssetStreamer {
    void update(const Camera& camera);
    // Load high-res for nearby objects
    // Unload far objects
    // Keep low-res for distant objects
};
```

#### 4. Object Pooling
```cpp
template<typename T>
class ObjectPool {
    T* acquire();  // Get from pool instead of new
    void release(T* obj);  // Return to pool instead of delete
};

// No allocations during gameplay!
```

#### 5. Memory Arenas
```cpp
class FrameArena {
    void* allocate(size_t size);
    void reset();  // Clear all at end of frame
};

// Fast allocation, bulk deallocation
```

---

## Implementation Priority

### Immediate (1-2 hours)
1. **Remove all non-essential prints**
   - Keep only errors and loading messages
   - Move debug to 'H' key press only

2. **Add loading screen**
   - Simple text-based progress
   - Show what's loading

3. **Pre-allocate known sizes**
   - Animation vectors
   - Bone matrices
   - Vertex buffers

### Short-term (2-4 hours)
4. **Async animation loading**
   - Load animations in background
   - Show progress bar

5. **Object pools for frequent allocations**
   - Particle systems
   - Temporary buffers

### Medium-term (4-8 hours)
6. **Asset streaming system**
   - Load/unload based on distance
   - Reduce memory footprint

7. **Memory arenas**
   - Frame-based allocation
   - Reduce fragmentation

---

## Expected Benefits

| Metric | Current | After Phase 1 | After Phase 2 |
|--------|---------|---------------|---------------|
| Log spam | High | Minimal | None (errors only) |
| Initial load | Blocking | With progress | Async + progress |
| Memory usage | High | Same | 30-50% lower |
| Frame hitches | Frequent | Reduced | Rare |
| FPS stability | Variable | Better | Stable |

---

## Code Standards

### Logging Macro
```cpp
#define LOG_INFO(fmt, ...)  printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  printf("[WARN] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) printf("[ERROR] " fmt "\n", ##__VA_ARGS__)

#ifdef DEBUG
    #define LOG_DEBUG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
    #define LOG_DEBUG(fmt, ...)
#endif
```

### Memory Allocation Rules
1. **Never allocate in render loop**
2. **Pre-allocate at startup**
3. **Use pools for frequent objects**
4. **Profile before optimizing**

---

## Testing Strategy

Since we're removing debug prints, tests become CRITICAL:

```bash
# Run before every commit
make test

# All 210 tests must pass
[==========] 210 tests from 10 test suites ran.
[  PASSED  ] 210 tests.
```

Tests replace runtime debugging!
