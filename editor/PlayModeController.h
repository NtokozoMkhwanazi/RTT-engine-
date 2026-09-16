/**
 * PlayModeController - wraps AnimatedCharacter for the editor's Play Mode.
 *
 * Owns the character's lifecycle (load model + locomotion clips, update,
 * preview, diagnostics) so the editor only has to forward input state and a
 * terrain height function. Pure logic - no GL calls - so it can be unit-tested
 * headlessly.
 */
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "AnimatedCharacter.h"

namespace Editor {

class PlayModeController {
public:
    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------
    // Load the skinned model and its locomotion clips from the given dir.
    bool load(const std::string& modelPath, const std::string& clipsDir);
    // Release everything (safe to call multiple times / when not loaded).
    void shutdown();
    bool isLoaded() const { return character_ && character_->ready(); }

    // -----------------------------------------------------------------------
    // Update
    // -----------------------------------------------------------------------
    // Advance the character with the given input and terrain height function.
    void update(float dt, const CharacterInput& input,
                const std::function<float(float, float)>& terrain) {
        if (character_ && character_->ready()) {
            character_->update(dt, input, terrain);
        }
    }

    // -----------------------------------------------------------------------
    // Preview / diagnostics passthrough
    // -----------------------------------------------------------------------
    void playPreview(int index, float duration) {
        if (character_) character_->playPreview(index, duration);
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

private:
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

} // namespace Editor
