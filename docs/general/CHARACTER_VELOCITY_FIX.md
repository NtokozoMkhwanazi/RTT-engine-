# CRITICAL FIX: Character Velocity Not Calculated

## Problem
Character was **stuck in idle animation** forever, even when pressing WASD to move.

## Root Cause
`characterVelocity` was declared but **never calculated**:
```cpp
glm::vec3 characterVelocity(0.0f);  // ← Never changed from (0,0,0)!
```

Motion matching received velocity=(0,0,0) every frame, so it always selected idle animation.

## Symptoms
- Character stays in idle animation regardless of input
- Pressing W/A/S/D doesn't trigger walk/run animations
- Motion matching debug shows `Speed=0` always
- Root motion was moving character, but velocity wasn't tracked

## Solution
Calculate velocity from position delta after root motion is applied:

```cpp
// 1. Add previous position tracking (line ~844)
glm::vec3 prevCharacterPos(0.0f, 0.0f, 0.0f);

// 2. Calculate velocity after root motion update (line ~1188)
characterVelocity = (characterPos - prevCharacterPos) / dt;
prevCharacterPos = characterPos;  // Save for next frame
```

## Files Modified
- `test.cpp` - Added velocity calculation

## Testing
1. Build: `make clean && make`
2. Run: `./bin/run`
3. Press W to walk forward - should transition from Idle → Walk animation
4. Press Shift+W to run - should transition to Run animation
5. Press H to debug - should show non-zero speed when moving

## Expected Behavior After Fix
- Standing still → **Idle** animation
- Walking (W) → **Walk** animation  
- Running (Shift+W) → **Run** animation
- Jumping (Space) → **Jump** animation
- Crouching (Ctrl) → **Crouch** animation

## Motion Matching Debug Output
Press H in-game to see:
```
=== MOTION MATCHING DEBUG ===
Current pose: 42
Animation: Walk @ 0.533s
Search: 10980 poses, 0ms
Score: 0.234
Feet: L=PLANTED R=FREE
```

When moving, you should see:
- Animation changes from "Idle" to "Walk" or "Run"
- Speed value > 0 when pressing WASD
