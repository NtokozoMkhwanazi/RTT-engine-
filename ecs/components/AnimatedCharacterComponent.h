#pragma once

/**
 * AnimatedCharacterComponent - ECS component that lets an NPC / character
 * entity share the play character's *full* AnimatedCharacter properties
 * (locomotion FSM + motion matching + foot IK) instead of a plain single-clip
 * Animator.
 *
 * Rendering: ModelRenderSystem::renderAnimated() draws these via the skin
 * shader, mirroring RenderPlayCharacter (model->Draw(shader, *animator())
 * using AnimatedCharacter::modelMatrix() which carries visualScale + the 180?
 * facing flip).
 *
 * Ticking: ModelRenderSystem::update() advances these every frame through
 * TickAnimatedCharacter(), which runs the character's FSM/motion-matching and
 * mirrors the resulting pose into the entity's TransformComponent so the
 * selection-driven camera / focus system / gizmo can target the NPC.
 */

#include "../ECS.h"
#include "TransformComponent.h"
#include "../../modelSystem/ModelHandle.h"
#include "../../modelSystem/Model.h"
#include "../../editor/AnimatedCharacter.h"
#include "../../animationSystem/AnimationStateMachine.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace ecs {

struct AnimatedCharacterComponent : public Component {
    // The live character (FSM + motion matcher + foot IK). NOT the play-
    // mode bot - this is an independent NPC that shares the bot's assets/
    // properties. Owns its own skeleton/animator/clips (destruction order
    // matters inside AnimatedCharacter).
    std::unique_ptr<AnimatedCharacter> character;

    // Non-owning borrows for entities that share an externally-owned
    // AnimatedCharacter + Model (the play-mode bot: PlayModeController owns
    // its FSM+animator, so the ECS render path borrows them and must NOT tick
    // the NPC wander/geo FSM). renderAnimated() draws these via the skin
    // shader + Model::Draw(shader,animator), mirroring RenderPlayCharacter.
    AnimatedCharacter* externalCharacter = nullptr;
    Model* externalModel = nullptr;
    bool externallyTicked = false;   // skip updateAnimatedCharacters tick

    // Render model for the same asset (skinning draw needs a Model* + the
    // character's Animator; mirror the PlayModeController split where the
    // bot keeps a separate render Model loaded from the same FBX).
    ModelSystem::ModelHandle renderHandle;
    std::string clipsDir;

    AnimatedCharacter::MotionContext motionContext =
        AnimatedCharacter::MotionContext::LOCOMOTION;
    bool active = true;

    // NPC wander cadence (idle most of the time, slow wander otherwise) so a
    // spawned NPC is visibly alive instead of frozen in place.
    float idleTimer = 0.0f;
    float wanderTimer = 0.0f;
    glm::vec2 wanderDir{0.0f};

    // Geo-telemetry drive (live feeds). When geoDriven, the geospatial system
    // owns the character's absolute position each tick (teleporting it to the
    // live lat/lon/ENU fix) and feeds geoMoveDir/geoMoveSpeed as the FSM input
    // (CharacterInput.moveDirection / moveMagnitude) so the locomotion FSM
    // plays idle/walk/jog/run by feed speed and faces along the feed heading.
    // Not used by standalone editor wander NPCs.
    bool geoDriven = false;
    glm::vec2 geoMoveDir{0.0f};   // unit move direction in the character's XZ plane
    float geoMoveSpeed = 0.0f;    // normalized [0..1] -> idle/walk/jog/run band

    bool isValid() const {
        const AnimatedCharacter* ch = externalCharacter ? externalCharacter : character.get();
        if (!ch || !ch->ready()) return false;
        if (externalModel) return externalModel->GetMeshCount() > 0;
        return renderHandle.isValid()
            && ModelSystem::ModelRegistry::getInstance().get(renderHandle) != nullptr;
    }

    Model* getModel() const {
        if (externalModel) return externalModel;
        return renderHandle.isValid()
            ? ModelSystem::ModelRegistry::getInstance().get(renderHandle)
            : nullptr;
    }

    AnimatedCharacter* get() { return externalCharacter ? externalCharacter : character.get(); }
    const AnimatedCharacter* get() const { return externalCharacter ? externalCharacter : character.get(); }

    // Hot-swap this NPC's character pack in place (forhotswap.txt). Reuses
    // AnimatedCharacter::HotSwapCharacterPack (flush + reload, stage 1) so NPCs
    // share the play character's verified single-asset path, and reloads the
    // GPU render model from the SAME path so skeleton+mesh never mismatch.
    // Noop (returns false) if the component has no live character.
    bool reload(const std::string& modelPath, const std::string& clipsDirPath) {
        if (!character) return false;
        // Capture the PRE-swap pose so the NPC cross-fades from it into the
        // hot-swapped clip set instead of popping. The layer is blended out by
        // tickInertialBlender() in ModelRenderSystem::updateAnimatedCharacters().
        std::vector<glm::mat4> prePose = snapshotCurrentPose();
        if (!character->HotSwapCharacterPack(modelPath, clipsDirPath)) return false;
        if (!prePose.empty()) {
            NPCBlendLayer layer;
            layer.boneSnapshot = std::move(prePose);
            layer.weight = 1.0f;
            layer.active = true;
            activeBlendLayers.push_back(std::move(layer));
        }
        renderHandle = ModelSystem::ModelRegistry::getInstance().load(modelPath);
        clipsDir = clipsDirPath;   // record active clips dir (for lazy contexts)
        return true;
    }

    // Multi-clip preview (Character/NPC details panels): play ONE clip (by
    // AnimatedCharacter::ClipSlot index) for `duration` seconds, overriding the
    // locomotion FSM. AnimatedCharacter::playPreview is already wired in
    // driveFsm (cancels on movement / timer expiry), so this just delegates.
    void playPreview(int clipIndex, float duration = 5.0f) {
        if (character) character->playPreview(clipIndex, duration);
    }

    // Multi-layer inertial crossfade state (forhotswap.txt Point 3) — mirrors
    // PlayModeController::BlendLayer so hot-swapped NPCs cross-fade their
    // skeleton instead of popping. Populated by reload() (pre-swap pose snapshot);
    // drained by tickInertialBlender() from ModelRenderSystem::updateAnimatedCharacters().
    struct NPCBlendLayer {
        std::vector<glm::mat4> boneSnapshot;
        float weight     = 1.0f;   // 1.0 -> old pose .. 0 -> blended into live pose
        float decaySpeed = 4.0f;   // ~0.25s fade
        bool  active     = false;
    };
    std::vector<NPCBlendLayer> activeBlendLayers;

    // Snapshot the NPC's current live bone palette (copy of debugFinalBones so it
    // survives a HotSwap that rebuilds the animator/skeleton).
    std::vector<glm::mat4> snapshotCurrentPose() const {
        if (!character || !character->ready()) return {};
        const auto& bones = character->debugFinalBones();
        return std::vector<glm::mat4>(bones.begin(), bones.end());
    }

    // Fade every active NPC blend layer out of `outMatrices` (the live bone
    // palette) and drop layers once decayed. Mirrors PlayModeController's
    // tickInertialBlender; debugFinalBones() is a const view, so callers must
    // const_cast the palette before passing (the same documented write-back trick
    // RenderPlayCharacter relies on for the play character).
    void tickInertialBlender(float dt, std::vector<glm::mat4>& outMatrices) {
        for (auto it = activeBlendLayers.begin(); it != activeBlendLayers.end();) {
            if (!it->active) {
                it = activeBlendLayers.erase(it);
                continue;
            }
            it->weight -= it->decaySpeed * dt;
            if (it->weight <= 0.0f) {
                it->active = false;
                it = activeBlendLayers.erase(it);
            } else {
                const size_t n = std::min(outMatrices.size(), it->boneSnapshot.size());
                for (size_t b = 0; b < n; ++b)
                    outMatrices[b] = outMatrices[b] * (1.0f - it->weight)
                                         + it->boneSnapshot[b] * it->weight;
                ++it;
            }
        }
    }
};

/**
 * Tick one AnimatedCharacter (FSM + motion matching + foot IK) and mirror its
 * pose into the ECS TransformComponent so the camera/focus system/gizmo track
 * the NPC. `terrain` maps (x,z) -> ground height (flat 0 by default).
 *
 * Kept as a free function (no World/Renderer dependency) so it is unit-testable
 * in isolation - the test proves an NPC driven by this shares the play
 * character's animation path (bones populate, animator advances - not T-pose).
 */
inline void TickAnimatedCharacter(AnimatedCharacter& ac, TransformComponent& t,
                                  float dt, const CharacterInput& input,
                                  const std::function<float(float, float)>& terrain =
                                      [](float, float) -> float { return 0.0f; }) {
    if (!ac.ready()) return;
    ac.update(dt, input, terrain);
    // Mirror pose into the ECS transform (position + heading). Scale is
    // carried by the character's visualScale at render time via modelMatrix(),
    // so the ECS scale is left at 1.0f; the render matrix applies visualScale.
    t.position = ac.position;
    t.setEulerAngles(glm::vec3(0.0f, ac.heading, 0.0f));
}

} // namespace ecs
