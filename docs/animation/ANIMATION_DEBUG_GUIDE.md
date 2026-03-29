# Animation Debug Output Guide

## What to Look For When You Run

When you run `./bin/run`, you'll see output like this:

```
=== ANIMATION INFO ===
Expected lengths @30fps:
  Idle: 60-90 frames (2-3s) - loops IN PLACE
  Walk: 30-45 frames (1-1.5s) - loops IN PLACE
  Run: 24-36 frames (0.8-1.2s) - loops IN PLACE
  Jump: 30-45 frames (1-1.5s) - ONE-SHOT with root motion

Actual animations:
  Idle: duration=2.5s (75 frames @30fps), ticks/sec=30, bones=45
  Walk: duration=1.2s (36 frames @30fps), ticks/sec=30, bones=45
  Run: duration=0.9s (27 frames @30fps), ticks/sec=30, bones=45
  Jump: duration=1.1s (33 frames @30fps), ticks/sec=30, bones=45
  Fall: NOT LOADED
  Crouch: duration=0.6s (18 frames @30fps), ticks/sec=30, bones=45
  CrouchWalk: duration=1.0s (30 frames @30fps), ticks/sec=30, bones=45

=== ROOT MOTION STATUS ===
Root position locked: NO (root motion DRIVES movement)

=== TROUBLESHOOTING ===
If character slides/stuck in pose:
  1. Animations too short? Should be 30-45 frames for walk
  2. Root motion wrong? Try: animator->SetLockRootPosition(true)
  3. Movement multiplier wrong? Adjust in code (currently 0.25f)
```

## Animation Problems & Solutions

### Problem 1: Character "Stuck" in Running Pose

**Symptoms:**
- Character looks like they're running but not moving legs
- Animation plays at correct speed but no leg movement
- Character slides across ground

**Cause:** Animation is too short or missing key frames

**Check:** Look at frame count in debug output
```
Walk: duration=0.3s (9 frames @30fps)  ← TOO SHORT!
    ⚠️  WARNING: Walk should be 30-45 frames (1-1.5s @30fps)
```

**Fix:**
1. Re-export walk animation with FULL cycle (both feet complete step)
2. Should be 30-45 frames minimum
3. Ensure animation LOOP in FBX export settings

### Problem 2: Character Slides on Ground

**Symptoms:**
- Feet slide forward/backward while walking
- Character moves but feet don't match ground speed
- Looks like ice skating

**Cause:** Root motion doesn't match animation OR root motion + code movement

**Fix Option A - Lock Root (Recommended for testing):**
```cpp
animator->SetLockRootPosition(true);  // Animation plays in place
```

**Fix Option B - Adjust Root Motion Multiplier:**
```cpp
// In test.cpp, find root motion application
float movementMultiplier = 0.15f;  // Try 0.1f, 0.2f, etc.
characterPos += moveDirection * rootMotion * movementMultiplier;
```

### Problem 3: Jump Doesn't Work

**Symptoms:**
- Press space but nothing happens
- Or jumps but stuck in jump pose
- Or jumps repeatedly while holding space

**Check:**
```
Jump: duration=0.5s (15 frames @30fps)  ← TOO SHORT!
    ⚠️  WARNING: Jump should be 30-45 frames (1-1.5s @30fps)
```

**Cause:**
- Jump animation too short (needs full arc: crouch→jump→land)
- Jump is one-shot, must complete before next jump
- FSM waiting for grounded=true

**Fix:**
1. Ensure jump animation is 30-45 frames
2. Includes takeoff, air, landing phases
3. Check "Root position locked" status

### Problem 4: Crouch Toggles Wrong

**Symptoms:**
- Press ctrl, crouches
- Press again, doesn't stand
- Or stands immediately after crouching

**Check:**
```
Crouch: duration=0.3s (9 frames @30fps)  ← TOO SHORT!
```

**Fix:**
1. Crouch should be 15-20 frames (0.5-0.7s)
2. Should crouch DOWN and HOLD that pose
3. NOT crouch down then stand up (that's two animations)

## Ideal Animation Specifications

### Walk Cycle
```
Frames: 30-45 @30fps (1-1.5 seconds)
Root Motion: ZERO (feet cycle but character stays in place)
Loop: YES (seamless loop)
Key Poses:
  - Contact (heel strike)
  - Recoil (foot flat)
  - Passing (foot mid-swing)
  - High Point (toe off)
  - Repeat for other foot
```

### Run Cycle
```
Frames: 24-36 @30fps (0.8-1.2 seconds)
Root Motion: ZERO (or very small)
Loop: YES (seamless loop)
Key Poses:
  - Contact
  - Passing
  - High Point (both feet off ground!)
  - Repeat
```

### Jump (One-Shot)
```
Frames: 30-45 @30fps (1-1.5 seconds)
Root Motion: YES (arc upward and forward)
Loop: NO (plays once)
Key Poses:
  - Anticipation (crouch down)
  - Takeoff (push off ground)
  - Air (legs tucked or running)
  - Landing (feet contact)
  - Recovery (stand up)
```

### Crouch (Looping Hold)
```
Frames: 15-20 @30fps (0.5-0.7 seconds)
Root Motion: ZERO
Loop: Partial (crouch down, then hold last frame)
Key Poses:
  - Start (standing)
  - Mid (halfway down)
  - End (crouched, HOLD this pose)
```

### Crouch Walk
```
Frames: 30-45 @30fps (1-1.5 seconds)
Root Motion: ZERO
Loop: YES
Key Poses: Same as walk but lower to ground
```

### Idle
```
Frames: 60-90 @30fps (2-3 seconds)
Root Motion: ZERO
Loop: YES
Key Poses:
  - Subtle breathing
  - Weight shift
  - Head movement
  - Arm swing (very subtle)
```

## Quick Reference

| Animation | Min Frames | Ideal Frames | Max Frames | Root Motion | Loops |
|-----------|-----------|--------------|-----------|-------------|-------|
| Idle | 40 | 60-90 | 120 | No | Yes |
| Walk | 20 | 30-45 | 60 | No | Yes |
| Run | 15 | 24-36 | 50 | No | Yes |
| Jump | 20 | 30-45 | 60 | Yes | No |
| Crouch | 10 | 15-20 | 30 | No | Partial |
| CrouchWalk | 20 | 30-45 | 60 | No | Yes |

## Testing Checklist

Run the program and verify:

- [ ] Walk animation is 30-45 frames
- [ ] Run animation is 24-36 frames
- [ ] Jump animation is 30-45 frames
- [ ] No WARNING messages in output
- [ ] Root motion status is correct for your setup
- [ ] Character walks without sliding
- [ ] Character runs without sliding
- [ ] Jump plays once and lands
- [ ] Crouch toggles on/off correctly

## How to Fix Animations in Blender/Maya

1. **Export Settings:**
   - Frame rate: 30 fps
   - Apply transform: YES
   - Bake animation: YES
   - Loop: For cyclic animations (walk, run, idle)

2. **Root Motion:**
   - For in-place: Lock root bone position
   - For root motion: Allow root to move forward

3. **Animation Length:**
   - Walk: Full cycle (both feet step)
   - Run: Full cycle (including air time)
   - Jump: Complete arc (takeoff to landing)
