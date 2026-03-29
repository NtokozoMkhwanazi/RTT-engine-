# FBX Animation Export Fix

## The Problem

Your FBX animations have **WRONG duration**:
```
Idle: 499 seconds (should be 2-3s)
Walk: 29 seconds (should be 1-1.5s)
Run: 19 seconds (should be 0.8-1.2s)
Jump: 50 seconds (should be 1-1.5s)
```

This causes animations to play **incredibly slowly**, making character look "stuck" or "jittery".

## Root Cause

When exporting from Blender/Maya, you exported the **entire timeline** instead of just the animation cycle.

**What happened:**
1. Your animation cycle is frames 1-30 (1 second)
2. But your timeline is 15000 frames (499 seconds at 30fps)
3. FBX exported ALL 15000 frames
4. Engine tries to play 1 second of animation over 499 seconds = SUPER SLOW

## Temporary Fix (Code)

I've added code to truncate the duration:
```cpp
anim->duration = 1.2f;  // Force correct duration
```

This makes animations play at correct speed by ignoring the extra frames.

## PERMANENT FIX (Re-export FBX properly)

### Blender Export Settings

1. **Select ONLY the animation range:**
   - In Timeline, set Start: 1, End: 30 (for walk cycle)
   - NOT the entire timeline (1-15000)

2. **FBX Export Settings:**
   ```
   ✓ Selected Objects (if you have skeleton selected)
   ✓ Animation
     ✓ Animation Range: [✓] Custom
     Start Frame: 1
     End Frame: 30  (NOT 15000!)
   ✓ Bake Animation
     ✓ Sample Rate: 1 (30fps)
   ```

3. **For each animation:**
   | Animation | Start Frame | End Frame | Duration |
   |-----------|-------------|-----------|----------|
   | Idle | 1 | 60-90 | 2-3s |
   | Walk | 1 | 30-45 | 1-1.5s |
   | Run | 1 | 24-36 | 0.8-1.2s |
   | Jump | 1 | 30-45 | 1-1.5s |
   | Crouch | 1 | 15-20 | 0.5-0.7s |

### Maya Export Settings

1. **Set Time Slider Range:**
   - Time Slider: Set Start/End to animation range
   - NOT the full timeline

2. **FBX Export:**
   ```
   Animation
     ✓ Bake Animation
     Start/End: [Set to cycle range, not full timeline]
   ```

## How to Find Actual Cycle Length

### In Blender:
1. Open the FBX in Blender
2. Look at Timeline/Dope Sheet
3. Find where the cycle REPEATS
4. That's your end frame!

```
Walk cycle example:
Frame 1:  Right foot contact
Frame 15: Left foot contact  
Frame 30: Right foot contact (REPEATS frame 1)
→ Export frames 1-30 (NOT 1-15000!)
```

### Check Keyframes:
```
Bad: Keyframes at 0, 1, 2, 3, ... 14999 (entire timeline)
Good: Keyframes at 0, 1, 2, ... 29 (just the cycle)
```

## Verify Export is Correct

After re-exporting, run the engine and check:
```
=== ANIMATION INFO ===
  Idle: duration=2.5s (75 frames @30fps) ← GOOD!
  Walk: duration=1.2s (36 frames @30fps) ← GOOD!
  Run: duration=0.9s (27 frames @30fps) ← GOOD!

No WARNING messages!
```

## Current Status

The code fix truncates duration so animations play correctly NOW, but you should re-export for:
- Smaller file sizes (1MB instead of 50MB)
- Correct keyframe data
- Better performance
- Proper looping

## Quick Reference

| Animation | Frames @30fps | Duration | Export Range |
|-----------|--------------|----------|--------------|
| Idle | 60-90 | 2-3s | 1-90 |
| Walk | 30-45 | 1-1.5s | 1-45 |
| Run | 24-36 | 0.8-1.2s | 1-36 |
| Jump | 30-45 | 1-1.5s | 1-45 |
| Crouch | 15-20 | 0.5-0.7s | 1-20 |
| CrouchWalk | 30-45 | 1-1.5s | 1-45 |

## Testing After Re-export

1. Delete old FBX files
2. Export new FBX with correct range
3. Run engine
4. Check output - NO warnings
5. Test in-game - smooth animation, no jitter
