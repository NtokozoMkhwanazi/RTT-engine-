# Hybrid Motion Matching + FSM Architecture Analysis

## Executive Summary

The current Hybrid MM+FSM implementation has **critical architectural flaws** that prevent proper locomotion blending. The character stays in idle animation regardless of input because the motion matching query doesn't properly match walk/run poses.

---

## Part 1: AAA Game Engine Motion Matching Architecture

### How Ubisoft/EA/Naughty Dog Do It

#### 1. **Unified Motion Database**
```
AAA Approach:
┌─────────────────────────────────────────┐
│     SINGLE MOTION DATABASE (1000+ poses) │
│  ┌─────┬─────┬─────┬─────┬─────┐       │
│  │Idle │Walk │Run  │Strafe│Jump│  ...  │
│  │ 50  │ 200 │ 200 │ 150  │ 50 │       │
│  └─────┴─────┴─────┴─────┴─────┘       │
│                                          │
│  All animations sampled at 30-60fps     │
│  Root velocity extracted for EACH frame │
│  Feature vectors include:               │
│    - Root velocity (XZ)                 │
│    - Speed magnitude                    │
│    - Move direction relative to facing  │
│    - Foot heights/velocities            │
│    - Bone poses (compressed)            │
└─────────────────────────────────────────┘
```

#### 2. **Search Query Construction**
```cpp
// AAA studios build query from CHARACTER STATE, not input
MotionFeatures query;
query.rootVelocity = character.getActualVelocity();  // From PREVIOUS frame
query.speed = length(query.rootVelocity);
query.moveDirection = normalize(inputDirection);
query.facingAngle = character.getYaw();
query.leftFootHeight = skeleton.getFootHeight(Left);
query.rightFootHeight = skeleton.getFootHeight(Right);

// Search finds poses that MATCH current trajectory
Pose bestPose = database.search(query);
```

#### 3. **State Machine Integration**
```
┌──────────────────────────────────────────────────────┐
│                    FSM LAYER                         │
│  ┌─────────┐    ┌─────────┐    ┌─────────┐         │
│  │ JUMP    │    │  FALL   │    │  COMBAT │         │
│  └────┬────┘    └────┬────┘    └────┬────┘         │
│       │              │              │               │
│       └──────────────┼──────────────┘               │
│                      │                              │
│              ┌───────▼────────┐                     │
│              │   LOCOMOTION   │ ◄── MM handles this│
│              │   (MM Active)  │                     │
│              └────────────────┘                     │
└──────────────────────────────────────────────────────┘

Key Insight: MM IS the locomotion state, not separate from it
```

#### 4. **Feature Extraction (CRITICAL)**
```cpp
// For EACH frame of EACH animation:
for (float time = 0; time < duration; time += 1/30f) {
    // Sample root bone position
    vec3 rootPos = rootBone.samplePosition(time);
    vec3 nextPos = rootBone.samplePosition(time + 1/30f);
    
    // Calculate velocity from delta
    vec3 rootVelocity = (nextPos - rootPos) / (1/30f);
    
    // Store in pose features
    pose.features.speed = length(rootVelocity.xz);
    pose.features.rootVelocity = rootVelocity;
    pose.features.moveAngle = atan2(rootVelocity.x, rootVelocity.z);
}
```

---

## Part 2: Current Implementation Issues

### Issue 1: Feature Extraction Broken ❌

**Current Code (MotionDatabase.cpp:242-258):**
```cpp
// Try bone index "0" first (WRONG - bone names are strings like "Hips")
auto it = anim->boneAnimations.find("0");  // Won't find anything!

// Fallback to first bone (might not be root!)
if (!anim->boneAnimations.empty()) {
    rootBoneAnim = &anim->boneAnimations.begin()->second;
}
```

**Problem:** 
- Bone "0" doesn't exist - Mixamo uses "Hips" or "Mixamorig:Hips"
- First bone might be a finger, not the root
- Root velocity extraction fails → all poses have speed=0
- KD-Tree can't distinguish idle/walk/run (all have speed=0)

### Issue 2: Search Query Uses Wrong Data ❌

**Current Code (test.cpp):**
```cpp
// Character velocity is ZERO because root motion isn't applied
hybridState.velocity = characterVelocity;  // Always (0,0,0)!
hybridState.position = characterPos;       // Updated by fallback movement
```

**Problem:**
- `characterVelocity` is calculated from position delta
- But position is updated by FALLBACK movement, not animation
- Query speed = 0 even when walking
- MM returns idle poses (closest match to speed=0)

### Issue 3: Animation Names Not Matching ❌

**Current Output:**
```
Current Animation: mixamo.com  // Always shows this!
```

**Problem:**
- Animation name is "mixamo.com" (from FBX metadata)
- Not "Idle", "Walk", or "Run"
- Can't verify which animation is playing

### Issue 4: FSM Transitions Don't Affect MM ❌

**Current Architecture:**
```
FSM States: LOCOMOTION, JUMP, FALL, CROUCH
MM Database: Has idle+walk+run poses

When WASD pressed:
1. FSM stays in LOCOMOTION state ✓
2. MM should blend idle→walk→run ✗ (broken)
3. Character moves via FALLBACK, not MM ✗
```

---

## Part 3: Recommended Fixes (Priority Order)

### Route A: Quick Fix (1-2 hours) ✅ RECOMMENDED FIRST

**Goal:** Get basic MM working with proper feature extraction

1. **Fix root bone lookup:**
```cpp
// Search for actual root bone names
std::vector<std::string> rootBoneNames = {
    "Hips", "mixamorig:Hips", "Root", "root", "Hip"
};
for (const auto& name : rootBoneNames) {
    auto it = anim->boneAnimations.find(name);
    if (it != anim->boneAnimations.end()) {
        rootBoneAnim = &it->second;
        break;
    }
}
```

2. **Fix character velocity calculation:**
```cpp
// Use root motion from animator, not position delta
glm::vec3 rootMotion = animator->ConsumeRootMotion();
characterVelocity = rootMotion / dt;  // Actual animation velocity
```

3. **Add debug output:**
```cpp
std::cout << "[MM Query] speed=" << query.speed 
          << " vel=" << query.rootVelocity << "\n";
```

### Route B: Proper Refactor (1-2 days) ⚠️ NEEDED FOR AAA QUALITY

**Goal:** Restructure to match AAA architecture

1. **Unified Motion Database:**
```cpp
// Single database for ALL locomotion
MotionDatabase locomotionDB;
locomotionDB.AddAnimation("Idle", idleAnim);
locomotionDB.AddAnimation("Walk", walkAnim);
locomotionDB.AddAnimation("Run", runAnim);
locomotionDB.AddAnimation("StrafeLeft", strafeLeftAnim);
locomotionDB.AddAnimation("StrafeRight", strafeRightAnim);
locomotionDB.BuildSearchIndex();  // ONE KD-Tree
```

2. **Proper Feature Extraction:**
```cpp
void MotionDatabase::ExtractPoseFeatures(...) {
    // Find root bone by name (Hips, Root, etc.)
    const BoneAnimation* rootBone = FindRootBone(anim);
    
    // Sample position at current and next frame
    vec3 pos = rootBone->InterpolatePosition(time);
    vec3 nextPos = rootBone->InterpolatePosition(time + dt);
    
    // Calculate velocity
    pose.features.rootVelocity = (nextPos - pos) / dt;
    pose.features.speed = length(pose.features.rootVelocity.xz);
    pose.features.moveAngle = atan2(rootVelocity.x, rootVelocity.z);
}
```

3. **Query from Character State:**
```cpp
void MotionMatcher::Update(float dt, const CharacterState& state) {
    // Use ACTUAL character velocity (from previous frame's root motion)
    characterVelocity = previousFrameRootMotion / dt;
    
    // Build query
    MotionFeatures query;
    query.rootVelocity = characterVelocity;
    query.speed = length(characterVelocity.xz);
    query.moveDirection = state.moveDirection;
    
    // Search and blend
    SearchAndBlend(dt);
}
```

4. **FSM Integration:**
```cpp
void HybridMMFSM::Update(float dt, const HybridMMFSMState& state) {
    characterState = state;
    
    // Update state machine
    UpdateStateMachine(dt);
    
    switch (currentState) {
        case HybridState::LOCOMOTION:
            // MM handles idle/walk/run blending
            motionMatcher.Update(dt, state);
            break;
            
        case HybridState::JUMP:
            // Play jump animation (one-shot)
            PlayStateAnimation(HybridState::JUMP, dt);
            break;
            
        case HybridState::FALL:
            // Play fall animation (looping)
            PlayStateAnimation(HybridState::FALL, dt);
            break;
    }
}
```

### Route C: Complete Rewrite (1-2 weeks) 🔥 ONLY IF A/B FAIL

**Goal:** Production-ready system matching The Last of Us / Assassin's Creed

Key changes:
1. Separate animation player for MM (don't use Animator directly)
2. Add trajectory prediction with multiple future points
3. Implement proper inertialization for all transitions
4. Add pose caching and GPU skinning optimization
5. Support animation compression (keyframe reduction)

---

## Part 4: Immediate Action Plan

### Step 1: Fix Feature Extraction (30 min)
- Fix root bone lookup to search for "Hips", "mixamorig:Hips"
- Verify velocity extraction with debug output
- Rebuild KD-Tree and verify node count (>100 for 138 poses)

### Step 2: Fix Character Velocity (30 min)
- Use root motion from animator for velocity calculation
- Update query to use actual animation velocity
- Verify query speed > 0 when walking

### Step 3: Verify MM is Working (30 min)
- Check that animation name changes when pressing W
- Verify bone positions change for walk cycle
- Check FPS impact (should be <0.1ms per frame)

### Step 4: Polish (1 hour)
- Add proper debug visualization
- Tune search parameters (number of neighbors, blend weights)
- Add foot planting on top of MM

---

## Conclusion

**Current Status:** MM database is built but search always returns idle poses because:
1. Root bone lookup fails → velocity = 0 for all poses
2. Character velocity = 0 → query matches idle poses
3. Fallback movement moves character, not MM

**Recommended Fix:** Start with Route A (quick fixes) to verify MM works, then implement Route B (proper refactor) for production quality.

**Expected Result After Fix:**
- Idle: speed ≈ 0 m/s
- Walk: speed ≈ 1-2 m/s  
- Run: speed ≈ 4-6 m/s
- Smooth blending between states based on input magnitude
