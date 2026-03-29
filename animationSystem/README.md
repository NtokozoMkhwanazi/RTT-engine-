# Animation System

Advanced character animation with hybrid FSM + Motion Matching, GPU skinning, and inverse kinematics.

## 📁 Files

```
animationSystem/
├── Animation.h               # Animation data structures
├── Animation.cpp             # Animation implementation
├── AnimationConfig.h         # Configuration settings
├── AnimationLayerSystem.h    # Animation layering
├── AnimationLayerSystem.cpp  # Layer system implementation
├── AnimationRetargeting.h    # Skeleton retargeting
├── AnimationRetargeting.cpp  # Retargeting implementation
├── AnimationStateMachine.h   # State machine
├── AnimationStateMachine.cpp # FSM implementation
├── AnimationTypes.h          # Type definitions
├── Animator.h                # Main animator class
├── Animator.cpp              # Animator implementation
├── AssimpAnimationLoader.h   # FBX/animation loading
├── AssimpAnimationLoader.cpp # Loader implementation
├── BoneMatrixBuffer.h        # GPU bone storage
├── BoneMatrixBuffer.cpp      # Buffer implementation
├── HybridAnimGraph.h         # Animation graph
├── HybridAnimGraph.cpp       # Graph implementation
├── HybridMMFSM.h             # Hybrid FSM + MM
├── HybridMMFSM.cpp           # Hybrid implementation
├── SkeletonRetargeter.h      # Skeleton retargeting
├── SkeletonRetargeter.cpp    # Retargeter implementation
└── README.md                 # This file
```

## 🎭 Features

### Animation Playback
- **FBX Import** - Load animations from FBX files
- **Animation Blending** - Smooth transitions between animations
- **Layer System** - Multiple animation layers (base, upper body, etc.)
- **State Machines** - FSM-based animation control
- **Retargeting** - Apply animations to different skeletons

### Motion Matching
- **Hybrid System** - FSM + Motion Matching
- **Trajectory Prediction** - Predict future movement
- **Foot Planting IK** - Accurate foot placement
- **KD-Tree Search** - Fast motion database queries
- **LOD for Animations** - Distance-based quality

### GPU Skinning
- **Bone Matrix Buffer** - UBO/SSBO for bone matrices
- **Vertex Shader Skinning** - GPU-accelerated skinning
- **Texture Storage** - Bone matrices in textures for many bones

### Inverse Kinematics
- **Foot IK** - Ground adaptation
- **Look-at IK** - Head/eye tracking
- **Full-body IK** - Complete body positioning

## 📖 Usage

### Loading Animations

```cpp
Model* model = new Model("character.fbx");

// Get animations
size_t animCount = model->GetAnimationCount();
Animation* idle = model->GetAnimation(0);
Animation* walk = model->GetAnimation(1);
Animation* run = model->GetAnimation(2);
```

### Animator Setup

```cpp
Animator animator;
animator.SetModel(model);
animator.PlayAnimation("Idle");

// In update loop
animator.Update(deltaTime);
```

### State Machine

```cpp
AnimationStateMachine fsm;
fsm.AddState("Idle", idleAnim);
fsm.AddState("Walk", walkAnim);
fsm.AddState("Run", runAnim);

fsm.AddTransition("Idle", "Walk", []() { 
    return inputVelocity.length() > 0.1f; 
});

fsm.Update(deltaTime);
```

### Motion Matching

```cpp
MotionMatcher matcher;
matcher.LoadDatabase("motions.db");
matcher.SetTrajectory(targetTrajectory);

// Find best matching motion
MotionClip bestMatch = matcher.FindBestMatch(
    currentPose, 
    targetVelocity,
    deltaTime
);
```

### GPU Skinning

```cpp
// Upload bone matrices
BoneMatrixBuffer boneBuffer;
boneBuffer.Initialize(maxBones);
boneBuffer.UploadBoneMatrices(finalBoneMatrices);

// In shader
layout(std140) uniform BoneBuffer {
    mat4 boneMatrices[MAX_BONES];
};

vec4 skinVertex(vec4 position, ivec4 indices, vec4 weights) {
    vec4 skinnedPos = vec4(0);
    for (int i = 0; i < 4; i++) {
        skinnedPos += boneMatrices[indices[i]] * position * weights[i];
    }
    return skinnedPos;
}
```

## 🔧 Configuration

### Animation Settings
```cpp
AnimationConfig config;
config.blendTime = 0.2f;           // Blend duration
config.loopEnabled = true;         // Loop animations
config.playbackSpeed = 1.0f;       // Playback speed
config.interpolationQuality = HIGH; // Quality level
```

### Motion Matching Settings
```cpp
MotionMatchingConfig mmConfig;
mmConfig.searchRadius = 0.5f;      // Search tolerance
mmConfig.predictTime = 0.5f;       // Prediction horizon
mmConfig.databaseSize = 10000;     // Max clips in database
```

## 📊 Performance

| Feature | CPU | GPU | Memory |
|---------|-----|-----|--------|
| **Animation Blending** | ~0.1ms | - | Low |
| **Motion Matching** | ~0.5ms | - | Medium |
| **GPU Skinning** | ~0.01ms | ~0.1ms | Low |
| **Foot IK** | ~0.2ms | - | Low |

## 🐛 Debugging

See documentation in `docs/animation/`:
- [ANIMATION_DEBUG_GUIDE.md](../docs/animation/ANIMATION_DEBUG_GUIDE.md)
- [MOTION_MATCHING_DEBUG_FIXES.md](../docs/animation/MOTION_MATCHING_DEBUG_FIXES.md)
- [FOOT_IK_CHARACTER_GROUNDING_FIX.md](../docs/animation/FOOT_IK_CHARACTER_GROUNDING_FIX.md)

---
