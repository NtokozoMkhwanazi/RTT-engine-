# Animation FSM Fix - Edge-Based State Transitions

## Problem

The animation FSM was **switching states automatically** instead of waiting for key presses:
- Walking → Running happened automatically when holding W+Shift
- States changed based on **level** (is button held) instead of **edge** (button just pressed)
- No control over state transitions - felt like the FSM was playing itself

## Root Cause

The FSM used **level-based detection**:
```cpp
// WRONG: Checks if button IS pressed (level)
if (sprinting) {  // True EVERY frame shift is held
    targetState = RUN;
}
```

This caused the FSM to evaluate states every frame based on current input state, rather than responding to **key press events**.

## Solution: Edge-Based Detection

Changed to **edge-based detection** - states only change on key PRESS or RELEASE:

```cpp
// CORRECT: Checks if button JUST GOT pressed (edge)
bool sprintJustPressed = sprinting && !prevSprinting;  // True ONLY on press frame

if (sprintJustPressed && currentState == WALK) {  // Only on shift press
    targetState = RUN;
}
```

### Edge Detection Logic

| Edge Type | Detection | Trigger |
|-----------|-----------|---------|
| **Just Pressed** | `current && !previous` | Key down this frame |
| **Just Released** | `!current && previous` | Key up this frame |

## New FSM Behavior

### Movement States
| Input | Old Behavior | New Behavior |
|-------|-------------|--------------|
| Press W | Walk starts | Walk starts **once** on press |
| Hold W | Stays in walk | **Stays** in walk (no re-trigger) |
| Release W | Idle | Idle **once** on release |
| Press W+Shift | Run starts | Run starts **once** on press |
| Release Shift | Walk | Walk **once** on shift release |

### Jump
| Input | Old Behavior | New Behavior |
|-------|-------------|--------------|
| Press Space | Jumps | Jumps **once** on press |
| Hold Space | Still jumping (animation) | Animation plays through |
| Release Space | May re-trigger | No effect (already jumped) |
| Land | May auto-fall | Returns to idle |

### Crouch
| Input | Old Behavior | New Behavior |
|-------|-------------|--------------|
| Press Ctrl | Crouch | **Toggle**: Idle ↔ Crouch |
| Hold Ctrl + W | Crouch walk | Crouch walk |
| Release Ctrl | Stand | **Toggle**: Stand up |

## State Transition Flow

```
IDLE
  ↓ (Press W)
WALK
  ↓ (Press Shift)
RUN
  ↓ (Release Shift)
WALK
  ↓ (Release W)
IDLE

IDLE
  ↓ (Press Space)
JUMP
  ↓ (Land + Release Space)
IDLE

IDLE
  ↓ (Press Ctrl)
CROUCH
  ↓ (Press W)
CROUCH_WALK
  ↓ (Release W)
CROUCH
  ↓ (Press Ctrl)
IDLE
```

## Code Changes

### AnimationStateMachine.h
Added `prevCrouch` tracking:
```cpp
bool prevCrouch = false;  // Track crouch previous state
```

### AnimationStateMachine.cpp
Edge detection for all inputs:
```cpp
bool movingJustStarted = isMoving && !prevMoving;
bool movingJustStopped = !isMoving && prevMoving;
bool sprintJustPressed = sprinting && !prevSprinting;
bool sprintJustReleased = !sprinting && prevSprinting;
bool jumpJustPressed = jumping && !prevJump;

// Update previous states
prevMoving = isMoving;
prevSprinting = sprinting;
prevJump = jumping;
prevCrouch = crouching;
```

State transitions on edges only:
```cpp
// JUMP: Only on press
if (jumpJustPressed && grounded && !crouching) {
    targetState = JUMP;
}

// MOVEMENT: Only on movement start
else if (movingJustStarted && grounded && !crouching) {
    targetState = sprinting ? RUN : WALK;
}

// SPRINT: Only on shift press/release
else if (sprintJustPressed && currentState == WALK) {
    targetState = RUN;
}
else if (sprintJustReleased && currentState == RUN) {
    targetState = WALK;
}

// IDLE: Only on movement stop
else if (movingJustStopped) {
    targetState = IDLE;
}
```

## Testing

### Test Sequence
1. **Press W** → Should transition to WALK **once**
2. **Hold W** → Should stay in WALK (no re-triggering)
3. **Press Shift** → Should transition to RUN **once**
4. **Hold Shift+W** → Should stay in RUN
5. **Release Shift** → Should transition to WALK **once**
6. **Release W** → Should transition to IDLE **once**
7. **Press Space** → Should JUMP **once**
8. **Land** → Should return to IDLE
9. **Press Ctrl** → Should CROUCH **once**
10. **Press Ctrl again** → Should return to IDLE (toggle)

### Debug Output
Press **H** in-game to see edge detection:
```
[FSM] spd=0.85 grounded=1 jump=0 sprint=1 crouch=0
       [EDGES] moveStart=0 moveStop=0 sprintPress=1 jumpPress=0 crouch=1 prevCrouch=0
```

- `moveStart=1` → W/A/S/D just pressed
- `sprintPress=1` → Shift just pressed
- `jumpPress=1` → Space just pressed
- `crouch=1, prevCrouch=0` → Ctrl just pressed

## Benefits

1. **Predictable**: States change only when you press keys
2. **Controlled**: No automatic state switching
3. **Responsive**: Immediate response to key presses
4. **Clean**: No rapid state flipping when holding keys
5. **AAA Quality**: Matches professional game feel

## Files Changed

- `animationSystem/AnimationStateMachine.h` - Added `prevCrouch` member
- `animationSystem/AnimationStateMachine.cpp` - Edge-based state transitions

## Verification

All 196 unit tests pass:
```
[==========] 196 tests from 9 test suites ran.
[  PASSED  ] 196 tests.
All tests passed!
```
