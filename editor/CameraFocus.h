#pragma once

/**
 * Selection-driven camera focus resolver — ECS glue.
 *
 * The camera follows a registered entity (the selected one, or the
 * playable-character twin as a fallback) instead of being hard-wired to the
 * AnimatedCharacter object. Thin translation layer: turns an EntityID into the
 * pure EntityFocus the testable CameraFocusSystem consumes. See
 * cameraSystem/CameraFocusSystem.h for the selection rules.
 */

#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "cameraSystem/CameraFocusSystem.h"
#include "modelSystem/Model.h"   // Model::GetBoundingBox() for the torso focus height
#include <glm/glm.hpp>

namespace Editor {

/**
 * Build an EntityFocus from any ECS entity that carries a TransformComponent.
 * An entity is treated as "animated / playable" (-> orbit-behind-forward) when
 * it sits on the Characters layer or carries a ModelAnimatorComponent. A static
 * prop (tree/rock/model with no animator) resolves to a focus-only target: the
 * camera glides to frame it but does not chase a heading.
 */
inline EntityFocus focusFromEntity(const ecs::World& world, ecs::EntityID id) {
    EntityFocus f;
    f.forward = glm::vec3(0.0f, 0.0f, 1.0f);   // neutral; ignored for static props
    if (id == ecs::INVALID_ENTITY_ID) return f;

    const ecs::Entity e{id};
    if (const auto* t = world.getComponentArchetype<ecs::TransformComponent>(e)) {
        f.position = t->position;
        f.forward  = t->getForward();
        f.valid = true;
        // NOTE: do NOT lift f.position here — ThirdPersonCamera::calculateIdealPosition
        // already adds config.pivotHeight above characterPosition to form the look-at
        // pivot. Lifting f.position AND adding pivotHeight would stack the offsets and
        // place the pivot above the entity's head, making the camera look down at empty
        // space instead of the entity's torso (the "camera from below" framing bug).
    }
    // Animated/playable if it's on the Characters layer (the play-character
    // twin carries it; skinned NPCs get it too) OR it has an animator.
    if (const auto* lc = world.getComponentArchetype<ecs::LayerComponent>(e))
        f.animated = (lc->layer == ecs::LayerComponent::Characters);
    if (world.getComponentArchetype<ecs::ModelAnimatorComponent>(e))
        f.animated = true;
    return f;
}

/**
 * Resolve the camera's EntityFocus for this frame:
 *   1. selected ECS entity wins (animated if Characters-layer/animator, else static)
 *   2. else the playable-character twin entity (always animated)
 *   3. else the live character focus (passed by the caller, used before the twin
 *      exists / after it's been deleted)
 * Selection and the fallback character are mutually exclusive: a selected
 * static prop fully takes over framing until the user clears the selection or
 * re-selects a character/NPC.
 */
inline EntityFocus resolveCameraFocus(const ecs::World& world,
                                      ecs::EntityID selected,
                                      ecs::EntityID characterEntity,
                                      const EntityFocus& characterFocus) {
    EntityFocus sel = focusFromEntity(world, selected);
    if (sel.valid) return sel;

    if (characterEntity != ecs::INVALID_ENTITY_ID) {
        EntityFocus twin = focusFromEntity(world, characterEntity);
        if (twin.valid) {
            twin.animated = true;            // the twin represents the playable char
            return twin;
        }
    }
    return characterFocus;
}

} // namespace Editor
