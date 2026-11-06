#pragma once
#include "CameraTypes.h"
#include <glm/glm.hpp>

/**
 * Selection-driven camera focus (engine milestone).
 *
 * The follow camera is no longer hard-wired to the playable character: any
 * registered entity — the play-character twin, an NPC, or a static prop — can
 * be the focus, and the camera decides per-target whether to orbit-behind its
 * forward (animated / playable entities) or simply re-frame it in view (static
 * props such as trees, which have no heading to chase). When nothing
 * selectable is selected, the camera falls back to the playable character.
 *
 * This header is deliberately free of ECS / GL types so the resolution math is
 * covered by the camera test suite (tests/test_camera_focus.cpp). ECS callers
 * (editor_application.cpp, editor_main.cpp, test.cpp) translate an EntityID
 * into an EntityFocus via the helper in editor/CameraFocus.h and feed the
 * resulting CameraInput into ThirdPersonCamera::update().
 */

/**
 * The world-space description of whatever the camera should be looking at.
 * Callers build this from a selected ECS entity (position/forward from its
 * TransformComponent, `animated` from its layer/animator) or from the live
 * AnimatedCharacter (for the playable-character fallback, which carries a real
 * animation state + heading).
 */
struct EntityFocus {
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 0.0f, 1.0f};   // orbit-behind forward (animated only)
    CameraState animState = CameraState::IDLE;
    float moveMagnitude = 0.0f;
    glm::vec3 velocity{0.0f};
    bool animated = false;   // orbit-behind-forward + state-aware smoothing
    bool valid = false;      // is there a real focus target?
};

/**
 * Resolves the camera feed from a selection state + playable-character
 * fallback. Selection always wins over the fallback; an invalid selection
 * (valid == false) reverts to the character.
 */
class CameraFocusSystem {
public:
    EntityFocus selected;    // current selection (valid=false -> use fallback)
    EntityFocus fallback;    // the playable character (always treated as animated)

    // Build the CameraInput for this frame. The active focus is the selected
    // entity when one is selected, otherwise the playable-character fallback.
    // `targetIsAnimated` is set from the focus so ThirdPersonCamera only
    // auto-orients behind animated/playable entities (static props are framed
    // in place, never spun onto an arbitrary heading).
    CameraInput resolveCameraInput() const {
        const EntityFocus& f = (selected.valid || selected.animated) ? selected : fallback;
        CameraInput in;
        in.characterPosition  = f.position;
        in.characterForward   = f.forward;
        in.animState          = f.animState;
        in.moveMagnitude      = f.moveMagnitude;
        in.characterVelocity  = f.velocity;
        in.isGrounded         = true;
        in.targetIsAnimated   = f.animated;
        return in;
    }

    // Which focus is driving the camera this frame?
    const EntityFocus& activeFocus() const {
        return (selected.valid || selected.animated) ? selected : fallback;
    }

    // True when an explicit selection is driving the camera (vs. the fallback).
    bool followingSelection() const {
        return selected.valid || selected.animated;
    }

    // Set a fresh selection. Pass an EntityFocus whose `valid` is false to clear
    // the selection back to the playable character (matches "until the user
    // selects the playable character" semantics).
    void setSelected(const EntityFocus& f) { selected = f; }

    // The playable-character fallback is always animated + followable.
    void setFallback(const EntityFocus& f) {
        fallback = f;
        fallback.animated = true;
        fallback.valid = true;
    }
};
