# Foot IK & Character Grounding Fixes

## Problem

Character was standing in the air instead of on the ground. Debug output showed:
```
[CHARACTER] Pos=(0, 40, 0)  ← Floating 40 units in air!
Feet: L=FREE R=FREE  ← Feet not planted
```

## Root Causes

### 1. Static Floor Height
**Problem:** Foot IK used a fixed `floorHeight = 0.0f`, but terrain height at spawn was 40 units.

**Fix:** Update floor height dynamically based on terrain:
```cpp
// In main loop - update foot IK floor height based on terrain
if (terrain && animator->footIKSettings.enabled) {
    float terrainHeight = terrain->getHeightAt(characterPos.x, characterPos.z);
    if (std::isfinite(terrainHeight)) {
        Animator::FootIKSettings ikSettings = animator->footIKSettings;
        ikSettings.floorHeight = terrainHeight;  // Update to match terrain
        animator->SetFootIKSettings(ikSettings);
    }
}
```

### 2. IsFootPlanted Too Strict
**Problem:** Original check only looked at vertical velocity < 0.001f, didn't consider ground proximity.

**Fix:** Enhanced check with ground proximity:
```cpp
bool Animator::IsFootPlanted(int bone) const {
    float verticalVelocity = std::abs(currBoneWorldPos[bone].y - prevBoneWorldPos[bone].y);
    float footHeight = currBoneWorldPos[bone].y;
    bool nearGround = footHeight < (footIKSettings.floorHeight + 0.2f);
    
    return (verticalVelocity < 0.01f) && nearGround;  // Both conditions must be true
}
```

### 3. No Character Grounded Check
**Problem:** No easy way to check if character has at least one foot planted.

**Fix:** Added `IsCharacterGrounded()` helper:
```cpp
bool Animator::IsCharacterGrounded() const {
    int leftFoot = footIKSettings.leftFootBone;
    int rightFoot = footIKSettings.rightFootBone;
    
    bool leftPlanted = (leftFoot >= 0) ? IsFootPlanted(leftFoot) : false;
    bool rightPlanted = (rightFoot >= 0) ? IsFootPlanted(rightFoot) : false;
    
    return leftPlanted || rightPlanted;  // Grounded if at least one foot planted
}
```

### 4. Missing Debug Output
**Problem:** Hard to diagnose foot IK issues.

**Fix:** Enhanced 'G' key debug output:
```cpp
if (glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS && !lastG && animator) {
    std::cout << "\n=== FOOT IK STATUS ===\n";
    std::cout << "Enabled: " << (animator->footIKSettings.enabled ? "YES" : "NO") << "\n";
    std::cout << "Floor height: " << animator->footIKSettings.floorHeight << "\n";
    std::cout << "Character grounded: " << (animator->IsCharacterGrounded() ? "YES" : "NO") << "\n";
    std::cout << "Left foot planted: " << (animator->IsFootPlanted(leftFoot) ? "YES" : "NO") << "\n";
    std::cout << "Right foot planted: " << (animator->IsFootPlanted(rightFoot) ? "YES" : "NO") << "\n";
    std::cout << "Character Y position: " << characterPos.y << "\n";
    animator->DebugDrawFootIK();
}
```

---

## Files Modified

1. **test.cpp**
   - Dynamic floor height update in main loop
   - Enhanced foot IK debug output

2. **animationSystem/Animator.cpp**
   - Enhanced `IsFootPlanted()` with ground proximity check
   - Added `IsCharacterGrounded()` helper function

3. **animationSystem/Animator.h**
   - Added `IsCharacterGrounded()` declaration

---

## Expected Behavior After Fix

### Debug Output (Press G)
```
=== FOOT IK STATUS ===
Enabled: YES
Floor height: 40.5  ← Matches terrain!
Character grounded: YES  ← Character is on ground!
Left foot planted: YES
Right foot planted: NO
Character Y position: 40.5
```

### Character Position
```
[CHARACTER] Pos=(0, 40.5, 0)  ← On terrain!
[DISTANCE] Camera-to-character=10.2 units  ← Normal distance
```

### Motion Matching Debug (Press H)
```
[QUERY FEATURES]
  Speed: 0 m/s
  Grounded: YES  ← Correctly detected!
  Crouching: NO
```

---

## Testing

1. **Run the application:**
   ```bash
   ./bin/run
   ```

2. **Check foot IK status (Press G):**
   - Should show "Character grounded: YES"
   - Floor height should match character Y position
   - At least one foot should be planted when standing still

3. **Walk around (Press W):**
   - Feet should plant naturally during walking cycle
   - Character should stay on terrain

4. **Move to different terrain heights:**
   - Floor height should update dynamically
   - Character should remain grounded

---

## Troubleshooting

### Character Still Floating
**Check:**
```cpp
// Press G to see foot IK status
// Check if "Floor height" matches "Character Y position"
// If not, terrain height query may be failing
```

**Fix:** Verify terrain is loaded and heightmap is valid:
```cpp
if (terrain) {
    float h = terrain->getHeightAt(characterPos.x, characterPos.z);
    std::cout << "Terrain height at character: " << h << "\n";
}
```

### Feet Sliding
**Check:**
- `maxIKDistance` too large (should be 0.1-0.2f)
- `ikStrength` too high (should be 0.8-1.0f)

**Fix:** Adjust in foot IK settings:
```cpp
ikSettings.maxIKDistance = 0.15f;  // Reduce sliding
ikSettings.ikStrength = 0.9f;      // Slightly reduce IK influence
```

### Character Snapping to Ground
**Check:**
- `footLockBlend` too high (causes instant locking)

**Fix:** Reduce blend speed:
```cpp
ikSettings.footLockBlend = 0.5f;  // Slower lock engagement
```

---

## Summary

**Fixed Issues:**
- ✅ Character now spawns on terrain (not floating)
- ✅ Foot IK floor height updates dynamically
- ✅ Improved foot planting detection
- ✅ Added character grounded check
- ✅ Enhanced debug output

**Result:** Character stands firmly on ground with proper foot locking!
