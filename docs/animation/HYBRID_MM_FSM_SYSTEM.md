# Hybrid MM+FSM Animation System

## Overview

This document explains the **Hybrid Motion Matching + Finite State Machine** animation system implemented in this engine. This system combines the best of both approaches:

- **Motion Matching (MM)**: Smooth, continuous locomotion with natural transitions
- **Finite State Machine (FSM)**: Explicit control over discrete states (jump, vault, combat)
- **Layered Blending**: Upper-body actions over locomotion (attacks, gestures, aim offsets)

## Why Hybrid?

### The Problem with Pure Motion Matching

Motion Matching works excellently for **locomotion** (idle, walk, run, crouch) but has limitations:

1. **Static Root Bones**: MM relies on **root velocity** for feature matching. Animations with static roots (no root motion) all look identical to MM, causing poor matching.

2. **Complex Interactions**: Vaulting, climbing, door interactions require precise timing and event triggers that are harder to manage with pure MM.

3. **Combat**: Attack animations often need explicit state control (combo chains, hit reactions) that FSM handles better.

### The Hybrid Solution

```
┌─────────────────────────────────────────────────────────────┐
│                    HYBRID ANIMATION GRAPH                    │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────────┐         ┌──────────────────┐          │
│  │  LOCOMOTION      │         │    FSM STATES    │          │
│  │  (Motion Match)  │◄───────►│  (State Machine) │          │
│  │                  │         │                  │          │
│  │  • Idle          │         │  • Jump/Fall     │          │
│  │  • Walk          │         │  • Vault         │          │
│  │  • Run           │         │  • Combat        │          │
│  │  • Crouch        │         │  • Interaction   │          │
│  └──────────────────┘         └──────────────────┘          │
│           │                              │                   │
│           └──────────┬───────────────────┘                   │
│                      │                                       │
│              ┌───────▼────────┐                              │
│              │ LAYER SYSTEM   │                              │
│              │                │                              │
│              │ • Upper Body   │                              │
│              │ • Additive     │                              │
│              │ • Full Override│                              │
│              └────────────────┘                              │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

## Architecture

### Files Created

| File | Description |
|------|-------------|
| `animationSystem/HybridAnimGraph.h` | Main hybrid system header |
| `animationSystem/HybridAnimGraph.cpp` | Main hybrid system implementation |
| `animationSystem/AnimationLayerSystem.h` | Layered blending header |
| `animationSystem/AnimationLayerSystem.cpp` | Layered blending implementation |
| `motionMatching/MotionMatcher.h` (updated) | Added database switching & static root detection |
| `motionMatching/MotionMatcher.cpp` (updated) | Added static root fallback |
| `tests/test_hybrid_animation.cpp` | Unit tests |

### Key Classes

#### 1. `HybridAnimGraph`

The main entry point for the hybrid system.

```cpp
// Initialize
HybridAnimGraph graph;
graph.Initialize(skeleton, animator);

// Load animations
graph.LoadLocomotionAnimation("Walk", walkAnim);  // For MM
graph.LoadStateAnimation("Jump", jumpAnim, HybridState::LOCOMOTION_AIR);  // For FSM
graph.LoadUpperBodyAnimation("Punch", punchAnim, upperBodyMask);  // For layering

// Build databases
graph.BuildDatabases();

// Each frame
graph.Update(dt, characterState);
```

#### 2. `AnimationLayerSystem`

Manages layered animation blending.

```cpp
// Initialize
AnimationLayerSystem layerSystem;
layerSystem.Initialize(skeleton);

// Add layers
layerSystem.AddLayer("Attack", attackAnim,
                     LayerBlendMode::LINEAR,
                     BoneMaskPreset::UPPER_BODY);

layerSystem.AddLayer("AimOffset", aimAnim,
                     LayerBlendMode::ADDITIVE,
                     BoneMaskPreset::ARMS_ONLY);

// Control weights
layerSystem.SetLayerWeight("Attack", 1.0f, 0.1f);
layerSystem.FadeOutLayer("AimOffset", 0.2f);

// Update
layerSystem.Update(dt);
layerSystem.ApplyToAnimator(animator);
```

#### 3. `MotionMatcher` (Enhanced)

Now supports database switching and static root fallback.

```cpp
// Check if database is suitable for MM
if (!matcher.IsDatabaseValidForMM()) {
    // Fall back to FSM for this animation set
}

// Switch databases (e.g., walk → crouch)
matcher.SetDatabase(crouchDatabase, 0.2f);

// Check if current pose has static root
if (matcher.HasStaticRoot()) {
    // MM will automatically fall back to simple playback
}
```

## Usage Guide

### Step 1: Setup

```cpp
// In your character initialization
void Character::Initialize() {
    // Create systems
    hybridGraph = std::make_unique<HybridAnimGraph>();
    hybridGraph->Initialize(skeleton, animator);

    layerSystem = std::make_unique<AnimationLayerSystem>();
    layerSystem->Initialize(skeleton);

    // Load locomotion animations (for Motion Matching)
    hybridGraph->LoadLocomotionAnimation("Idle", idleAnim);
    hybridGraph->LoadLocomotionAnimation("Walk", walkAnim);
    hybridGraph->LoadLocomotionAnimation("Run", runAnim);
    hybridGraph->LoadLocomotionAnimation("CrouchWalk", crouchWalkAnim);

    // Load state animations (for FSM)
    hybridGraph->LoadStateAnimation("Jump", jumpAnim, HybridState::LOCOMOTION_AIR);
    hybridGraph->LoadStateAnimation("Fall", fallAnim, HybridState::LOCOMOTION_AIR);
    hybridGraph->LoadStateAnimation("Vault", vaultAnim, HybridState::VAULTING);

    // Load upper-body actions (for layering)
    std::vector<bool> upperBodyMask = CreateUpperBodyMask();
    hybridGraph->LoadUpperBodyAnimation("Punch", punchAnim, upperBodyMask);
    hybridGraph->LoadUpperBodyAnimation("Reload", reloadAnim, upperBodyMask);

    // Load additive animations
    hybridGraph->LoadAdditiveAnimation("AimOffset", aimAnim);

    // Build databases
    hybridGraph->BuildDatabases();
}
```

### Step 2: Update Loop

```cpp
void Character::Update(float dt) {
    // Gather character state
    HybridCharacterState state;
    state.position = body->position;
    state.velocity = body->velocity;
    state.rotation = body->rotation;
    state.moveDirection = input.moveDirection;
    state.moveMagnitude = input.moveMagnitude;
    state.grounded = physics->isGrounded();
    state.jumping = input.jumpPressed;
    state.crouching = input.crouchHeld;
    state.sprinting = input.sprintHeld;
    state.attackPressed = input.attackPressed;
    state.interactPressed = input.interactPressed;

    // Update hybrid graph
    hybridGraph->Update(dt, state);

    // Play upper-body actions
    if (input.attackPressed) {
        hybridGraph->PlayUpperBodyAction("Punch", 1.0f, false);
    }

    // Update layer system
    layerSystem->Update(dt);
    layerSystem->ApplyToAnimator(animator);
}
```

### Step 3: Custom Transitions

```cpp
// Add custom transition rules
hybridGraph->AddTransition(
    HybridState::LOCOMOTION_GROUNDED,
    HybridState::COMBAT,
    0.1f,
    [this]() { return characterState.attackPressed; }
);

hybridGraph->AddTransition(
    HybridState::LOCOMOTION_GROUNDED,
    HybridState::INTERACTION,
    0.3f,
    [this]() { return characterState.interactPressed && CanInteract(); }
);
```

## State Machine Design

### Default States

| State | Description | Animation Type |
|-------|-------------|----------------|
| `LOCOMOTION_GROUNDED` | Walking, running, idle | Motion Matching |
| `LOCOMOTION_AIR` | Jumping, falling | FSM (static root) |
| `LOCOMOTION_CROUCH` | Crouching movement | Motion Matching (crouch database) |
| `VAULTING` | Vaulting obstacles | FSM (full body) |
| `COMBAT` | Combat stance, attacks | FSM + Layers |
| `INTERACTION` | Door open, pickup | FSM (full body) |
| `CUSTOM` | User-defined | User-defined |

### Transition Rules

Transitions are evaluated every frame. When a condition is true, the system blends from the current state to the target state.

```cpp
// Example: Grounded ↔ Air transitions
AddTransition(LOCOMOTION_GROUNDED, LOCOMOTION_AIR, 0.15f,
    []() { return !state.grounded && state.jumping; });

AddTransition(LOCOMOTION_AIR, LOCOMOTION_GROUNDED, 0.1f,
    []() { return state.grounded && !state.falling; });

// Example: Grounded ↔ Crouch
AddTransition(LOCOMOTION_GROUNDED, LOCOMOTION_CROUCH, 0.2f,
    []() { return state.crouching; });

AddTransition(LOCOMOTION_CROUCH, LOCOMOTION_GROUNDED, 0.2f,
    []() { return !state.crouching; });
```

## Layer System

### Blend Modes

| Mode | Description | Use Case |
|------|-------------|----------|
| `LINEAR` | Standard blending (A*w + B*(1-w)) | Upper-body actions |
| `ADDITIVE` | Additive (Base + Additive*w) | Aim offsets, recoil |
| `MULTIPLICATIVE` | Multiplicative blending | Advanced effects |
| `PROJECTION` | Projection blending | Look-at IK |

### Bone Mask Presets

| Preset | Affected Bones |
|--------|----------------|
| `FULL_BODY` | All bones |
| `UPPER_BODY` | Arms, shoulders, spine, head |
| `LOWER_BODY` | Legs, hips, lower spine |
| `LEFT_ARM` | Left arm, forearm, hand |
| `RIGHT_ARM` | Right arm, forearm, hand |
| `HEAD_ONLY` | Head, neck |
| `ARMS_ONLY` | Both arms, shoulders |
| `CUSTOM` | User-defined mask |

### Example: Combat Layering

```cpp
// Base layer: MM locomotion (handled by HybridAnimGraph)

// Layer 1: Upper-body attack
layerSystem->AddLayer("Attack", punchAnim,
                     LayerBlendMode::LINEAR,
                     BoneMaskPreset::UPPER_BODY);

// Layer 2: Additive aim offset
layerSystem->AddLayer("AimOffset", aimAnim,
                     LayerBlendMode::ADDITIVE,
                     BoneMaskPreset::ARMS_ONLY);

// Layer 3: Head look-at (procedural)
layerSystem->AddLayer("LookAt", nullptr,
                     LayerBlendMode::PROJECTION,
                     BoneMaskPreset::HEAD_ONLY);

// Control weights during combat
if (isAiming) {
    layerSystem->SetLayerWeight("AimOffset", 1.0f, 0.2f);
} else {
    layerSystem->SetLayerWeight("AimOffset", 0.0f, 0.2f);
}
```

## Static Root Handling

### Why MM Doesn't Work with Static Roots

Motion Matching searches for poses based on **feature similarity**:

```
Feature Vector = [rootVelocity, speed, moveDirection, footPlant, ...]
```

When root velocity is always zero (static root), all poses look identical to the search algorithm, causing:
- Random pose selection
- Popping artifacts
- No meaningful matching

### Hybrid Solution

The enhanced `MotionMatcher` now:

1. **Detects static roots** automatically
2. **Falls back to FSM-style playback** for static animations
3. **Validates databases** before using them for MM

```cpp
// Check if database is suitable for MM
if (!matcher.IsDatabaseValidForMM()) {
    std::cout << "WARNING: Database has mostly static roots.\n";
    std::cout << "Consider using FSM for these animations.\n";
}

// MM automatically falls back to simple playback for static roots
matcher.SetStaticRootFallback(true);  // Enabled by default
```

### Best Practices

| Animation Type | Root Motion | Recommended System |
|----------------|-------------|-------------------|
| Idle | Static | FSM or MM with fallback |
| Walk/Run | Dynamic | Motion Matching |
| Jump | Dynamic (vertical) | FSM (explicit control) |
| Fall | Static | FSM |
| Attack | Static/Dynamic | Layer over MM |
| Vault | Dynamic | FSM (full body) |
| Interaction | Static | FSM (full body) |

## Database Switching

Switch between different MM databases for different contexts:

```cpp
// Create separate databases
MotionDatabase walkDatabase;
MotionDatabase crouchDatabase;
MotionDatabase combatDatabase;

// Load animations into each
walkDatabase.AddAnimation("Walk", walkAnim, skeleton);
walkDatabase.AddAnimation("Run", runAnim, skeleton);

crouchDatabase.AddAnimation("CrouchWalk", crouchWalkAnim, skeleton);
crouchDatabase.AddAnimation("CrouchIdle", crouchIdleAnim, skeleton);

// Switch when crouching
if (isCrouching) {
    matcher.SetDatabase(crouchDatabase, 0.2f);  // Blend over 0.2s
} else {
    matcher.SetDatabase(walkDatabase, 0.2f);
}
```

## Debugging

### Enable Debug Mode

```cpp
hybridGraph->SetDebugEnabled(true);
hybridGraph->PrintDebugInfo();

layerSystem->SetDebugEnabled(true);
layerSystem->PrintDebugInfo();

matcher.SetDebugEnabled(true);
matcher.PrintDebugInfo();
```

### Sample Output

```
=== HYBRID ANIMATION GRAPH ===
Current State: Locomotion_Grounded
Previous State: Locomotion_Grounded
Transitioning: NO
Current Database: Primary
Motion Matching: ENABLED
Upper-Body Layers: 1
Additive Layers: 1

=== MOTION MATCHING DEBUG ===
Current pose: 42
Animation: Walk @ 0.523s
Search: 150 poses, 0.03ms
Score: 0.142
Feet: L=PLANTED R=FREE

=== LAYER SYSTEM STATE ===
Layers: 2 / 8
Active Layers:
  - Attack
    Weight: 1.000000 (target: 1.000000)
    Time: 0.234000 / 0.500000
    Blend Mode: 0
    Priority: 0
  - AimOffset
    Weight: 0.500000 (target: 0.500000)
    Time: 1.234000 / 2.000000
    Blend Mode: 1
    Priority: 0
```

## Performance Considerations

### Memory

- Each MM database stores all pose samples (~100-1000 poses per animation)
- Layer system maintains per-bone masks
- Typical memory: 5-20 MB for full character setup

### CPU

- MM search: O(log n) with KD-Tree (~0.01-0.1ms)
- Layer blending: O(numLayers * numBones) (~0.05-0.2ms)
- Total animation cost: ~0.1-0.5ms per character

### Optimization Tips

1. **Use MM only for locomotion** - FSM for complex states
2. **Limit active layers** - Max 4-8 layers per character
3. **Share databases** - Multiple characters can share the same MM database
4. **LOD** - Reduce layer count for distant characters

## Migration Guide

### From Pure FSM

```cpp
// Old FSM code
animationStateMachine->update(dt, input);

// New hybrid code
hybridGraph->Update(dt, characterState);
layerSystem->Update(dt);
layerSystem->ApplyToAnimator(animator);
```

### From Pure MM

```cpp
// Old MM code
motionMatcher->Update(dt, characterState);

// New hybrid code (MM for locomotion, FSM for rest)
hybridGraph->Update(dt, characterState);
```

## Common Use Cases

### 1. Locomotion + Combat

```cpp
// MM handles walking/running
// FSM handles combat state
// Layer handles attacks

if (inCombat) {
    hybridGraph->PlayUpperBodyAction("Attack", 1.0f);
}
```

### 2. Locomotion + Interaction

```cpp
// FSM takes over for full-body interaction
if (canInteract && input.interactPressed) {
    // Transition to interaction state
    // MM is temporarily disabled
}
```

### 3. Locomotion + Aim Offset

```cpp
// Additive layer for aim offset
layerSystem->AddLayer("AimOffset", aimAnim,
                     LayerBlendMode::ADDITIVE,
                     BoneMaskPreset::ARMS_ONLY);

// Update aim offset based on input
float aimWeight = isAiming ? 1.0f : 0.0f;
layerSystem->SetLayerWeight("AimOffset", aimWeight, 0.2f);
```

## Testing

Run the unit tests:

```bash
make test
# or
ctest --verbose
```

Key tests:
- `HybridAnimGraph_Initialize` - System initialization
- `HybridAnimGraph_StateTransitions` - FSM state changes
- `LayerSystem_AddLayer` - Layer management
- `MotionMatcher_StaticRootDetection` - Static root handling
- `HybridSystem_Integration` - Full system integration

## Troubleshooting

### Problem: MM not matching correctly

**Solution**: Check if animations have root motion:
```cpp
if (!matcher.IsDatabaseValidForMM()) {
    // Use FSM instead
}
```

### Problem: Layer not blending

**Solution**: Check bone mask and weight:
```cpp
layerSystem->SetLayerWeight("MyLayer", 1.0f, 0.1f);
```

### Problem: State transitions popping

**Solution**: Increase blend duration:
```cpp
hybridGraph->AddTransition(from, to, 0.3f, condition);  // Longer blend
```

## Future Enhancements

- [ ] Procedural IK integration (foot IK, hand IK)
- [ ] Motion warping for alignment
- [ ] Compressed pose storage
- [ ] GPU skinning integration
- [ ] Animation graph visual editor
