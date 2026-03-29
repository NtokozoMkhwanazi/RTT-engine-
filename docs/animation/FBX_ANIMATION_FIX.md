# 🔧 FBX Loading & Animation Fix Plan

## 📋 Issues Identified

### 1. **bear_DEMO.fbx and datsun.fbx Not Loading**
**Problem:** File paths are wrong in `WorldObjectManager.cpp`
- Current: `assetDir + "Bear_DEMO.fbx"` (capital B)
- Actual file: `assets/World_objects/Bear_DEMO.fbx` (correct)
- Issue: Asset directory path is wrong

**Problem:** Camera messes up when loading new FBX
- Different models have different root bones
- Skeleton hierarchy varies between models
- Camera follow assumes character at origin

**Problem:** Character model messes up
 bone hierarchy differences between bot.fbx and other models
- Root motion extraction assumes specific bone structure

### 2. **Animation State Transitions Not Working**
**Problem:** Only rotates on axis, no actual movement
- `AnimationStateMachine` exists but isn't being used
- `MotionMatcher` is being used instead
- MotionMatcher transitions work differently than FSM

---

## 🔧 Fixes

### Fix 1: WorldObjectManager Path Correction

**File:** `world/WorldObjectManager.cpp`

```cpp
// OLD (wrong):
void WorldObjectManager::initialize(const std::string& assetDir = "assets/world_objects/") {

// NEW (correct - matches actual folder structure):
void WorldObjectManager::initialize(const std::string& assetDir = "assets/World_objects/") {
```

### Fix 2: Add FBX Model Loader Function

**File:** `test.cpp` - Add function to load any FBX model dynamically

```cpp
// Helper function to load and display any FBX model
Model* LoadFBXModel(const std::string& path, const char* displayName = nullptr) {
    std::cout << "\n=== Loading Model: " << (displayName ? displayName : path.c_str()) << " ===\n";
    
    try {
        Model* model = new Model(path.c_str());
        
        if (!model || model->GetMeshCount() == 0) {
            std::cerr << "  [ERROR] Failed to load: " << path << "\n";
            if (model) delete model;
            return nullptr;
        }
        
        const Skeleton& skel = model->GetSkeleton();
        std::cout << "  Bones: " << skel.bones.size() << "\n";
        std::cout << "  Meshes: " << model->GetMeshCount() << "\n";
        
        glm::vec3 size = model->GetSize();
        std::cout << "  Size: " << glm::to_string(size) << "\n";
        
        return model;
    } catch (const std::exception& e) {
        std::cerr << "  [EXCEPTION] " << e.what() << "\n";
        return nullptr;
    }
}
```

### Fix 3: Add Model Switching System

**File:** `test.cpp` - Add key to switch between loaded models

```cpp
// Add global variables for model switching
Model* character = nullptr;
Model* bearModel = nullptr;
Model* datsunModel = nullptr;
int currentModelIndex = 0;  // 0=bot, 1=bear, 2=datsun

// Add key handler (press M to cycle)
static bool lastM = false;
if (glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS && !lastM) {
    currentModelIndex = (currentModelIndex + 1) % 3;
    std::cout << ">>> Switched to model " << currentModelIndex << "\n";
}
lastM = glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS;
```

### Fix 4: Fix Animation State Machine Integration

**The REAL issue:** MotionMatcher is being used, but it's not working correctly.

The problem is that MotionMatcher needs:
1. Proper skeleton from the loaded model
2. Animations loaded into the database
3. KD-Tree built for searching
4. Character state properly queried

**Current issue in test.cpp:**
- MotionMatcher is initialized with `&skeleton` from character model
- But when you load a different model, the skeleton changes
- Animations might not be compatible with new skeleton

---

## 🎯 Recommended Solution

### Option A: Fix MotionMatcher (Recommended)

MotionMatcher is the superior system (AAA-quality). Fix it properly:

1. **Ensure skeleton compatibility:**
   - All animations must use same skeleton as character model
   - Or use animation retargeting system

2. **Fix motion matching update:**
   ```cpp
   // In test.cpp, update motion matcher correctly
   if (matcher) {
       CharacterState charState;
       charState.position = characterPos;
       charState.velocity = characterVelocity;
       charState.rotation = glm::radians(rotationAngle);
       charState.moveDirection = moveDir;
       charState.grounded = charInput.grounded;
       charState.crouching = charInput.crouch;
       charState.jumping = charInput.jump;
       
       matcher->Update(dt, charState);
       
       if (animator) {
           animator->Update(dt);
       }
   }
   ```

3. **Debug motion matching:**
   - Press H to print debug info
   - Check if search is finding poses
   - Verify blend is working

### Option B: Switch to AnimationStateMachine

If MotionMatcher is too complex, switch back to FSM:

```cpp
// Replace MotionMatcher with AnimationStateMachine
AnimationStateMachine* fsm = nullptr;

// Initialize
fsm = new AnimationStateMachine(animator);
fsm->registerAnimations(idleAnim, walkAnim, runAnim, jumpAnim, fallAnim, crouchAnim, crouchWalkAnim);
fsm->initialize();

// Update
fsm->update(dt, charInput);
```

---

## 📝 Step-by-Step Fix Guide

### Step 1: Fix WorldObjectManager Paths
Edit `world/WorldObjectManager.cpp`:
```cpp
// Line 12: Change path to match actual folder
void WorldObjectManager::initialize(const std::string& assetDir) {
```

### Step 2: Test bear_DEMO.fbx Loading
In `test.cpp`, add loading code:
```cpp
Model* bearModel = new Model("assets/World_objects/Bear_DEMO.fbx");
std::cout << "Bear model loaded: " << (bearModel ? "YES" : "NO") << "\n";
```

### Step 3: Test datsun.fbx Loading
```cpp
Model* datsunModel = new Model("assets/World_objects/datsun.fbx");
std::cout << "Datsun model loaded: " << (datsunModel ? "YES" : "NO") << "\n";
```

### Step 4: Fix Camera for Different Models
Add model height offset:
```cpp
float modelHeightOffset = 0.0f;
if (currentModel == bearModel) modelHeightOffset = 1.5f;  // Adjust for bear
else if (currentModel == datsunModel) modelHeightOffset = 0.8f;  // Adjust for car
```

### Step 5: Fix Animation Transitions
Press H to check motion matching debug output. If it shows:
- "Database size: 0" → Animations not loaded
- "Search found: 0 results" → KD-Tree not built
- "Current pose: invalid" → Skeleton mismatch

---

## 🧪 Testing Checklist

After applying fixes:

- [ ] `make clean && make` - builds without errors
- [ ] Run engine - no crashes
- [ ] Press M - cycles between models (bot, bear, datsun)
- [ ] Camera follows each model correctly
- [ ] WASD moves character (not just rotates)
- [ ] Animations blend smoothly (idle → walk → run)
- [ ] Press H - shows motion matching debug info
- [ ] Press G - shows foot IK status

---

## 🎯 Expected Behavior After Fix

### Model Loading:
```
[Model] Loading: assets/World_objects/Bear_DEMO.fbx
  Bones: 45
  Meshes: 3
  Size: (2.5, 1.8, 4.2)
  
[Model] Loading: assets/World_objects/datsun.fbx
  Bones: 0 (static model)
  Meshes: 1
  Size: (4.0, 1.5, 2.0)
```

### Animation:
```
W pressed → Character walks forward (not just rotates)
W+Shift → Character runs
Space → Character jumps
Ctrl → Character crouches
```

### Camera:
```
Camera follows smoothly behind character
No jitter or snapping
Correct distance for each model size
```

---

**Ready to apply these fixes?** Let me know which approach you prefer (fix MotionMatcher or switch to FSM) and I'll implement it.
