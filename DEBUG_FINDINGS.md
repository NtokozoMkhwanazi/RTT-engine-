# Skeletal Animation Deformation Debug

## Issues Isolated

### 1. **Animation Channel Mismatch** ✅ FIXED
- Some bones have only rotation keyframes, no position keyframes
- Example: `spine1 | (431 frames)` - has frames but zero channels!
- Solution: Fall back to bind pose when animation channel is missing
- File: `link/Animation.h` - Added `HasPositionAnimation()`, `HasRotationAnimation()`, `HasScaleAnimation()`

### 2. **Bone Name Normalization** ✅ FIXED  
- Multiple normalization functions causing animation lookups to fail
- Solution: Created canonical `NormalizeBoneName()` in `BoneName.h`
- All files now use same normalization

### 3. **Inverted Skeleton** ⚠️ PARTIALLY ADDRESSED
- Bind pose Y positions were negative (inverted)
- Applied Y-flip to hierarchy and offset matrices
- **But: May be causing new deformation issues**

## Current State
- ✅ Animations load correctly
- ✅ Root motion working
- ✅ Bone matrices computed
- ❌ Deformation still "stretched weird"

## Diagnostic Output Available

In code:
- `[BIND POSE DEBUG]` - Shows offset matrix positions and scales
- `[BONE DEBUG]` - Shows animated bone positions and scales every 2 seconds  
- `[IDLE ANIMATION CHANNELS]` - Shows which animation channels exist for each bone

## Suspected Root Cause

The Y-flip approach may be breaking parent-child relationships. Alternative approaches to test:

1. **Don't flip Y** - Accept inverted skeleton visually
2. **Flip entire scene** - Apply Y-flip to model transformation instead
3. **Coordinate system conversion** - Use different handedness

## Code Locations

- **Y-flip applied**: `link/model.cpp` - `ReadHierarchy()`, `ExtractBones()`
- **Fallback logic**: `link/Animator.cpp` - `EvaluateNode()`
- **Channel detection**: `link/Animation.h` - BoneAnimation struct

## Next Steps

1. Test WITHOUT Y-flip to see original deformation pattern
2. Compare bind pose values with/without flip
3. Check if globalInverseTransform needs adjustment
4. Consider vertex weight distribution issues
