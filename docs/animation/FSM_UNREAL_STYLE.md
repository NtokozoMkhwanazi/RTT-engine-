# Unreal-Style Animation State Machine

## Problem Analysis

The FSM kept **switching states automatically** because:
1. No **exit rules** - states could be exited any frame
2. No **state locking** - one-shot animations (jump) could be interrupted
3. No **priority system** - all transitions evaluated equally
4. **Level-based** input instead of **edge-based**

## Unreal Engine Approach

Unreal uses a **rule-based state machine** with:

```
┌─────────────────────────────────────────────────────────────┐
│                    STATE MACHINE RULES                       │
├─────────────────────────────────────────────────────────────┤
│ 1. ENTRY RULES: Conditions to ENTER a state                 │
│ 2. EXIT RULES: Conditions to EXIT a state (state locking)   │
│ 3. TRANSITION RULES: Priority-based transition selection    │
│ 4. BLEND RULES: How to blend between states                 │
└─────────────────────────────────────────────────────────────┘
```

## Implementation

### State Locking (Exit Rules)

Some states **cannot be exited** until conditions are met:

```cpp
// JUMP: Locked until animation completes AND grounded
if (currentState == JUMP) {
    if (jumpAnimationPlaying && jumpAnimationStartTime < minDuration) {
        canExitCurrentState = false;  // LOCKED: Animation playing
    }
    if (!grounded) {
        canExitCurrentState = false;  // LOCKED: Still in air
    }
}

// CROUCH: Locked while crouch button held (toggle behavior)
if (currentState == CROUCH || currentState == CROUCH_WALK) {
    if (crouching && !crouchJustReleased && !crouchJustPressed) {
        canExitCurrentState = false;  // LOCKED: Crouch held
    }
}
```

### Priority-Based Transitions

Transitions evaluated in **priority order**:

```
PRIORITY 1: Jump (highest - interruptible action)
PRIORITY 2: Crouch toggle (explicit player choice)
PRIORITY 3: Movement states (walk/run based on input)
PRIORITY 4: Crouch movement (sub-state of crouch)
PRIORITY 5: Idle (default when no input)
```

### Edge Detection

All transitions triggered by **key press/release events**:

```cpp
bool jumpJustPressed = jumping && !prevJump;       // Space DOWN
bool crouchJustPressed = crouching && !prevCrouch; // Ctrl DOWN
bool crouchJustReleased = !crouching && prevCrouch;// Ctrl UP
bool movingJustStarted = isMoving && !prevMoving;  // W/A/S/D DOWN
bool movingJustStopped = !isMoving && prevMoving;  // W/A/S/D UP
```

## State Transition Diagram

```
                              ┌──────────────────────────────┐
                              │         JUMP (Locked)        │
                              │  Exit: Animation complete +  │
                              │         Grounded             │
                              └──────────────┬───────────────┘
                                             │
                                             │ Jump pressed (grounded)
                                             │
┌─────────────┐    W pressed    ┌───────────┴────────────┐    Ctrl pressed    ┌──────────────┐
│             │────────────────▶│      MOVEMENT          │───────────────────▶│              │
│    IDLE     │◀───────────────│  (Walk/Run based on    │◀───────────────────│   CROUCH     │
│             │   W released    │       sprint)          │   Ctrl released    │              │
└─────────────┘                 └───────────┬────────────┘                    └──────────────┘
                                            │                                          │
                                            │ W pressed + Ctrl held                    │ W pressed
                                            ▼                                          ▼
                                   ┌────────────────┐                        ┌──────────────────┐
                                   │   CROUCH_WALK  │◀───────────────────────│   (Crouch held)  │
                                   └────────────────┘                        └──────────────────┘
```

## State Behaviors

### IDLE
- **Entry**: No input, movement stopped
- **Exit**: Movement key pressed, jump pressed, crouch pressed
- **Locked**: No
- **Animation**: Looping idle

### WALK
- **Entry**: W/A/S/D pressed (not sprinting)
- **Exit**: Keys released (→Idle), sprint pressed (→Run)
- **Locked**: No
- **Animation**: Looping walk, speed-matched

### RUN
- **Entry**: W/A/S/D + Shift pressed
- **Exit**: Shift released (→Walk), keys released (→Idle)
- **Locked**: No
- **Animation**: Looping run

### JUMP
- **Entry**: Space pressed (grounded, not crouching)
- **Exit**: Animation complete + grounded
- **Locked**: YES - cannot exit until conditions met
- **Animation**: Non-looping jump sequence

### CROUCH
- **Entry**: Ctrl pressed (toggle on)
- **Exit**: Ctrl pressed/released (toggle off)
- **Locked**: YES - stays while crouch held
- **Animation**: Looping crouch idle

### CROUCH_WALK
- **Entry**: Moving while crouching
- **Exit**: Stop moving (→Crouch), release crouch (→Idle/Walk)
- **Locked**: No (sub-state of crouch)
- **Animation**: Looping crouch walk

## Key Press Scenarios

### Jump Sequence
```
Frame 1: Press Space
  → jumpJustPressed = true
  → targetState = JUMP
  → jumpAnimationPlaying = true
  → jumpAnimationStartTime = 0

Frame 2-20: Holding Space, in air
  → canExitCurrentState = false (animation playing < 0.3s)
  → Stay in JUMP

Frame 21-60: Still in air
  → canExitCurrentState = false (!grounded)
  → Stay in JUMP

Frame 61: Landed, animation complete
  → canExitCurrentState = true
  → grounded = true, !jumping
  → targetState = IDLE
  → jumpAnimationPlaying = false
```

### Crouch Toggle
```
Frame 1: Press Ctrl
  → crouchJustPressed = true
  → currentState = IDLE → targetState = CROUCH
  → Transition: IDLE → CROUCH

Frame 2-30: Holding Ctrl
  → canExitCurrentState = false (crouching && !release && !press)
  → Stay in CROUCH

Frame 31: Press Ctrl again (toggle off)
  → crouchJustPressed = true
  → currentState = CROUCH → targetState = IDLE
  → Transition: CROUCH → IDLE
```

### Walk → Run Transition
```
Frame 1: Press W (not sprinting)
  → movingJustStarted = true
  → targetState = WALK
  → Transition: IDLE → WALK

Frame 2-60: Holding W
  → isMoving = true, !movingJustStarted
  → Stay in WALK

Frame 61: Press Shift
  → sprintJustPressed = true
  → currentState = WALK → targetState = RUN
  → Transition: WALK → RUN

Frame 62-120: Holding W+Shift
  → isMoving = true, sprinting = true
  → Stay in RUN

Frame 121: Release Shift
  → sprintJustReleased = true
  → currentState = RUN → targetState = WALK
  → Transition: RUN → WALK
```

## Debug Output

Press **H** in-game:
```
[FSM] State=Run Speed=0.85 Grounded=1 Jump=0 Crouch=0 Sprint=1 canExit=1 jumpPlaying=0 jumpTime=0
```

| Field | Meaning |
|-------|---------|
| `State` | Current animation state |
| `Speed` | Movement magnitude (0-1) |
| `Grounded` | Is character on ground |
| `canExit` | Can current state be exited |
| `jumpPlaying` | Is jump animation playing |
| `jumpTime` | Time since jump started |

## Files Changed

- `animationSystem/AnimationStateMachine.h`
  - Added `jumpAnimationPlaying` flag
  - Added `jumpAnimationStartTime` timer
  
- `animationSystem/AnimationStateMachine.cpp`
  - Complete rewrite of `update()` function
  - Added exit rule evaluation
  - Added priority-based transition selection
  - Added state locking for jump/crouch

## Testing Checklist

- [ ] **Jump**: Press space → jumps, cannot cancel mid-air, lands → idle
- [ ] **Crouch Toggle**: Press ctrl → crouch, press again → stand
- [ ] **Walk**: Press W → walk, release → idle
- [ ] **Run**: Press W+Shift → run, release Shift → walk
- [ ] **Crouch Walk**: Crouch + W → crouch walk, release W → crouch
- [ ] **No Auto-Switching**: States don't change without input

## Comparison: Before vs After

| Aspect | Before | After |
|--------|--------|-------|
| **State Changes** | Every frame based on input | Only on key edges |
| **Jump** | Could cancel anytime | Locked until complete |
| **Crouch** | Hold-based | Toggle-based |
| **Walk/Run** | Auto-switched | Explicit transitions |
| **Priority** | None (all equal) | Jump > Crouch > Move > Idle |
| **Exit Rules** | None | State-specific locking |

## Verification

All 196 unit tests pass:
```
[==========] 196 tests from 9 test suites ran.
[  PASSED  ] 196 tests.
All tests passed!
```
