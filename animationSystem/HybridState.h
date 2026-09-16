#pragma once
#include <string>

// ============================================================================
// HYBRID STATE ENUM
// ============================================================================
// Extracted into its own header to break circular dependencies between
// HybridMMFSM.h, MotionMatcher.h, and MotionTransitionGraph.h.
// ============================================================================

enum class HybridState {
    LOCOMOTION,      // MM handles idle/walk/run
    JUMP,            // One-shot jump animation
    FALL,            // Falling animation (looping)
    CROUCH,          // Crouch idle
    CROUCH_WALK,     // Crouch walk (MM with crouch database)
    COMBAT,          // Combat state
    VAULT,           // Vaulting/climbing
    CUSTOM           // User-defined state
};

// Forward-declared — defined in HybridState.cpp to avoid weak-symbol collision
// with HybridAnimGraph.h's identically-named HybridStateToString. Both enums
// share the same name "HybridState" but have different values, so if
// HybridStateToString is inline, the linker may resolve calls to the wrong
// version (HybridAnimGraph's LOCOMOTION_GROUNDED = 0 instead of our LOCOMOTION = 0).
std::string HybridStateToString(HybridState state);
