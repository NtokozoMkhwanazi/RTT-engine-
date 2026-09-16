#include "HybridState.h"

// Defined out-of-line (not inline) to produce a strong symbol that the linker
// will prefer over the weak inline HybridStateToString defined in
// HybridAnimGraph.h. Both enums share the name "HybridState" but have different
// values; a strong symbol ensures correct resolution.
std::string HybridStateToString(HybridState state) {
    switch (state) {
        case HybridState::LOCOMOTION: return "Locomotion";
        case HybridState::JUMP: return "Jump";
        case HybridState::FALL: return "Fall";
        case HybridState::CROUCH: return "Crouch";
        case HybridState::CROUCH_WALK: return "CrouchWalk";
        case HybridState::COMBAT: return "Combat";
        case HybridState::VAULT: return "Vault";
        case HybridState::CUSTOM: return "Custom";
        default: return "Unknown";
    }
}
