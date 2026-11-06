/**
 * PlayModeController - wraps AnimatedCharacter for the editor's Play Mode.
 *
 * Owns the character's lifecycle (load model + locomotion clips, update,
 * preview, diagnostics) so the editor only has to forward input state and a
 * terrain height function. Pure logic - no GL calls - so it can be unit-tested
 * headlessly.
 */
#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "AnimatedCharacter.h"

namespace Editor {

class PlayModeController {
public:
    // -----------------------------------------------------------------------
    // Multi-layer inertial crossfade (forhotswap.txt, Point 1)
    // -----------------------------------------------------------------------
    // Snapshot of the bone palette captured at the instant a preview clip starts
    // (see playPreview()). It is blended OUT of the live palette each tick so the
    // next clip cross-fades on instead of snapping from a hard cut. Multiple
    // layers may overlap (concurrent previews) and each decays independently.
    struct BlendLayer {
        std::vector<glm::mat4> boneSnapshot;
        float weight      = 1.0f;   // 1.0 -> fully on (old pose) .. 0 -> blended out
        float decaySpeed  = 4.0f;   // ~0.25s fade to rest
        bool  active      = false;
    };

    // Fade every active blend layer out of `outMatrices` (the live bone palette)
    // and drop layers once they have decayed. Called once per frame from update()
    // on the post-tick bone palette; debugFinalBones() is a const view into the
    // animator's palette, so callers const_cast it back (see update()).
    void tickInertialBlender(float dt, std::vector<glm::mat4>& outMatrices) {
        for (auto it = m_activeBlendLayers.begin(); it != m_activeBlendLayers.end();) {
            if (!it->active) {
                it = m_activeBlendLayers.erase(it);
                continue;
            }
            it->weight -= it->decaySpeed * dt;
            if (it->weight <= 0.0f) {
                it->active = false;
                it = m_activeBlendLayers.erase(it);
            } else {
                // outMatrices = mix(outMatrices, snapshot, weight): at weight=1
                // the old pose fully overrides, decaying to the live pose at 0.
                const size_t n = std::min(outMatrices.size(), it->boneSnapshot.size());
                for (size_t b = 0; b < n; ++b) {
                    outMatrices[b] = outMatrices[b] * (1.0f - it->weight)
                                         + it->boneSnapshot[b] * it->weight;
                }
                ++it;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------
    // Load the skinned model and its locomotion clips from the given dir.
    bool load(const std::string& modelPath, const std::string& clipsDir);
    // Release everything (safe to call multiple times / when not loaded).
    void shutdown();
    // Dynamic Hot-Swap (forhotswap.txt, stage 1): reload the character's model
    // + clip pack IN PLACE (preserves the AnimatedCharacter object and its
    // external render-model refs). Returns false on failure.
    bool reload(const std::string& modelPath, const std::string& clipsDir);
    bool isLoaded() const { return character_ && character_->ready(); }

    // -----------------------------------------------------------------------
    // Update
    // -----------------------------------------------------------------------
    // Advance the character with the given input and terrain height function.
    void update(float dt, const CharacterInput& input,
                const std::function<float(float, float)>& terrain) {
        if (!character_ || !character_->ready()) {
            return;
        }
        // forPlayer.txt Theme 1: while the viewport gizmo is dragging the
        // play-character twin, pause the locomotion FSM so the user's manual
        // translate/rotate/scale is held for this frame and syncPlayCharacterTwin()
        // flows the twin edit straight into the bot instead of the next tick
        // overwriting it.
        if (isBeingManipulatedByGizmo) {
            return;
        }
        character_->update(dt, input, terrain);
        // Inertial crossfade: fade any active preview layers (captured in
        // playPreview()) out of the live bone palette so the next preview clip
        // blends on instead of snapping. debugFinalBones() returns a const view
        // into the animator's palette, which renderPlayCharacter reads via
        // GetFinalBoneMatrices() after update() - so const_cast back into it for
        // the blended palette to reach the skin shader (the documented trick).
        const auto& bones = character_->debugFinalBones();
        if (!bones.empty()) {
            auto& palette = const_cast<std::vector<glm::mat4>&>(bones);
            tickInertialBlender(dt, palette);
        }
    }

    // -----------------------------------------------------------------------
    // Preview / diagnostics passthrough
    // -----------------------------------------------------------------------
    void playPreview(int index, float duration) {
        if (!character_) return;
        // Capture the CURRENT live pose into a blend layer (weight=1, active)
        // so the outgoing pose cross-fades out over ~0.25s instead of the preview
        // clip snapping in from a hard cut. The layer is drained by
        // tickInertialBlender() in update(). The caller already maps context-clip
        // indices into the valid 0..CLIP_SLOTS-1 range (see the inspector);
        // AnimatedCharacter::playPreview re-guards the range regardless.
        BlendLayer layer;
        layer.boneSnapshot = character_->debugFinalBones();
        layer.weight = 1.0f;
        layer.active = true;
        m_activeBlendLayers.push_back(std::move(layer));
        character_->playPreview(index, duration);
    }
    AnimatedCharacter::PoseDiag debugPoseDiag() const {
        return character_ ? character_->debugPoseDiag() : AnimatedCharacter::PoseDiag{};
    }
    float debugClipSpeed(int index) const {
        return character_ ? character_->debugClipSpeed(index) : 1.0f;
    }

    // -----------------------------------------------------------------------
    // Access
    // -----------------------------------------------------------------------
    AnimatedCharacter& character() { return *character_; }
    const AnimatedCharacter& character() const { return *character_; }
    bool hasCharacter() const { return character_ != nullptr; }

    // Stage 2 (forhotswap.txt, async): atomically adopt pre-built systems that
    // a worker thread compiled off the main thread (see AnimatedCharacter::
    // ApplyRuntimeSystems). The character object + its external render refs are
    // preserved; only the motion systems are swapped, and the live pose is kept
    // so the bot never teleports mid-play.
    bool swapInRuntimeSystems(AnimatedCharacter::RuntimeSystems&& rs) {
        if (!character_) return false;
        character_->ApplyRuntimeSystems(std::move(rs));
        return character_->ready();
    }

    // forPlayer.txt Theme 1: true while the viewport gizmo is dragging the
    // play-character twin this frame. Set by EditorApplication::update() (only
    // in play mode, only when IsDragging() on the twin's entity). When true,
    // update() bypasses the locomotion FSM, and updatePlayMode() skips the
    // capsule resolver, so the gizmo edit flows into the bot unopposed.
    bool isBeingManipulatedByGizmo = false;

private:
    std::vector<BlendLayer> m_activeBlendLayers;
    std::unique_ptr<AnimatedCharacter> character_;
};

inline bool PlayModeController::load(const std::string& modelPath, const std::string& clipsDir) {
    shutdown();
    auto cc = std::make_unique<AnimatedCharacter>();
    if (!cc->load(modelPath)) return false;
    if (cc->loadLocomotion(clipsDir) < 1) return false;
    character_ = std::move(cc);
    return true;
}

inline void PlayModeController::shutdown() {
    character_.reset();
}

inline bool PlayModeController::reload(const std::string& modelPath, const std::string& clipsDir) {
    // Stage 1 hot-swap: flush + reload the character's motion systems in place.
    // The AnimatedCharacter object (and thus this controller's external refs)
    // is preserved, so callers can keep their render-model handle across packs.
    return character_ ? character_->HotSwapCharacterPack(modelPath, clipsDir) : false;
}

} // namespace Editor
