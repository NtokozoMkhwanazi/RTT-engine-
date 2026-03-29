# Gradient Band Interpolation (Blend Space)

## Overview

**Gradient Band Interpolation** (also known as **Blend Space** or **Motion Matching**) is the technique AAA games use for smooth locomotion animation. Instead of discrete state transitions (idle→walk→run with popping), we continuously blend animations based on movement speed.

## How It Works

### Traditional Discrete States (OLD)
```
Speed:     0.0    0.5    1.0
State:     [IDLE] [WALK] [RUN]
           ───┬───  ──┬──  ──┬──
              │       │      │
              ▼       ▼      ▼
           POP!    POP!   POP!
```

### Gradient Band Interpolation (NEW)
```
Speed:     0.0 ───── 0.5 ───── 1.0
Blend:     IDLE ←→ WALK ←→ RUN
           Smooth gradient, no popping!
```

## Implementation

### Blend Weight Calculation

```cpp
// Normalize speed to blend weight (0.0 to 1.0)
float normalizedSpeed = movementSpeed / maxRunSpeed;
targetBlendWeight = clamp(normalizedSpeed, 0.0, 1.0);

// Smooth blend weight transition
blendWeight = mix(blendWeight, targetBlendWeight, blendSmoothRate * dt);
```

### Blend Bands

| Speed Range | Blend Weight | Animation Mix |
|-------------|--------------|---------------|
| 0.0 - 0.3   | 0.0 - 0.25   | 100% Idle |
| 0.3 - 0.6   | 0.25 - 0.5   | Idle ↔ Walk |
| 0.6 - 1.0   | 0.5 - 1.0    | Walk ↔ Run |

### Animation Blending

```cpp
if (blendWeight < 0.5f) {
    // Blending idle ↔ walk
    float idleWeight = 1.0 - (blendWeight * 2.0);
    float walkWeight = blendWeight * 2.0;
    BlendTwoAnimations(idle, idleWeight, walk, walkWeight);
} else {
    // Blending walk ↔ run
    float walkWeight = 2.0 - (blendWeight * 2.0);
    float runWeight = (blendWeight - 0.5f) * 2.0;
    BlendTwoAnimations(walk, walkWeight, run, runWeight);
}
```

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│              GRADIENT BAND INTERPOLATION                 │
├─────────────────────────────────────────────────────────┤
│                                                          │
│  Input: movementSpeed (0.0 to maxRunSpeed)              │
│                                                          │
│  ┌──────────────┐                                       │
│  │ Normalize    │ → normalizedSpeed (0-1)              │
│  └──────────────┘                                       │
│         ↓                                                │
│  ┌──────────────┐                                       │
│  │ Smooth       │ → blendWeight (lerped)               │
│  └──────────────┘                                       │
│         ↓                                                │
│  ┌──────────────┐                                       │
│  │ Weight Map   │ → idle/walk/run weights              │
│  └──────────────┘                                       │
│         ↓                                                │
│  ┌──────────────┐                                       │
│  │ Blend        │ → Final pose                         │
│  └──────────────┘                                       │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

## Configuration

### Enable/Disable
```cpp
stateMachine->setBlendSpaceEnabled(true);  // Enable gradient band interpolation
```

### Blend Band Thresholds
```cpp
stateMachine->setBlendBands(0.3f, 0.6f);
// 0.3 = speed where idle→walk blend starts
// 0.6 = speed where walk→run blend starts
```

### Blend Smooth Rate
```cpp
stateMachine->blendSmoothRate = 10.0f;  // Higher = faster blend transitions
```

## Benefits

| Aspect | Discrete States | Gradient Bands |
|--------|----------------|----------------|
| **Transitions** | Popping | Smooth |
| **Foot sliding** | Visible at boundaries | Minimized |
| **Animation count** | 3 separate | 3 blended |
| **Memory** | Load all | Load all |
| **CPU cost** | Low | Medium (blending) |
| **Visual quality** | Good | Excellent (AAA) |

## State Machine Integration

The gradient band system works alongside the existing state machine:

- **Locomotion (idle/walk/run)**: Handled by blend space
- **One-shot actions (jump)**: Discrete state with locking
- **Special states (crouch)**: Discrete state with toggle

```
                    ┌─────────────────┐
                    │  JUMP (Locked)  │
                    └────────┬────────┘
                             │
                    ┌────────▼────────┐
                    │  CROUCH (Toggle)│
                    └────────┬────────┘
                             │
              ┌──────────────┼──────────────┐
              │              │              │
       ┌──────▼──────┐ ┌────▼────┐ ┌──────▼──────┐
       │    IDLE     │ │  WALK   │ │     RUN     │
       │  (blend=0)  │ │(blend=0.5)│ │  (blend=1)  │
       └─────────────┘ └─────────┘ └─────────────┘
              ▲              ▲              ▲
              └──────────────┴──────────────┘
                   Gradient Band Blending
```

## Debug Output

Press **H** in-game:
```
[FSM] State=Walk Speed=0.45 Grounded=1 BlendWeight=0.38 TargetBlend=0.45 canExit=1
```

| Field | Meaning |
|-------|---------|
| `BlendWeight` | Current blend (0=idle, 0.5=walk, 1=run) |
| `TargetBlend` | Target blend weight |
| `State` | Dominant state based on blend weight |

## Tuning Guide

### For smoother transitions:
```cpp
blendSmoothRate = 5.0f;   // Slower blend (more smoothing)
idleToWalkThreshold = 0.2f;  // Earlier walk blend
walkToRunThreshold = 0.8f;   // Later run blend
```

### For snappier response:
```cpp
blendSmoothRate = 15.0f;  // Faster blend
idleToWalkThreshold = 0.4f;  // Later walk blend
walkToRunThreshold = 0.5f;   // Earlier run blend
```

### For different character speeds:
```cpp
maxWalkSpeed = 3.0f;   // Walk speed threshold
maxRunSpeed = 8.0f;    // Run speed threshold
```

## Files Changed

- `animationSystem/AnimationStateMachine.h`
  - Added blend space members (`useBlendSpace`, `blendWeight`, etc.)
  - Added `setBlendSpaceEnabled()`, `setBlendBands()`
  - Added `applyBlendSpaceAnimation()` method

- `animationSystem/AnimationStateMachine.cpp`
  - Rewrote `update()` to use gradient bands
  - Implemented `applyBlendSpaceAnimation()`

- `animationSystem/Animator.h`
  - Added `BlendTwoAnimations()` method

- `animationSystem/Animator.cpp`
  - Implemented `BlendTwoAnimations()`

- `test.cpp`
  - Enabled blend space: `setBlendSpaceEnabled(true)`
  - Configured blend bands: `setBlendBands(0.3f, 0.6f)`

## Verification

All 196 unit tests pass:
```
[==========] 196 tests from 9 test suites ran.
[  PASSED  ] 196 tests.
All tests passed!
```

## References

- Unreal Engine Blend Spaces
- Unity Animator Blend Trees
- Motion Matching (EA, Ubisoft)
- Gradient Band Interpolation (GDC talks)
