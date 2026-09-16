/**
 * AnimatedCharacter - pure-logic character controller (GL-free).
 *
 * Loads a skinned model plus locomotion clips (Idle/Walk/Run/Jump/Fall/
 * Crouch/CrouchWalk), runs terrain-snapped kinematic movement with a
 * locomotion FSM, and drives the MotionMatcher pose search on top of an
 * Animator (including foot IK / footplanting). render() is intentionally not
 * implemented here - the test suite only exercises the logic surface.
 *
 * Clips are loaded from standalone FBX files and converted from Assimp's tick
 * domain to real seconds (duration AND key times) - without this a ~16s Idle
 * clip reports ~499s and is rejected by the motion database's 60s guard.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "../modelSystem/Model.h"
#include "../animationSystem/Animator.h"
#include "../animationSystem/Animation.h"
#include "../animationSystem/AnimationStateMachine.h"
#include "../animationSystem/AssimpAnimationLoader.h"
#include "../motionMatching/MotionDatabase.h"
#include "../motionMatching/MotionMatcher.h"
#include "../motionMatching/MotionKDTree.h"
#include "../motionMatching/MotionTransitionGraph.h"
#include "../memory/MemoryManager.h"
#include "../boneSystem/BoneName.h"
#include "profiler.h"
#include "world_manager.h"

class AnimatedCharacter {
public:
    // Stable clip slots used by activeClipIndex()/debugClipSpeed().
    enum ClipSlot {
        CLIP_IDLE = 0,
        CLIP_WALK = 1,
        CLIP_RUN = 2,
        CLIP_JUMP = 3,
        CLIP_FALL = 4,
        CLIP_CROUCH = 5,
        CLIP_CROUCH_WALK = 6,
        CLIP_SLOTS = 7
    };

    // Motion contexts - each owns a dedicated motion database (Contextual
    // Database Switching, the architecture suggested in suggestions.txt). The
    // matcher searches ONLY the active context's clips, so a combat swing can
    // never bleed into a locomotion walk and vice versa, and each database
    // stays small and independently tunable. LOCOMOTION is the default;
    // crouch auto-switches with the crouch input; combat/capoeira/dance are
    // explicit requests (editor keys 3/4/5, or CharacterInput.motionContext).
    enum class MotionContext {
        LOCOMOTION = 0,  // Idle/Walk/Run/Jump/turns/run-look-back
        CROUCH,          // Crouching + CrouchWalk (auto with crouch input)
        COMBAT,          // Boxing strikes/blocks/defeated
        CAPOEIRA,        // the ~39-clip capoeira set (built lazily on demand)
        DANCE,           // rumba/hip-hop/taunts/social clips
        COUNT
    };

    // REMOVED: StandstillPostureLock / StableIdleSnapshot.
    // Previously froze the skeleton on the first frame of rest to prevent
    // IK-loop knee jitter. Now replaced by continuous motion matching through
    // the structural transition graph — all clips flow seamlessly, no pose
    // is ever frozen. Foot IK runs with sub-frame damping (see ApplyFootIK).

    // -----------------------------------------------------------------------
    // Tuning (public so tests can read/write)
    // -----------------------------------------------------------------------
    float scale = 1.0f;                // World scale applied when rendering
    // Render-only shrink: the bot asset is authored at ~1.8 m (human scale) but
    // visually towers over the props/trees, so render it at 70% of its height
    // (~1.26 m). This ONLY scales the MODEL MATRIX - the animation-speed math
    // (currentSpeed()/scale) and the physics capsule keep using `scale` so
    // locomotion tuning is unchanged. Set per-character (test.cpp/editor).
    float visualScale = 0.7f;
    float targetHeight() const { return targetHeight_; }
    float playbackSpeed = 1.0f;        // FSM-path time multiplier
    bool motionMatchingEnabled = true; // Pose-search drives the animator
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float heading = 0.0f;              // Yaw (radians); local -Z is forward
    bool grounded = true;
    glm::vec3 terrainNormal{0.0f, 1.0f, 0.0f};

    // -----------------------------------------------------------------------
    // Loading
    // -----------------------------------------------------------------------
    bool load(const std::string& path);
    int loadLocomotion(const std::string& dir);
    static Animation* LoadClipFromFile(const std::string& path);

    // -----------------------------------------------------------------------
    // Queries
    // -----------------------------------------------------------------------
    size_t boneCount() const { return skeleton_ ? skeleton_->bones.size() : 0; }
    float rawSizeY() const { return rawSizeY_; }
    bool ready() const { return loaded_; }
    int clipCount() const { return clipCount_; }
    AnimationState state() const { return state_; }
    float idleClipDuration() const { return idleClipDuration_; }
    bool crouchState() const { return crouching_; }
    float currentSpeed() const { return glm::length(glm::vec2(velocity.x, velocity.z)); }
    int activeClipIndex() const { return activeClipIndex_; }
    std::string activeClipName() const;
    float animatorTime() const { return animator_ ? animator_->GetCurrentTime() : 0.0f; }

    // -----------------------------------------------------------------------
    // Animation / rendering access
    // -----------------------------------------------------------------------
    Animator* animator() { return animator_.get(); }
    const Animator* animator() const { return animator_.get(); }
    const Skeleton* skeleton() const { return skeleton_.get(); }

    // -----------------------------------------------------------------------
    // World model matrix (rendering + foot IK)
    // -----------------------------------------------------------------------
    // The bot asset is authored facing +Z, while the character logic treats
    // local -Z as forward. The heading rotation must therefore be offset by
    // 180 degrees - without the flip the rendered model faces the follow
    // camera even though the camera sits behind the bot (back-to-front).
    // Renderers AND the foot-IK pipeline must use this same matrix so the
    // feet lock to the floor where the visible model is.
    static glm::mat4 modelMatrix(const glm::vec3& pos, float heading, float scale) {
        glm::mat4 m(1.0f);
        m = glm::translate(m, pos);
        m = glm::rotate(m, heading + kPi, glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::scale(m, glm::vec3(scale));
        return m;
    }
    glm::mat4 modelMatrix() const { return modelMatrix(position, heading, scale * visualScale); }

    // -----------------------------------------------------------------------
    // Motion matching state
    // -----------------------------------------------------------------------
    bool mmAirborne() const { return !grounded; }
    bool mmJumping() const { return legacyJumpTimer_ > 0.0f; }
    std::string mmActiveClip() const;
    bool motionMatchingReady() const { return mmReady_; }
    bool isMotionMatchingActive() const { return motionMatchingEnabled && mmReady_; }

    // -----------------------------------------------------------------------
    // Motion contexts (contextual database switching)
    // -----------------------------------------------------------------------
    // Switch the pose-search database to a context. Crossfades between the old
    // and new databases (blendDuration = 0 switches instantly); requests to
    // the already-active context are no-ops (safe to send every frame). The
    // capoeira database is built lazily on first request.
    void setMotionContext(MotionContext ctx, float blendDuration = 0.15f);

    // Build transition clips between all currently-built motion contexts.
    // Called after databases are loaded so the transition graph has both
    // endpoints available.
    void buildTransitionGraph();

    // Check if a transition clip is currently playing.
    bool isTransitionClipPlaying() const { return transitionClipPlaying_; }

    // Check if clip-lock mode is active (idle cycle lock — matcher is
    // advancing the current clip's play time without KD-tree search).
    bool isClipLocked() const;

    // Access the context-switch cooldown timer (for testing).
    float contextSwitchCooldown() const { return contextSwitchCooldown_; }

    MotionContext motionContext() const { return motionContext_; }
    std::string motionContextName() const;
    // Pose/clip counts of a context's database (0 = not built / empty).
    size_t contextPoseCount(MotionContext ctx) const;
    int contextClipCount(MotionContext ctx) const;

    // -----------------------------------------------------------------------
    // Preview (plays a clip by slot until it finishes or movement cancels it)
    // -----------------------------------------------------------------------
    void playPreview(int index, float duration) {
        if (index < 0 || index >= CLIP_SLOTS ||
            index >= static_cast<int>(clips_.size()) || !clips_[index]) return;
        previewClipIndex_ = index;
        previewTimer_ = duration;
        previewActive_ = true;
    }

    // -----------------------------------------------------------------------
    // Update
    // -----------------------------------------------------------------------
    void update(float dt, const CharacterInput& input,
                const std::function<float(float, float)>& terrain);

    // -----------------------------------------------------------------------
    // Debug / diagnostics
    // -----------------------------------------------------------------------
    const std::vector<glm::mat4>& debugFinalBones() const {
        static const std::vector<glm::mat4> kEmpty;
        return animator_ ? animator_->GetFinalBoneMatrices() : kEmpty;
    }

    struct PoseDiag {
        std::string matcherClip;              // Clip the pose search selected
        std::string dominantRawName;          // Animator's dominant layer clip
        bool dominantMatchesMatcher = false;  // Pointer identity
        float dominantWeight = 0.0f;
        float dominantTime = 0.0f;
        glm::vec3 leftFoot{0.0f};
        glm::vec3 rightFoot{0.0f};
        bool leftLocked = false;
        bool rightLocked = false;
        // --- Extended IK diagnostics (visual, post-IK) ---
        float leftKneeDeg  = -1.0f;     // Rendered left knee angle (175 = straight)
        float rightKneeDeg = -1.0f;     // Rendered right knee angle
        float leftLockWeight  = 0.0f;  // Foot lock weight [0,1]
        float rightLockWeight = 0.0f;
        float leftAnkleOffY_m  = 0.0f; // Ankle translate residual (world metres)
        float rightAnkleOffY_m = 0.0f;
        float pelvisDropY = 0.0f;      // Pelvis height adjustment (metres)
        float leftLegReach  = 0.0f;    // L1+L2 leg reach (world metres)
        float rightLegReach = 0.0f;
        bool isMoving = false;
    };
    PoseDiag debugPoseDiag() const;
    float debugClipSpeed(int index) const;

private:
    static constexpr float kPi = 3.14159265358979323846f;
    // FSM crossfade length. The old hard Play() cut snapped the whole pose on
    // every state change - most visibly on landing (Fall -> Walk) and on
    // direction reversals. 0.25s hides the pose delta without feeling mushy.
    static constexpr float kFsmBlendDuration = 0.25f;

    void driveFsm(float dt);
    void driveMotionMatching(float dt, float floorY);
    int slotForState(AnimationState s) const;
    const BoneAnimation* findRootBone(const Animation* clip) const;
    void computeClipSpeeds();
    static void TrimClipTo(Animation& clip, float maxSeconds);

    // Contextual database building (one MotionDatabase per MotionContext).
    // All contexts build lazily on first request except LOCOMOTION, which is
    // built at load time (it is the default search domain).
    void buildContextDatabases(const std::string& dir);
    void ensureContextBuilt(MotionContext ctx);
    void buildCapoeiraContext(const std::string& dir);

    // Combat combo helpers (Black Myth-style: movement during attacks,
    // directional selection, 3-step combo chain with input buffering).
    void startCombatAction(const glm::vec2& moveDir, float moveMag);
    void selectCombatClip(int& outIdx, int step, const glm::vec2& dir);
    void endCombatAction();

    // AAA Root Motion Extraction: extracts the root bone's world-space delta
    // from the active combat clip and applies it to the character's position.
    // This replaces procedural WASD movement during attacks — the animation's
    // authored root motion drives the capsule, eliminating skating/sliding.
    glm::vec3 ComputeRawAssetRootDisplacement(float dt);
    bool combatClipNeedsRootMotion() const;

    // Target Warping / Soft Magnetism: adjusts the extracted root motion delta
    // to steer the character toward a combat target during attack playback.
    // The target is set via setCombatTarget() — typically the nearest enemy or
    // the direction the player is attacking toward.
    void setCombatTarget(const glm::vec3& target) { combatTarget_ = target; }
    bool hasCombatTarget() const { return combatHasTarget_; }
    void clearCombatTarget() { combatHasTarget_ = false; }
    void ApplyTargetWarpingToRootMotion(glm::vec3& outWorldDelta, float dt,
                                         const glm::vec3& enemyTargetPos);

    // Tagless Procedural Foot Planting: monitors foot bone velocity and height
    // relative to the capsule root. When a foot is stationary and close to the
    // ground, the system dynamically engages a lock — no artist-authored
    // metatags required. Uses the existing FootIKState.isLocked/lockWeight fields.
    void UpdateProceduralFootPlanting(float dt);

    // Query helper for target magnetism (stub: returns closest enemy or false)
    bool EngineQueryClosestLivingEnemy(const glm::vec3& from, const glm::vec2& dir,
                                        float maxDist, glm::vec3& outTargetPos);

    // Register a Profiler provider that pushes MotionMatcher + memory stats
    // into the Profiler every frame (called once after the matcher is built).
    void registerProfilingProvider();
    void addClipToContext(MotionContext ctx, const std::string& name,
                          const std::string& file, const std::string& dir);

    // Destruction order matters: animator_/skeleton_ die first, then matcher_
    // (whose database holds non-owning aliases into clips_), then clips_ last.
    std::vector<std::unique_ptr<Animation>> clips_;
    std::vector<float> clipSpeeds_;

    // Contextual databases + pre-built KD-trees, one per MotionContext. The
    // matcher holds RAW pointers into these (non-owning SetDatabaseExplicit),
    // so they must be declared BEFORE matcher_ (destroyed AFTER it) and AFTER
    // clips_ (the context DBs alias slot clips, so clips_ must outlive them).
    //
    // FIX (todo §2): Each context's KD-tree is built ONCE at load time and
    // warm-swapped via SetDatabaseExplicit (zero allocation) instead of
    // being rebuilt inside SetDatabase on every context switch — the
    // rebuild was the source of the crouch↔run FPS dips.
    std::array<std::unique_ptr<MotionDatabase>, (size_t)MotionContext::COUNT> contextDatabases_;
    std::array<std::unique_ptr<MotionKDTree>, (size_t)MotionContext::COUNT> contextSearchTrees_;
    std::string clipsDir_;                        // for lazy capoeira building
    MotionContext motionContext_ = MotionContext::LOCOMOTION;
    bool wasCrouching_ = false;                   // crouch-context edge detect

    // ---- Structural Motion Graph: Pre-computed transition clips ----
    // Between motion context databases (LOCOMOTION↔CROUCH, LOCOMOTION↔COMBAT,
    // etc.), instead of instant SetDatabaseExplicit swaps. Each clip is a
    // smooth all-joint slerp blend pre-baked at load time via TransitionClipGenerator.
    MotionTransitionGraph transitionGraph_;
    bool transitionClipPlaying_{false};           // Is a transition clip active?
    MotionContext transitionTargetCtx_{MotionContext::LOCOMOTION};  // Where we're blending to

    // Cooldown timer for motion context switches (prevents rapid crouch
    // toggle jitter — e.g. double-tapping C causes conflicting transition clips
    // and leg flexing). While > 0, new context switch requests are ignored.
    float contextSwitchCooldown_{0.0f};
    static constexpr float kContextSwitchCooldown = 0.35f;  // ~1 cycle at 120fps

    // Speed threshold (m/s, world units) below which the character is
    // considered "at rest" and clip-lock mode engages for smooth idle cycling.
    static constexpr float kAtRestThreshold = 0.05f;

    std::unique_ptr<MotionMatcher> matcher_;
    std::unique_ptr<Skeleton> skeleton_;
    std::unique_ptr<Animator> animator_;

    int clipCount_ = 0;
    float idleClipDuration_ = 0.0f;
    float rawSizeY_ = 0.0f;
    float targetHeight_ = 1.8f;
    bool loaded_ = false;
    bool mmReady_ = false;
    AnimationState state_ = AnimationState::NONE;
    // Last state we actually started a clip for - driveFsm crossfades on a
    // state CHANGE only (re-blending every frame would restart the blend).
    AnimationState lastFsmState_ = AnimationState::NONE;
    bool crouching_ = false;
    bool sprinting_ = false;
    int activeClipIndex_ = CLIP_IDLE;
    float legacyJumpTimer_ = 0.0f;
    bool previewActive_ = false;
    int previewClipIndex_ = -1;
    float previewTimer_ = 0.0f;
    glm::vec2 lastMoveDir_{0.0f, 0.0f};

    // ---- Combat / dance action playback ----
    bool combatActionActive_ = false;   // True while a 1-shot combat clip plays
    float combatTimer_ = 0.0f;           // Counts down the active clip's duration
    std::shared_ptr<Animation> combatClip_;  // The clip being played directly
    bool prevAttack_ = false;            // Edge-detect the attack button
    glm::vec2 combatMoveDir_{0.0f, 0.0f};  // WASD captured at combat start (directional attacks)

    // Combat target for soft magnetism / target warping
    glm::vec3 combatTarget_{0.0f};            // World-space target during combat
    bool combatHasTarget_ = false;            // True when a target is set

    // ---- Combo chain system (Black Myth Wukong style) ----
    // A combo advances through 3 steps (Punch → Swipe → JumpAttack). Each
    // attack press within kComboWindow seconds advances the chain. Movement
    // (WASD) is still processed during combat so the bot "slides" while
    // attacking — root motion from the clip adds to velocity. After the final
    // step's clip finishes with no further input, the character auto-returns
    // to Locomotion (or Combat Idle if standing still).
    int comboStep_ = 0;                 // 0 = no combo, 1/2/3 = combo step
    float comboTimer_ = 0.0f;           // Time window for next attack press
    bool pendingAttack_ = false;        // Attack input buffered during clip
    int combatClipIndex_ = 0;           // Index of current clip in the ordered combat list

    // Combat clip preference order per step (directional variants)
    static constexpr float kComboWindow = 0.3f;      // seconds for next input
    static constexpr float kAttackBuffer = 0.15f;    // pre-finish input acceptance
    static constexpr float kCombatMoveScale = 0.3f; // WASD speed modifier during attack

    // Movement tuning
    float jumpSpeed_ = 5.5f;
    float gravity_ = 9.8f;

    // ---- Jump cycle tuning (todo §1) ----
    // Jump input is edge-detected (rising edge only) to prevent double/triple
    // jumps when the button is held.  Coyote time gives a brief grace window
    // after leaving ground so a slightly-late press still fires.  Jump buffer
    // holds a press made mid-air so it auto-fires on the next landing.
    static constexpr float kCoyoteTime = 0.10f;     // 6 frames @ 60fps
    static constexpr float kJumpBufferTime = 0.10f;  // 6 frames
    static constexpr float kLandingRecovery = 0.15f; // hold Fall→Idle gap
    // Hysteresis around the apex: only flip JUMP→FALL when *descending*,
    // and keep JUMP through the flat apex zone (|vy| < this band).  0.3 m/s is
    // small enough to be visually meaningless but large enough to absorb
    // float noise — at 60fps gravity moves vy by 0.16 m/s/frame, so without a
    // band the state could flip-flop on 1-2 frames near zero.
    static constexpr float kJumpApexHysteresis = 0.3f;
    bool prevJumpInput_ = false;
    float coyoteTimeLeft_ = 0.0f;
    float jumpBufferLeft_ = 0.0f;
    float landingRecoveryLeft_ = 0.0f;
    // Sticky airborne state for JUMP↔FALL hysteresis (avoids apex jitter).
    AnimationState lastAirborneState_ = AnimationState::JUMP;
};

// ============================================================================
// Implementation
// ============================================================================

inline bool AnimatedCharacter::load(const std::string& path) {
    loaded_ = false;
    float progress = 0.0f;
    std::unique_ptr<AsyncModelData> data;
    try {
        data = Model::LoadModelData(path, &progress);
    } catch (...) {
        return false;
    }
    if (!data || data->skeleton.bones.empty()) return false;

    skeleton_ = std::make_unique<Skeleton>(data->skeleton);
    // Normalize the bone-mapping keys (the FBX stores "mixamorig:LeftFoot"
    // etc.). Every lookup in the engine (animator foot IK, motion matcher,
    // diagnostics) uses NormalizeBoneName() on the query, so the map must be
    // in the same normalized space or the foot/root bones are never found.
    std::map<std::string, int> normMapping;
    for (const auto& kv : skeleton_->boneMapping) {
        normMapping[NormalizeBoneName(kv.first)] = kv.second;
    }
    skeleton_->boneMapping.swap(normMapping);
    // The FBX may not identify a root bone; pick the Hips node when present.
    if (skeleton_->rootBoneIndex < 0) {
        auto it = skeleton_->boneMapping.find("hips");
        if (it != skeleton_->boneMapping.end()) skeleton_->rootBoneIndex = it->second;
    }
    animator_ = std::make_unique<Animator>(skeleton_.get());

    // Lock the root bone to its bind pose. The locomotion clips carry baked
    // root translation that accumulates over each cycle and SNAPS back to the
    // cycle start at the loop point - with the character position already
    // driven by velocity, the unlocked root made the bot visibly jump back a
    // step every clip loop ("misses a step") and move faster than the tuned
    // speed. Locking removes the baked translation entirely; the velocity-
    // driven position provides all forward motion.
    animator_->SetLockRootPosition(true);

    // Raw model height from the bounding box (fall back to skeleton extent).
    const BoundingBox& bb = data->boundingBox;
    float sizeY = 0.0f;
    if (std::isfinite(bb.max.y) && std::isfinite(bb.min.y) && bb.max.y > bb.min.y) {
        sizeY = bb.max.y - bb.min.y;
    }
    if (!(sizeY > 0.001f)) {
        float minY = FLT_MAX, maxY = -FLT_MAX;
        for (const auto& b : skeleton_->bones) {
            const float ty = b.bindTransform[3][1];
            minY = std::min(minY, ty);
            maxY = std::max(maxY, ty);
        }
        if (std::isfinite(minY) && std::isfinite(maxY) && maxY > minY) sizeY = maxY - minY;
    }
    if (!(sizeY > 0.001f) || !std::isfinite(sizeY)) sizeY = 1.8f;
    rawSizeY_ = sizeY;
    scale = targetHeight_ / rawSizeY_;

    loaded_ = true;
    return true;
}

inline Animation* AnimatedCharacter::LoadClipFromFile(const std::string& path) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(
        path, aiProcess_Triangulate | aiProcess_GenUVCoords | aiProcess_FlipUVs);
    if (!scene || !scene->HasAnimations() || scene->mNumAnimations == 0) return nullptr;

    Animation raw("", 0.0f, 0.0f);
    try {
        raw = AssimpAnimationLoader::LoadAnimation(scene, scene->mAnimations[0]);
    } catch (...) {
        return nullptr;
    }

    // Convert Assimp's tick domain to real seconds (duration AND key times).
    const float tps = raw.GetTicksPerSecond();
    if (tps > 0.0f) {
        raw.duration = raw.duration / tps;
        for (auto& kv : raw.boneAnimations) {
            for (auto& t : kv.second.positionTimes) t /= tps;
            for (auto& t : kv.second.rotationTimes) t /= tps;
            for (auto& t : kv.second.scaleTimes) t /= tps;
        }
    }
    return new Animation(std::move(raw));
}

inline int AnimatedCharacter::loadLocomotion(const std::string& dir) {
    clipCount_ = 0;
    idleClipDuration_ = 0.0f;
    // Destroy the matcher (whose motion database holds aliasing shared_ptrs
    // into clips_) BEFORE freeing the clip objects.
    matcher_.reset();
    clips_.clear();
    clips_.resize(CLIP_SLOTS);
    clipSpeeds_.assign(CLIP_SLOTS, 0.0f);

    struct SlotFile { ClipSlot slot; const char* name; const char* file; };
    static const SlotFile kSlots[] = {
        {CLIP_IDLE, "Idle", "Creature Pack/mutant idle.fbx"},
        {CLIP_WALK, "Walk", "Creature Pack/mutant walking.fbx"},
        {CLIP_RUN, "Run", "Creature Pack/mutant run.fbx"},
        {CLIP_JUMP, "Jump", "Creature Pack/mutant jumping.fbx"},
        // Fall clip: creature pack doesn't provide a dedicated fall; the Jump
        // clip covers the entire airborne arc via pose-follow / loop.
        {CLIP_CROUCH, "Crouch", "Crouching.fbx"},
        {CLIP_CROUCH_WALK, "CrouchWalk", "chrouchWalk.fbx"},
    };

    for (const auto& s : kSlots) {
        std::unique_ptr<Animation> clip(LoadClipFromFile(dir + "/" + s.file));
        if (!clip) continue;
        if (clip->duration <= 0.0f || clip->duration > 60.0f) continue;
        if (clip->boneAnimations.empty()) continue;

        if (s.slot == CLIP_IDLE) {
            // Trim the long ambient Idle (~16.6s) to ~5s so the motion
            // database doesn't waste ~1200 near-identical poses on it.
            idleClipDuration_ = std::min(clip->duration, 5.0f);
            if (clip->duration > 5.0f) {
                TrimClipTo(*clip, 5.0f);
                clip->duration = 5.0f;
            }
        }
        clip->name = s.name;   // canonical names: DB tags jump/fall as airborne
        clip->speed = 1.0f;
        clips_[s.slot] = std::move(clip);
        clipCount_++;
    }

    if (clips_[CLIP_IDLE]) {
        state_ = AnimationState::IDLE;
        activeClipIndex_ = CLIP_IDLE;
    }

    computeClipSpeeds();

    // Build the per-context databases. Only the DEFAULT locomotion context is
    // built eagerly (it's the active search domain from frame one); crouch /
    // combat / capoeira / dance build lazily on first request, per the
    // suggestions' "only load what the active context needs".
    clipsDir_ = dir;
    buildContextDatabases(dir);
    motionContext_ = MotionContext::LOCOMOTION;
    wasCrouching_ = false;

    // Build the motion-matching database over the loaded clips.
    mmReady_ = false;
    if (clipCount_ > 0 && skeleton_) {
        matcher_ = std::make_unique<MotionMatcher>();
        matcher_->Initialize(skeleton_.get(), animator_.get());
        for (int i = 0; i < CLIP_SLOTS; ++i) {
            if (!clips_[i]) continue;
            // Non-owning alias: the database lives inside matcher_, which is
            // destroyed before clips_, so the alias never dangles.
            matcher_->LoadAnimation(clips_[i]->name,
                std::shared_ptr<Animation>(clips_[i].get(), [](Animation*) {}));
        }
        matcher_->BuildSearchIndex();
        mmReady_ = matcher_->IsInitialized() && matcher_->GetDatabase() &&
                   matcher_->GetDatabase()->GetPoseCount() > 0;

        // Register a profiling provider so the Profiler pulls MotionMatcher
        // stats (KD-tree geometry, search timing, SIMD status, memory) once
        // per endFrame() and forwards them to the console flush + UI panel.
        registerProfilingProvider();

        // FIX (todo §2): Activate the ENRICHED locomotion context (all 10 clips,
        // including Run/Jump/turns/run-look-back) as the default search domain
        // so MM handles the extra locomotion clips out of the box. Use the zero-
        // allocation SetDatabaseExplicit + pre-built KD-tree + pre-warmed SIMD
        // cache instead of SetDatabase(…, 0.0f) which rebuilt the tree on every
        // activation (the crouch↔run FPS dip source).
        if (contextDatabases_[(int)MotionContext::LOCOMOTION] &&
            contextDatabases_[(int)MotionContext::LOCOMOTION]->GetPoseCount() > 0 &&
            contextSearchTrees_[(int)MotionContext::LOCOMOTION]) {
            matcher_->SetDatabaseExplicit(
                *contextDatabases_[(int)MotionContext::LOCOMOTION],
                *contextSearchTrees_[(int)MotionContext::LOCOMOTION]);
        }
    }

    // ---- Structural Motion Graph: Build pre-computed transition clips ----
    // Pre-compute smooth transition clips between all built context databases.
    // This replaces the instant SetDatabaseExplicit swap at runtime with
    // smooth all-joint slerp blends (eq. 6, 7) + 2D alignment (eq. 1).
    buildTransitionGraph();

    return clipCount_;
}

inline void AnimatedCharacter::buildTransitionGraph() {
    if (!skeleton_) return;

    // Build a map of int -> MotionDatabase* for all contexts that have
    // databases built.
    std::map<int, MotionDatabase*> dbMap;
    for (int i = 0; i < static_cast<int>(MotionContext::COUNT); ++i) {
        if (contextDatabases_[i] && contextDatabases_[i]->GetPoseCount() > 0) {
            dbMap[i] = contextDatabases_[i].get();
        }
    }

    if (dbMap.size() >= 2) {
        std::cout << "[AnimatedCharacter] Building transition graph for "
                  << dbMap.size() << " contexts...\n";
        transitionGraph_.BuildTransitions(dbMap, skeleton_.get());
        transitionGraph_.PruneGraph();
        transitionGraph_.PrintGraph();
    } else {
        std::cout << "[AnimatedCharacter] Only " << dbMap.size()
                  << " contexts built - skipping transition graph\n";
    }
}

inline void AnimatedCharacter::registerProfilingProvider() {
    // Push a single MotionMatcher stats snapshot + MemoryTracker stats into
    // the Profiler.  The callback fires every endFrame() so the console
    // flush and UI panel can display live values.
    Profiler::Instance().registerProvider([this](Profiler& p) {
        if (!matcher_) return;

        MotionMatcher::Stats ms = matcher_->GetStats();

        // --- KD-TREE stats ---
        p.setStat("KDTree.TotalNodes",   static_cast<double>(ms.kdTreeNodes),  "nodes");
        p.setStat("KDTree.LeafNodes",    static_cast<double>(ms.kdTreeLeaves), "leaves");
        p.setStat("KDTree.MaxDepth",     static_cast<double>(ms.kdTreeMaxDepth), "max");
        p.setStat("KDTree.AvgLeafSize",  static_cast<double>(ms.kdTreeAvgLeafSize), "poses/leaf");
        p.setStat("KDTree.InternalNodes",static_cast<double>(ms.kdTreeInternal), "internal");

        // --- Database scale ---
        p.setStat("MotionDB.Poses",      static_cast<double>(ms.poseCount),     "poses");
        p.setStat("MotionDB.Animations", static_cast<double>(ms.animationCount), "clips");

        // --- Search metrics ---
        p.setStat("MotionMatch.SearchTimeMs", static_cast<double>(ms.searchTimeMs), "ms");
        p.setStat("MotionMatch.PosesSearched", static_cast<double>(ms.posesSearched), "poses");

        // --- SIMD status ---
        p.setStat("SIMD.AVX2_Available", ms.simdAvailable ? 1.0 : 0.0,
                  ms.simdAvailable ? "AVX2" : "no-AVX2");
        p.setStat("SIMD.AVX2_Active",    ms.simdActive ? 1.0 : 0.0,
                  ms.simdActive ? "on" : "off");

        // --- Current pose ---
        p.setStat("MotionMatch.QuerySpeed", ms.querySpeed, "m/s");
        // Use string stat for the clip name (not a number)
        p.setStringStat("MotionMatch.CurrentClip", ms.currentClip);

        // --- MemoryTracker stats ---
        MemoryTracker& mt = MemoryTracker::getInstance();
        p.setStat("Memory.AllocatedBytes",    static_cast<double>(mt.getCurrentAllocatedBytes()), "bytes");
        p.setStat("Memory.AllocationCount",   static_cast<double>(mt.getAllocationCount()), "allocs");
        p.setStat("Memory.DeallocationCount", static_cast<double>(mt.getDeallocationCount()), "deallocs");
    });
}

inline void AnimatedCharacter::update(float dt, const CharacterInput& input,
                                      const std::function<float(float, float)>& terrain) {
    if (!loaded_ || !animator_) return;
    if (dt < 0.0f) dt = 0.0f;

    // ---- Decode input ----
    glm::vec2 moveDir = input.moveDirection;
    const float moveLen = glm::length(moveDir);
    if (moveLen > 0.001f) moveDir /= moveLen;
    const float moveMag = glm::clamp(input.moveMagnitude, 0.0f, 1.0f);
    lastMoveDir_ = moveDir;
    crouching_ = input.crouch;
    sprinting_ = input.sprint && !crouching_ && moveMag > 0.01f;

    // ---- Contextual database switching ----
    // An explicit context request (editor keys 1-5 / CharacterInput.motion
    // Context) wins; the crouch input auto-switches Locomotion<->Crouch so the
    // dedicated crouch database drives crouching (swap the whole database on
    // input, per the suggestions). Both are edge-triggered - a held key or
    // sustained crouch only switches once, and switching away from combat/etc.
    // requires an explicit key.
    //
    // Cooldown: while a transition clip is playing OR the context-switch
    // cooldown is active, ignore new switch requests to prevent rapid toggling
    // (e.g. double-tap C → conflicting transition clips → leg flexing).
    if (contextSwitchCooldown_ > 0.0f) {
        contextSwitchCooldown_ -= dt;
        if (contextSwitchCooldown_ < 0.0f) contextSwitchCooldown_ = 0.0f;
    }
    const bool canSwitchContext = (contextSwitchCooldown_ <= 0.0f) &&
                                  !transitionClipPlaying_;

    if (canSwitchContext && input.motionContext >= 0 &&
        input.motionContext < (int)MotionContext::COUNT) {
        setMotionContext(static_cast<MotionContext>(input.motionContext));
    } else if (canSwitchContext && motionContext_ == MotionContext::LOCOMOTION &&
        crouching_ && !wasCrouching_) {
        setMotionContext(MotionContext::CROUCH);
    } else if (canSwitchContext && motionContext_ == MotionContext::CROUCH &&
        !crouching_ && wasCrouching_) {
        setMotionContext(MotionContext::LOCOMOTION);
    }
    wasCrouching_ = crouching_;

    // ---- Face the movement direction (local -Z is forward) ----
    if (moveMag > 0.01f) {
        const float targetHeading = std::atan2(-moveDir.x, -moveDir.y);
        float diff = targetHeading - heading;
        while (diff > kPi) diff -= 2.0f * kPi;
        while (diff < -kPi) diff += 2.0f * kPi;
        heading += diff * std::min(1.0f, 10.0f * dt);
    }

    // ---- Horizontal speed (walk / jog / run / crouch bands) ----
    // Speed caps aligned to industry-standard mocap gait speeds:
    //   Walk:   1.5 m/s  (was 2.0 — too high, caused matcher to select jog/run
    //                    clips since walk clips are typically 1.2–1.5 m/s)
    //   Sprint: 6.0 m/s  (run/sprint range 5.5–6.5)
    //   Crouch: 1.45 m/s (crouch-walk, slightly slower than standing walk)
    float speedCap = 1.5f;
    if (sprinting_) speedCap = 6.0f;
    else if (crouching_) speedCap = 1.45f;

    const float targetSpeed = speedCap * moveMag;

    // During combat attacks, WASD does NOT procedurally drive the capsule.
    // AAA Root Motion rule: the root bone delta extracted from the FBX clip
    // drives all movement.  WASD only selects the attack direction (directional
    // attacks) and the facing.  Zeroing horizontal velocity here prevents the
    // "skating" artifact the old kCombatMoveScale caused.
    const bool inCombatAction = combatActionActive_ && combatClip_;
    const float horizSpeed = glm::length(glm::vec2(velocity.x, velocity.z));
    const float accel = crouching_ ? 15.0f : 25.0f;  // m/s² (was 10/16 exponential)
    const float maxDelta = accel * dt;
    float velDelta = targetSpeed - horizSpeed;
    velDelta = std::clamp(velDelta, -maxDelta, maxDelta);
    const float newHoriz = horizSpeed + velDelta;
    velocity.x = moveDir.x * newHoriz;
    velocity.z = moveDir.y * newHoriz;
    if (inCombatAction) {
        // Combat action: nullify procedural WASD velocity.  If the clip has
        // root bone translation, extractCombatRootMotion() applies the
        // authored delta.  If the clip is in-place (no root motion), the
        // character stays planted — NO sliding regardless of WASD input.
        velocity.x = 0.0f;
        velocity.z = 0.0f;
    }

    // ---- Jump / gravity ----
    // FIX (todo §1 jump cycle): Three interlocked problems solved here:
    //   1. DOUBLE JUMP — input.jump is a held button (key-down), so checking
    //      it every grounded frame fires repeatedly.  Edge-detect: only take
    //      off on the RISING edge (input.jump && !prevJumpInput_).
    //   2. Coyote time — a press up to kCoyoteTime after walking off a ledge
    //      still jumps, forgiving sub-frame-perfect timing.
    //   3. Jump buffer — a press made mid-air is queued for kJumpBufferTime;
    //      it fires the instant the character touches down, so jumping just
    //      before landing still lifts off instead of being swallowed.
    const bool mmCanDriveAir = isMotionMatchingActive() && matcher_ && matcher_->HasAirbornePoses();

    // Track coyote time: refreshed while grounded, counts down airborne.
    if (grounded) {
        coyoteTimeLeft_ = kCoyoteTime;
    } else {
        coyoteTimeLeft_ = std::max(0.0f, coyoteTimeLeft_ - dt);
    }
    // Track jump buffer: refreshed while jump is held, counts down airborne.
    // This lets a press made just before landing (or just as the character
    // leaves the ground) fire at the next valid opportunity.
    if (input.jump) {
        jumpBufferLeft_ = kJumpBufferTime;
    } else if (jumpBufferLeft_ > 0.0f) {
        jumpBufferLeft_ = std::max(0.0f, jumpBufferLeft_ - dt);
    }
    const bool jumpPressed = input.jump && !prevJumpInput_;
    prevJumpInput_ = input.jump;

    if (grounded) {
        velocity.y = 0.0f;
        // Jump fires on the rising edge of the button.  When grounded this
        // is always allowed (coyoteTimeLeft_ >= 0 fresh from the block above).
        // When airborne but within coyote time (just left ground), the same
        // check fires from the else-branch below.
        if (jumpPressed) {
            velocity.y = jumpSpeed_;
            grounded = false;
            lastAirborneState_ = AnimationState::JUMP;  // fresh takeoff = JUMP
            // Legacy one-shot jump timer: only used when the pose search can't
            // represent the arc (no Jump/Fall poses in the database).  Duration
            // matches the actual physics air time (~1.1s at 5.5 m/s / 9.8 m/s²)
            // so mmJumping() stays consistent through the full arc — the old
            // 0.4s value expired mid-flight, leaving the legacy path with a
            // stale "not jumping" flag that let Idle leak in.
            legacyJumpTimer_ = (!mmCanDriveAir)
                ? (2.0f * jumpSpeed_ / gravity_ + 0.1f) : 0.0f;
            // Landing recovery: hold the airborne state label briefly before
            // the FSM is allowed to pick IDLE/WALK, preventing an instant
            // Fall→Idle snap that looks like the character "skips landing".
            landingRecoveryLeft_ = kLandingRecovery;
            coyoteTimeLeft_ = 0.0f;  // consumed
            jumpBufferLeft_ = 0.0f;  // consumed
        }
    } else {
        // Airborne: coyote-time catch — a rising-edge press within the grace
        // window (just left ground) still takes off, forgiving sub-frame-perfect
        // timing on platform edges.
        if (jumpPressed && coyoteTimeLeft_ > 0.0f) {
            velocity.y = jumpSpeed_;
            lastAirborneState_ = AnimationState::JUMP;
            legacyJumpTimer_ = (!mmCanDriveAir)
                ? (2.0f * jumpSpeed_ / gravity_ + 0.1f) : 0.0f;
            landingRecoveryLeft_ = kLandingRecovery;
            coyoteTimeLeft_ = 0.0f;
            jumpBufferLeft_ = 0.0f;
        }
        velocity.y -= gravity_ * dt;
        if (legacyJumpTimer_ > 0.0f) legacyJumpTimer_ -= dt;
    }

    // ---- Integrate ----
    position += velocity * dt;

    // ---- Root Motion Extraction (combat attacks) ----
    // AAA rule: during combat, the capsule position is driven by the root bone's
    // baked delta from the FBX, NOT by procedural WASD velocity.  This eliminates
    // the skating/sliding artifact where the character slides forward regardless
    // of whether the animation actually moves the feet.
    // Phase 2+3 pipeline (per todo design doc):
    //   2. Trajectory Extraction — extract raw root bone displacement
    //   3. Warping Transform — apply target magnetism if an enemy is nearby
    if (combatActionActive_ && combatClip_ && animator_) {
        // 1. Extract raw displacement from the FBX root bone
        glm::vec3 worldDelta = ComputeRawAssetRootDisplacement(dt);

        // 2. Apply target warping / soft magnetism
        //    If no explicit target was set, query the nearest enemy via
        //    the Intent Matrix + LOS filtered spatial query.
        if (!combatHasTarget_) {
            combatHasTarget_ = EngineQueryClosestLivingEnemy(
                position, combatMoveDir_, 5.5f, combatTarget_);
        }
        if (combatHasTarget_) {
            ApplyTargetWarpingToRootMotion(worldDelta, dt, combatTarget_);
        }

        // 3. Apply definitive root motion to character position
        position.x += worldDelta.x;
        position.z += worldDelta.z;
    }

    // ---- Terrain snap ----
    const float floorY = terrain ? terrain(position.x, position.z) : position.y;
    if (grounded) {
        position.y = floorY;
    } else if (position.y <= floorY) {
        position.y = floorY;
        velocity.y = 0.0f;
        // Jump buffer: if a press was queued mid-air, fire it immediately on
        // landing so the character doesn't "stick" to the ground when the
        // player was clearly trying to jump.
        if (jumpBufferLeft_ > 0.0f) {
            grounded = false;
            velocity.y = jumpSpeed_;
            lastAirborneState_ = AnimationState::JUMP;
            legacyJumpTimer_ = (!mmCanDriveAir)
                ? (2.0f * jumpSpeed_ / gravity_ + 0.1f) : 0.0f;
            landingRecoveryLeft_ = kLandingRecovery;
            jumpBufferLeft_ = 0.0f;
            coyoteTimeLeft_ = 0.0f;
        } else {
            grounded = true;
        }
    }

    // ---- Locomotion FSM ----
    const float speed2d = glm::length(glm::vec2(velocity.x, velocity.z));
    // FIX (todo §1): Hysteresis on the JUMP→FALL transition.  The old single
    // `velocity.y > 0.5f` threshold flipped state as soon as upward velocity
    // dropped below 0.5 m/s — but gravity crosses 0.5 m/s/symmetrically, so a
    // character rising *through* 0.5 m/s (ascent) and falling *toward* 0.5 m/s
    // (descent) both trigger the flip near the apex, causing JUMP↔FALL flicker
    // on 1-2 frames of float noise.  Hysteresis: only switch to FALL once
    // descending past -kJumpApexHysteresis, and only switch back to JUMP once
    // ascending past +kJumpApexHysteresis — the state is sticky through the
    // flat apex zone.
    if (!grounded) {
        if (velocity.y > kJumpApexHysteresis) {
            lastAirborneState_ = AnimationState::JUMP;
        } else if (velocity.y < -kJumpApexHysteresis && clips_[CLIP_FALL]) {
            // Only flip to FALL when the clip is actually available.  The Fall
            // clip is currently removed (see kSlots in loadLocomotion) to
            // eliminate mid-air JUMP↔FALL clip mismatch — the Jump clip covers
            // the entire arc.  When Fall is re-added, the hysteresis gate
            // re-activates automatically.
            lastAirborneState_ = AnimationState::FALL;
        }
        // Between -kJumpApexHysteresis and +kJumpApexHysteresis (near apex):
        // keep lastAirborneState_ — sticky, no flip.
        state_ = lastAirborneState_;
    } else if (landingRecoveryLeft_ > 0.0f && isMotionMatchingActive()) {
        // Landing recovery: hold the last airborne state (FALL) briefly so the
        // motion matcher can finish its landing crossfade.  Cutting straight to
        // IDLE/WALK here is what caused the "jump → instant idle (no fall/land)"
        // jank the animator saw.  During recovery the rest-override (which keys
        // on IDLE/CROUCH + grounded) doesn't fire, so the Fall pose holds.
        landingRecoveryLeft_ -= dt;
        state_ = lastAirborneState_;
    } else if (crouching_) {
        state_ = (speed2d > 0.2f) ? AnimationState::CROUCH_WALK : AnimationState::CROUCH;
    } else if (sprinting_) {
        state_ = AnimationState::RUN;
    } else if (speed2d > 0.2f) {
        state_ = AnimationState::WALK;
    } else {
        state_ = AnimationState::IDLE;
    }

    // ---- Combat / dance action playback ----
    // COMBAT context uses an explicit combo chain (not pose search): each
    // attack press advances the chain, movement (WASD) still slides the
    // character, and directional input selects the attack type (forward =
    // JumpAttack, side = Punch, neutral = Swipe). After the final clip or
    // timeout, the character auto-returns to Locomotion.
    const bool attackPressed = input.attack && !prevAttack_;
    prevAttack_ = input.attack;

    if (combatActionActive_) {
        // A 1-shot combat clip is playing.  Count it down.
        combatTimer_ -= dt;
        comboTimer_ -= dt;  // also shrink the input-acceptance window

        // Attack input during the second half of the clip advances the combo.
        // Input in the last kAttackBuffer seconds is queued for the next clip.
        if (attackPressed) {
            if (combatTimer_ <= kAttackBuffer || comboTimer_ > 0.0f) {
                pendingAttack_ = true;
                comboTimer_ = kComboWindow;
                comboStep_ = std::min(comboStep_ + 1, 3);
            } else {
                // Too early — extend the window so the player isn't punished
                // for a slightly-fast tap.
                comboTimer_ = kComboWindow;
                comboStep_ = std::min(comboStep_ + 1, 3);
            }
        }

        if (combatTimer_ <= 0.0f) {
            // Clip finished — either continue the combo or end it.
            if (pendingAttack_ || comboTimer_ > 0.0f) {
                pendingAttack_ = false;
                // Select the next combo clip based on step + direction
                selectCombatClip(combatClipIndex_, comboStep_, combatMoveDir_);
                combatTimer_ = combatClip_->duration;
                comboTimer_ = kComboWindow;
            } else {
                // Combo ended — auto-return to Locomotion.
                endCombatAction();
            }
        }
    } else if (motionContext_ == MotionContext::COMBAT && attackPressed) {
        // Start a new combo (step 1). Capture the movement direction for
        // directional attack selection (forward = JumpAttack, side = Punch).
        combatMoveDir_ = moveDir;
        startCombatAction(moveDir, moveMag);
    }
    if (previewActive_) {
        previewTimer_ -= dt;
        if (moveMag > 0.05f || previewTimer_ <= 0.0f) previewActive_ = false;
    }

    // ---- Continuous animation (no pose freezing) ----
    // REMOVED: Standstill Posture Lock (idle snapshot).
    // Previously froze the skeleton on the first frame of rest to prevent
    // IK-loop knee jitter. Now the structural motion graph handles all state
    // transitions via pre-computed transition clips (slerp + 2D alignment),
    // and idle poses are selected from the motion database each frame.
    // Foot IK runs continuously with sub-frame damping (ApplyFootIK) to
    // eliminate jitter without freezing — all clips flow seamlessly.

    // ---- Drive the animation ----
    if (previewActive_ && previewClipIndex_ >= 0 &&
        previewClipIndex_ < static_cast<int>(clips_.size()) && clips_[previewClipIndex_]) {
        activeClipIndex_ = previewClipIndex_;
        if (animator_->GetCurrentAnimation() != clips_[previewClipIndex_].get()) {
            animator_->Play(clips_[previewClipIndex_].get());
        }
    } else if (combatActionActive_ && combatClip_) {
        // Play the combat/dance one-shot clip directly.
        if (animator_->GetCurrentAnimation() != combatClip_.get()) {
            animator_->Play(combatClip_.get());
        }
    } else if (motionMatchingEnabled && mmReady_) {
        // Hand the terrain heightmap to the matcher -> animator foot IK so feet
        // tilt to ground slopes (todo Part 3, Option A). `terrain` is in scope
        // here (the per-frame integration param at update()).
        matcher_->SetTerrainFn(terrain);
        // Motion matching: SearchAndBlend sets the animation clip/time,
        // but does NOT run foot IK (deferred to after evaluation).
        driveMotionMatching(dt, floorY);
    } else {
        driveFsm(dt);
    }

    // ---- Evaluate the skeleton (advances layer times, computes matrices) ----
    animator_->Update(dt);

    // ---- Foot IK (AFTER skeleton evaluation) -------------------------------
    // Running foot IK + pelvis adjustment here — after Animator::Update
    // computed currBoneWorldPos — eliminates the 1-frame lag that caused
    // jittery legs and stretched knees during pose switches. The IK targets
    // are computed against this frame's bone positions; the offsets are
    // applied on the NEXT frame's EvaluateNode (standard 1-frame IK latency,
    // masked by the smoothed ankle / pelvis interpolation).
    if ((motionMatchingEnabled && mmReady_) && matcher_) {
        // CRITICAL ENGINE ALIGNMENT: Synchronize the model matrix translation
        // directly to the live character velocity profile. Pass horizontal
        // velocity to the animator to enable velocity-robust over-stride
        // releases — the IK solver scales its release threshold with body speed
        // to prevent false releases on high-speed turns.
        animator_->SetCharacterVelocity(velocity);

        // modelMatrix() matches what the renderer uses (180° flip included).
        matcher_->SetCharacterModelMatrix(modelMatrix());
        matcher_->ApplyFootIK(dt);
        // Re-evaluate the skeleton in the SAME frame so the visual pose
        // matches the IK solve (eliminates 1-frame lag → knee jitter during
        // motion-matching pose switches).
        animator_->RevalidateIK();
    }

    // ---- Procedural Foot Planting (tagless) -------------------------------
    // Phase 5: Evaluate foot bone velocity and height relative to capsule.
    // Engage locks dynamically when feet are stationary and near the ground.
    UpdateProceduralFootPlanting(dt);

    // REMOVED: Idle snapshot capture/release (was todo §1).
    // The structural transition graph + continuous foot IK damping handle
    // smooth idle stability without freezing poses.
}

inline void AnimatedCharacter::driveFsm(float dt) {
    (void)dt;
    const int slot = slotForState(state_);
    if (slot < 0 || slot >= static_cast<int>(clips_.size()) || !clips_[slot]) {
        activeClipIndex_ = CLIP_IDLE;
        return;
    }
    activeClipIndex_ = slot;
    Animation* clip = clips_[slot].get();
    // playbackSpeed scales the animator's per-layer clock (layer.time += dt*speed)
    if (std::abs(clip->speed - playbackSpeed) > 1e-4f) clip->speed = playbackSpeed;
    // Crossfade into the new clip instead of the old Play() hard cut, which
    // popped on every transition (Fall->Walk landing, direction reversals).
    // Fire on a state change, OR when some other path (preview clip, motion
    // matcher) hijacked the animator while the FSM was idle - but only when no
    // crossfade is in flight (>=2 layers), since the outgoing layer still owns
    // the highest weight mid-blend and re-blending would restart it.
    const bool fsmStateChanged = state_ != lastFsmState_;
    // Determine the animator's dominant animation by *layer weight* — NOT via
    // GetCurrentAnimation(), which falls back to the stale `current` pointer
    // (nullptr after Play(nullptr) clears the idle snapshot) when every layer's
    // weight is zero.  That fallback caused a false-positive hijacked=true on
    // every frame of a finishing crossfade, re-blending the clip at t=0.0
    // (T-pose pose) indefinitely — the walk snap.
    Animation* domAnim = nullptr;
    {
        float maxW = -1.0f;
        int nLayers = animator_->GetActiveAnimationLayerCount();
        for (int li = 0; li < nLayers; ++li) {
            const auto* l = animator_->GetActiveLayer(li);
            if (l && l->animation && l->weight > maxW) {
                maxW = l->weight;
                domAnim = l->animation;
            }
        }
    }
    const bool hijacked = animator_->GetActiveAnimationLayerCount() <= 1 &&
                          domAnim != clip;
    if (fsmStateChanged || hijacked) {
        lastFsmState_ = state_;
        // Preserve the current playback time instead of hard-resetting to 0.0f.
        // The old 0.0f start yanked the clip back to its entry pose on every
        // false-positive hijack, producing a visible snap to T-pose.  Using
        // the live clip time keeps the new layer phase-aligned.
        animator_->BlendToAt(clip, animator_->GetCurrentTime(), kFsmBlendDuration);
    }
}

inline void AnimatedCharacter::driveMotionMatching(float dt, float floorY) {
    if (!matcher_) return;

    // Feed the matcher the current physics state expressed in MODEL units -
    // the pose database's feature space. The character moves in world units
    // (scale ~0.01 for the ~180-unit bot), so a world-space velocity of 2 m/s
    // must arrive as ~200 model-units/s or the search sees Walk/Run poses as
    // infinitely far away and locks onto Idle.
    const float inv = scale > 0.0001f ? 1.0f / scale : 1.0f;
    CharacterState cs;
    cs.position = position * inv;
    cs.velocity = velocity * inv;
    cs.worldVelocity = velocity;  // Raw world-space m/s for speed-dependent filters
    cs.rotation = heading;
    cs.moveDirection = lastMoveDir_;
    cs.grounded = grounded;
    cs.crouching = crouching_;
    cs.jumping = !grounded && velocity.y > 0.1f;

    // Model-to-world transform so the matcher's foot IK runs in world space
    // (floor height is world units too). Must match the rendered model -
    // modelMatrix() applies the 180-degree facing flip.
    const glm::mat4 modelMat = modelMatrix();

    matcher_->SetFloorHeight(floorY);
    matcher_->SetCharacterModelMatrix(modelMat);

    // ---- Clip Lock Mode for Smooth Idle ----
    // When the character is truly at rest (grounded, zero velocity, no
    // transition playing), engage clip-lock mode so the idle clip plays
    // through its full cycle without pose-search interruptions ("cut mid
    // clip" effect). Resume normal search when the character starts moving.
    //
    // IMPORTANT: only engage clip-lock when the matcher already has a valid
    // current animation. After a transition clip completes, CompleteTransitionNow()
    // sets currentAnimationPtr = nullptr (forcing re-selection). If clip-lock
    // engages before that re-selection happens, the matcher skips the search
    // entirely and the animator is left stuck on the finished transition clip.
    // By requiring IsClipLockReady() (currentAnimationPtr != nullptr), we
    // guarantee the matcher has a valid clip to play before suspending the
    // search — the next frame will engage clip-lock with a real clip loaded.
    const float speed2dWorld = glm::length(glm::vec2(velocity.x, velocity.z));
    const bool atRest = grounded && speed2dWorld < kAtRestThreshold &&
                        !transitionClipPlaying_ && !matcher_->IsPlayingTransitionClip();
    if (atRest && matcher_->IsClipLockReady()) {
        matcher_->SetClipLock(true);
    } else if (matcher_->IsClipLocked()) {
        matcher_->SetClipLock(false);
    }

    matcher_->Update(dt, cs);

    // ---- Structural Motion Graph: Check transition clip completion ----
    if (transitionClipPlaying_ && matcher_ && !matcher_->IsPlayingTransitionClip()) {
        // Transition clip has finished — commit the context switch
        transitionClipPlaying_ = false;
        motionContext_ = transitionTargetCtx_;
        contextSwitchCooldown_ = kContextSwitchCooldown;
        std::cout << "[AnimatedCharacter] Transition clip complete -> "
                  << motionContextName() << " context active\n";

        // --- HIGH-LEVEL ATOMIC PRIMING PASS ---
        // Extract the exact target entry frame from the destination database
        // and push it into the animator layer immediately to prevent a
        // 0-weight fallback (the single-frame T-pose snap). This mirrors
        // the atomic hand-off in HybridMMFSM::Update — both ensure the
        // target animation is at full weight the same frame the transition
        // completes, so there's no frame where totalWeight < 0.0001f.
        auto destinationAnim = matcher_->GetCurrentAnimation();
        if (destinationAnim && animator_) {
            animator_->Play(destinationAnim.get());
            animator_->SetCurrentTime(matcher_->GetCurrentAnimationTime());
        }
        // --------------------------------------
    }

    // Deterministic safety-net overrides on top of the pose search. These are
    // edge-triggered: they blend ONCE, only when the animator/matcher is not
    // already playing the target clip. The old version force-blended every
    // frame, which (a) re-ramped the crossfade each frame (blendProgress
    // reset -> the blend never settled), (b) yanked the clip time to
    // GetCurrentTime() mid-blend (gait-phase mismatch / footskate), and
    // (c) spammed the console via the Animator's [BlendTo] prints - a visible
    // FPS drop + pose mismatch whenever the bot crouched or stood still. With
    // the contextual databases the matcher already picks the right gait (the
    // crouch DB contains only Crouch/CrouchWalk), so this is a safety net,
    // not a per-frame driver.
    // Deterministic safety-net overrides on top of the pose search. These are
    // edge-triggered: they blend ONCE, only when the animator/matcher is not
    // already playing the target clip. The old version force-blended every
    // frame, which (a) re-ramped the crossfade each frame (blendProgress
    // reset -> the blend never settled), (b) yanked the clip time to
    // GetCurrentTime() mid-blend (gait-phase mismatch / footskate), and (c)
    // spammed the console via the Animator's [BlendTo] prints - a visible
    // FPS drop + pose mismatch whenever the bot crouched or stood still. With
    // the contextual databases the matcher already picks the right gait (the
    // crouch DB contains only Crouch/CrouchWalk), so this is a safety net,
    // not a per-frame driver.
    //
    // DEFERRED: when clip-lock mode is active (idle) or a transition clip is
    // playing, the safety net must NOT run — it would override the clip-lock's
    // time advancement with BlendToAt calls, cutting the idle mid-cycle, and
    // it would interfere with transition clip playback.
    const bool matcherPlayingTransition =
        matcher_ && matcher_->IsPlayingTransitionClip();
    const bool clipLocked = matcher_ && matcher_->IsClipLocked();
    // CRITICAL HIGH-LEVEL PROTECTION GATE:
    // Completely disable safety-net overrides if a transition clip is active
    // OR the matcher is structurally blending joints via its internal
    // transition clip layer. The (motionContext_ == transitionTargetCtx_)
    // check ensures the context has fully settled — during a transition the
    // contexts mismatch and the safety net would override the blend mid-stride.
    const bool safetyNetActive = !clipLocked &&
                                 !transitionClipPlaying_ &&
                                 !matcherPlayingTransition &&
                                 (motionContext_ == transitionTargetCtx_);
    const bool locomotionLike =
        motionContext_ == MotionContext::LOCOMOTION ||
        motionContext_ == MotionContext::CROUCH;
    const float speed2d = glm::length(glm::vec2(velocity.x, velocity.z));
    if (safetyNetActive && grounded && locomotionLike) {
        const int slot =
            crouching_
                ? ((speed2d > 0.2f) ? CLIP_CROUCH_WALK : CLIP_CROUCH)
                : ((speed2d < 0.1f && motionContext_ == MotionContext::LOCOMOTION)
                       ? CLIP_IDLE : -1);
        if (slot >= 0 && slot < static_cast<int>(clips_.size()) && clips_[slot]) {
            Animation* want = clips_[slot].get();
            // Only blend if neither the matcher nor the animator's dominant
            // layer is already this clip (the context DB aliases the slot
            // pointers, so a matcher-driven pose satisfies this check).
            const bool matcherOnIt =
                matcher_ && matcher_->GetCurrentAnimation().get() == want;
            const bool animatorOnIt =
                animator_->GetCurrentAnimation() == want;
            if (!matcherOnIt && !animatorOnIt) {
                animator_->BlendToAt(want, animator_->GetCurrentTime(), 0.2f);
            }
        }
    }

    // Exposed "active" clip follows the FSM state (deterministic); the
    // animator's actual dominant layer is the matcher's choice.
    activeClipIndex_ = slotForState(state_);
}

inline std::string AnimatedCharacter::activeClipName() const {
    if (activeClipIndex_ >= 0 && activeClipIndex_ < static_cast<int>(clips_.size()) &&
        clips_[activeClipIndex_]) {
        return clips_[activeClipIndex_]->name;
    }
    return "";
}

inline std::string AnimatedCharacter::mmActiveClip() const {
    if (!matcher_) return "";
    auto anim = matcher_->GetCurrentAnimation();
    return anim ? anim->name : "";
}

inline AnimatedCharacter::PoseDiag AnimatedCharacter::debugPoseDiag() const {
    PoseDiag d;
    if (!animator_) return d;

    // Dominant animator layer.
    const int n = animator_->GetActiveAnimationLayerCount();
    int dom = -1;
    float maxW = -1.0f;
    for (int i = 0; i < n; ++i) {
        const auto* l = animator_->GetActiveLayer(i);
        if (l && l->animation && l->weight > maxW) {
            maxW = l->weight;
            dom = i;
        }
    }
    if (dom >= 0) {
        const auto* l = animator_->GetActiveLayer(dom);
        d.dominantRawName = l->animation->name;
        d.dominantWeight = l->weight;
        d.dominantTime = l->time;
    }

    // Matcher clip; dominant-matches-matcher is a pointer-identity check.
    Animation* matcherAnim = nullptr;
    if (matcher_) {
        auto anim = matcher_->GetCurrentAnimation();
        matcherAnim = anim.get();
        if (anim) d.matcherClip = anim->name;
    }
    if (dom >= 0 && matcherAnim) {
        d.dominantMatchesMatcher = animator_->GetActiveLayer(dom)->animation == matcherAnim;
    }

    // Foot world positions + plant locks.
    const glm::mat4 ident(1.0f);
    const glm::mat4 modelMat = modelMatrix();
    if (skeleton_) {
        int lf = skeleton_->GetBoneIndex("leftfoot");
        int rf = skeleton_->GetBoneIndex("rightfoot");
        if (lf < 0) lf = skeleton_->GetBoneIndex("LeftFoot");
        if (rf < 0) rf = skeleton_->GetBoneIndex("RightFoot");
        if (lf >= 0) d.leftFoot = animator_->GetBoneWorldPosition(lf, ident);
        if (rf >= 0) d.rightFoot = animator_->GetBoneWorldPosition(rf, ident);

        // Computed visual knee angles from the RENDERED bone positions (IK
        // already applied by Animator::Update). This is what the user sees.
        int lh = animator_->footIKSettings.leftUpLegBone;
        int lk = animator_->footIKSettings.leftLegBone;
        int rh = animator_->footIKSettings.rightUpLegBone;
        int rk = animator_->footIKSettings.rightLegBone;
        if (lh >= 0 && lk >= 0) {
            glm::vec3 h = animator_->GetBoneWorldPosition(lh, modelMat);
            glm::vec3 k = animator_->GetBoneWorldPosition(lk, modelMat);
            glm::vec3 a = animator_->GetBoneWorldPosition(lf, modelMat);
            glm::vec3 td = h - k, sd = a - k;
            float L = glm::length(td), S = glm::length(sd);
            if (L > 1e-4f && S > 1e-4f)
                d.leftKneeDeg = glm::degrees(acosf(glm::clamp(glm::dot(td, sd) / (L * S), -1.0f, 1.0f)));
        }
        if (rh >= 0 && rk >= 0) {
            glm::vec3 h = animator_->GetBoneWorldPosition(rh, modelMat);
            glm::vec3 k = animator_->GetBoneWorldPosition(rk, modelMat);
            glm::vec3 a = animator_->GetBoneWorldPosition(rf, modelMat);
            glm::vec3 td = h - k, sd = a - k;
            float L = glm::length(td), S = glm::length(sd);
            if (L > 1e-4f && S > 1e-4f)
                d.rightKneeDeg = glm::degrees(acosf(glm::clamp(glm::dot(td, sd) / (L * S), -1.0f, 1.0f)));
        }
    }
    d.leftLocked = animator_->leftFootIK.isLocked;
    d.rightLocked = animator_->rightFootIK.isLocked;
    d.leftLockWeight  = animator_->leftFootIK.lockWeight;
    d.rightLockWeight = animator_->rightFootIK.lockWeight;
    // Ankle offset is in model space; convert to world metres.
    const float wscale = animator_->getIKWorldScale();
    const float iscale = (wscale > 0.0f) ? wscale : 1.0f;
    d.leftAnkleOffY_m  = animator_->leftFootIK.ankleOffset.y * iscale;
    d.rightAnkleOffY_m = animator_->rightFootIK.ankleOffset.y * iscale;
    d.pelvisDropY = animator_->getPelvisDropY();
    d.leftLegReach  = animator_->getLeftLegReach();
    d.rightLegReach = animator_->getRightLegReach();
    d.isMoving = (glm::length(glm::vec2(velocity.x, velocity.z)) > 0.1f);
    return d;
}

inline float AnimatedCharacter::debugClipSpeed(int index) const {
    if (index < 0 || index >= static_cast<int>(clipSpeeds_.size())) return 1.0f;
    if (index == CLIP_IDLE) return 1.0f;  // Idle always real-time
    const float clipSpeed = clipSpeeds_[index];
    const float modelSpd = currentSpeed() / (scale > 0.0001f ? scale : 1.0f);
    if (clipSpeed <= 0.01f || modelSpd <= 0.01f) return 1.0f;
    // Anti-footskate rate: play the clip at physicsSpeed/clipStrideSpeed so a
    // stride's ground coverage matches the character's actual movement.
    // clipSpeeds_ are model-unit strides; modelSpd is the model-unit speed.
    return glm::clamp(modelSpd / clipSpeed, 0.6f, 1.5f);
}

inline int AnimatedCharacter::slotForState(AnimationState s) const {
    switch (s) {
        case AnimationState::IDLE: return CLIP_IDLE;
        case AnimationState::WALK: return CLIP_WALK;
        case AnimationState::RUN: return CLIP_RUN;
        case AnimationState::JUMP: return CLIP_JUMP;
        case AnimationState::FALL: return CLIP_FALL;
        case AnimationState::CROUCH: return CLIP_CROUCH;
        case AnimationState::CROUCH_WALK: return CLIP_CROUCH_WALK;
        default: return CLIP_IDLE;
    }
}

inline const BoneAnimation* AnimatedCharacter::findRootBone(const Animation* clip) const {
    if (!clip) return nullptr;
    // Prefer the canonical root/hips channel (the skeleton's rootBoneIndex is
    // set to Hips in load(), and the clips' channels are normalized).
    if (skeleton_ && skeleton_->rootBoneIndex >= 0) {
        for (const auto& kv : skeleton_->boneMapping) {
            if (kv.second == skeleton_->rootBoneIndex) {
                const BoneAnimation* ba = clip->GetBoneAnimation(kv.first);
                if (ba) return ba;
            }
        }
    }
    for (const char* name : {"hips", "root", "pelvis"}) {
        const BoneAnimation* ba = clip->GetBoneAnimation(name);
        if (ba && ba->positionTimes.size() >= 2) return ba;
    }
    for (const auto& kv : clip->boneAnimations) {
        if (kv.second.positionTimes.size() >= 2) return &kv.second;
    }
    return nullptr;
}

inline void AnimatedCharacter::computeClipSpeeds() {
    clipSpeeds_.assign(CLIP_SLOTS, 0.0f);
    for (int i = 0; i < CLIP_SLOTS; ++i) {
        if (!clips_[i]) continue;
        const Animation* clip = clips_[i].get();
        const BoneAnimation* ba = findRootBone(clip);
        if (!ba || ba->positionTimes.size() < 2) continue;
        float total = 0.0f;
        int count = 0;
        for (size_t k = 1; k < ba->positionTimes.size(); ++k) {
            const float dt = static_cast<float>(ba->positionTimes[k] - ba->positionTimes[k - 1]);
            if (dt <= 0.0f) continue;
            const glm::vec3 d = ba->positionValues[k] - ba->positionValues[k - 1];
            total += glm::length(glm::vec2(d.x, d.z)) / dt;
            ++count;
        }
        if (count > 0) clipSpeeds_[i] = total / static_cast<float>(count);
    }
}

// ============================================================================
// Motion contexts - contextual database switching
// ============================================================================

inline void AnimatedCharacter::addClipToContext(MotionContext ctx, const std::string& name,
                                                const std::string& file,
                                                const std::string& dir) {
    const int idx = static_cast<int>(ctx);
    if (idx < 0 || idx >= static_cast<int>(MotionContext::COUNT)) return;
    if (!contextDatabases_[idx]) contextDatabases_[idx] = std::make_unique<MotionDatabase>();

    // Clips already loaded into the fixed FSM slots are ALIASED (shared_ptr
    // with a no-op deleter) so the FBX is never parsed twice. Safe: the
    // context databases are destroyed before clips_.
    static const std::pair<const char*, ClipSlot> kSlotFiles[] = {
        {"Creature Pack/mutant idle.fbx", CLIP_IDLE},
        {"Creature Pack/mutant walking.fbx", CLIP_WALK},
        {"Creature Pack/mutant run.fbx", CLIP_RUN},
        {"Creature Pack/mutant jumping.fbx", CLIP_JUMP},
        {"Crouching.fbx", CLIP_CROUCH},
        {"chrouchWalk.fbx", CLIP_CROUCH_WALK},
    };
    for (const auto& sf : kSlotFiles) {
        if (file != sf.first || !clips_[sf.second]) continue;
        contextDatabases_[idx]->AddAnimation(
            name, std::shared_ptr<Animation>(clips_[sf.second].get(), [](Animation*) {}),
            skeleton_.get());
        return;
    }

    // Fresh load - the context database owns the clip via shared_ptr.
    std::unique_ptr<Animation> raw(LoadClipFromFile(dir + "/" + file));
    if (!raw) return;
    if (raw->duration <= 0.0f || raw->duration > 60.0f) return;
    if (raw->boneAnimations.empty()) return;
    if (name == "Idle" && raw->duration > 5.0f) {
        // Match the slot loader's trim so the DB doesn't waste poses on the
        // long ambient idle.
        TrimClipTo(*raw, 5.0f);
        raw->duration = 5.0f;
    }
    raw->name = name;   // canonical name: the DB tags Jump/Fall as airborne
    raw->speed = 1.0f;
    contextDatabases_[idx]->AddAnimation(
        name, std::shared_ptr<Animation>(std::move(raw)), skeleton_.get());
}

inline void AnimatedCharacter::buildContextDatabases(const std::string& dir) {
    for (auto& db : contextDatabases_) db.reset();  // fresh load
    for (auto& tree : contextSearchTrees_) tree.reset();  // discard stale trees
    // Only the DEFAULT search domain is built eagerly - the other contexts
    // (crouch/combat/capoeira/dance) load lazily on first request, per the
    // suggestions' "only load what the active context needs".
    ensureContextBuilt(MotionContext::LOCOMOTION);
    // Also eagerly build Crouch since walk↔crouch is the most common
    // transition and the transition graph needs both endpoints at load time.
    ensureContextBuilt(MotionContext::CROUCH);
}

inline void AnimatedCharacter::ensureContextBuilt(MotionContext ctx) {
    const int idx = static_cast<int>(ctx);
    if (idx < 0 || idx >= static_cast<int>(MotionContext::COUNT)) return;
    if (contextDatabases_[idx]) return;  // already built

    struct ContextClip { const char* name; const char* file; };
    static const ContextClip kLocomotion[] = {
        {"Idle", "Creature Pack/mutant idle.fbx"},
        {"Walk", "Creature Pack/mutant walking.fbx"},
        {"Run", "Creature Pack/mutant run.fbx"},
        {"Jump", "Creature Pack/mutant jumping.fbx"},
        {"LeftTurn", "Creature Pack/mutant left turn 45.fbx"},
        {"RightTurn", "Creature Pack/mutant right turn 45.fbx"},
        {"RightTurn90", "Creature Pack/mutant right turn 90.fbx"},
    };
    static const ContextClip kCrouch[] = {
        {"Crouch", "Crouching.fbx"}, {"CrouchWalk", "chrouchWalk.fbx"},
    };
    static const ContextClip kCombat[] = {
        {"Punch", "Creature Pack/mutant punch.fbx"},
        {"Swipe", "Creature Pack/mutant swiping.fbx"},
        {"JumpAttack", "Creature Pack/jump attack.fbx"},
        {"Defeated", "Defeated.fbx"},
    };
    static const ContextClip kDance[] = {
        {"Roar", "Creature Pack/mutant roaring.fbx"},
        {"Flex", "Creature Pack/mutant flexing muscles.fbx"},
        {"Breathing", "Creature Pack/mutant breathing idle.fbx"},
        {"Rumba", "Rumba Dancing.fbx"}, {"HipHop", "Hip Hop Dancing.fbx"},
        {"Capoeira", "Capoeira.fbx"}, {"Taunt", "Taunt.fbx"},
        {"Taunt2", "Taunt(1).fbx"}, {"SittingLaugh", "Sitting Laughing.fbx"},
    };

    switch (ctx) {
        case MotionContext::LOCOMOTION:
            for (const auto& c : kLocomotion) addClipToContext(ctx, c.name, c.file, clipsDir_);
            break;
        case MotionContext::CROUCH:
            for (const auto& c : kCrouch) addClipToContext(ctx, c.name, c.file, clipsDir_);
            break;
        case MotionContext::COMBAT:
            for (const auto& c : kCombat) addClipToContext(ctx, c.name, c.file, clipsDir_);
            break;
        case MotionContext::DANCE:
            for (const auto& c : kDance) addClipToContext(ctx, c.name, c.file, clipsDir_);
            break;
        case MotionContext::CAPOEIRA:
            buildCapoeiraContext(clipsDir_);
            break;
        default:
            break;
    }

    // FIX (todo §2): Pre-build the KD-tree and pre-warm the SIMD SoA cache
    // for this context's database at LOAD TIME.  At runtime setMotionContext()
    // swaps in both via SetDatabaseExplicit (zero allocation), so neither
    // BuildSearchIndex() nor RebuildSIMDCacheIfNeeded() ever allocates mid-frame
    // during a crouch↔run transition.
    if (contextDatabases_[idx] && contextDatabases_[idx]->GetPoseCount() > 0 &&
        !contextSearchTrees_[idx]) {
        contextSearchTrees_[idx] = std::make_unique<MotionKDTree>();
        contextSearchTrees_[idx]->BuildWithSAH(contextDatabases_[idx]->GetPoses(), 10);
        contextDatabases_[idx]->RebuildSIMDCacheIfNeeded();  // pre-bake SoA snapshot
    }
}

inline void AnimatedCharacter::buildCapoeiraContext(const std::string& dir) {
    const int idx = static_cast<int>(MotionContext::CAPOEIRA);
    if (contextDatabases_[idx]) return;  // already built
    contextDatabases_[idx] = std::make_unique<MotionDatabase>();

    const std::string folder = dir + "/Capoeira animiation clips";
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) {
        std::cerr << "[AnimatedCharacter] Capoeira clip folder not found: " << folder << "\n";
        return;
    }

    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (ec) { ec.clear(); continue; }
        if (!entry.is_regular_file(ec)) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".fbx") continue;
        files.push_back(entry.path().filename().string());
    }
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
        std::unique_ptr<Animation> raw(LoadClipFromFile(folder + "/" + file));
        if (!raw) continue;   // skips Ch41_nonPBR.fbx (a model, no animations)
        if (raw->duration <= 0.0f || raw->duration > 60.0f) continue;
        if (raw->boneAnimations.empty()) continue;
        const std::string stem = std::filesystem::path(file).stem().string();
        raw->name = stem;     // canonical: filename stem (e.g. "Ginga forward")
        raw->speed = 1.0f;
        contextDatabases_[idx]->AddAnimation(
            stem, std::shared_ptr<Animation>(std::move(raw)), skeleton_.get());
    }
    std::cout << "[AnimatedCharacter] Capoeira context built: "
              << contextDatabases_[idx]->GetAnimationCount() << " clips, "
              << contextDatabases_[idx]->GetPoseCount() << " poses\n";
}

inline void AnimatedCharacter::setMotionContext(MotionContext ctx, float blendDuration) {
    if (!matcher_ || !mmReady_) return;
    const int idx = static_cast<int>(ctx);
    if (idx < 0 || idx >= static_cast<int>(MotionContext::COUNT)) return;
    (void)blendDuration;  // retained for API compatibility; transition clips have fixed duration

    // Build the context's database + KD-tree + SIMD cache on first request
    // (all contexts build lazily on first request except LOCOMOTION which is
    // built at load time).
    ensureContextBuilt(ctx);
    if (!contextDatabases_[idx] || contextDatabases_[idx]->GetPoseCount() == 0) return;
    if (!contextSearchTrees_[idx]) return;  // tree must be pre-built (shouldn't happen)
    if (motionContext_ == ctx) return;  // already active - don't re-swap

    // ---- Structural Motion Graph: Play pre-computed transition clip ----
    // Instead of instantly swapping databases (SetDatabaseExplicit), look up
    // a pre-baked transition clip from the motion transition graph. The clip
    // smoothly blends ALL joints via slerp + linear root interpolation with
    // 2D coordinate alignment over ~0.33s. When the clip finishes, the
    // MotionMatcher swaps to the target database seamlessly.
    auto clip = transitionGraph_.GetTransition(
        static_cast<int>(motionContext_), static_cast<int>(ctx));

    if (clip && matcher_) {
        // Play the transition clip through the MotionMatcher
        const MotionDatabase* targetDB = contextDatabases_[idx].get();
        const MotionKDTree* targetTree = contextSearchTrees_[idx].get();
        matcher_->PlayTransitionClip(
            clip,
            targetDB ? *targetDB : *matcher_->GetDatabase(),
            targetTree ? *targetTree : matcher_->GetSearchTree(),
            0);

        transitionClipPlaying_ = true;
        transitionTargetCtx_ = ctx;
        contextSwitchCooldown_ = kContextSwitchCooldown;  // prevent rapid re-toggle
        std::cout << "[AnimatedCharacter] Playing transition clip: "
                  << motionContextName() << " -> "
                  << ([&]{ switch(ctx) {
                      case MotionContext::LOCOMOTION: return "Locomotion";
                      case MotionContext::CROUCH: return "Crouch";
                      case MotionContext::COMBAT: return "Combat";
                      case MotionContext::CAPOEIRA: return "Capoeira";
                      case MotionContext::DANCE: return "Dance";
                      default: return "Unknown";
                  }}())
                  << " (" << clip->duration << "s)\n";
    } else {
        // Fallback: instant swap (e.g. for contexts without a pre-built
        // transition clip, like CAPOEIRA/DANCE which may not be built yet)
        matcher_->SetDatabaseExplicit(*contextDatabases_[idx], *contextSearchTrees_[idx]);
        motionContext_ = ctx;
        std::cout << "[AnimatedCharacter] Instant motion context switch (no transition clip) -> "
                  << motionContextName() << " ("
                  << contextDatabases_[idx]->GetAnimationCount() << " clips, "
                  << contextDatabases_[idx]->GetPoseCount() << " poses, SIMD pre-warmed)\n";
    }
}

inline std::string AnimatedCharacter::motionContextName() const {
    switch (motionContext_) {
        case MotionContext::LOCOMOTION: return "Locomotion";
        case MotionContext::CROUCH: return "Crouch";
        case MotionContext::COMBAT: return "Combat";
        case MotionContext::CAPOEIRA: return "Capoeira";
        case MotionContext::DANCE: return "Dance";
        default: return "Unknown";
    }
}

inline size_t AnimatedCharacter::contextPoseCount(MotionContext ctx) const {
    const int idx = static_cast<int>(ctx);
    if (idx < 0 || idx >= static_cast<int>(MotionContext::COUNT)) return 0;
    return contextDatabases_[idx] ? contextDatabases_[idx]->GetPoseCount() : 0;
}

inline int AnimatedCharacter::contextClipCount(MotionContext ctx) const {
    const int idx = static_cast<int>(ctx);
    if (idx < 0 || idx >= static_cast<int>(MotionContext::COUNT)) return 0;
    return contextDatabases_[idx]
               ? static_cast<int>(contextDatabases_[idx]->GetAnimationCount()) : 0;
}

inline bool AnimatedCharacter::isClipLocked() const {
    return matcher_ ? matcher_->IsClipLocked() : false;
}

// -------------------------------------------------------------------
// Combat combo helpers
// -------------------------------------------------------------------
// Combat clip preference by combo step + movement direction (forward,
// side, or neutral).  The Creature Pack provides three combat clips:
//   - Punch      (quick jab, forward/side)
//   - Swipe      (wide arc, neutral)
//   - JumpAttack (leap forward, forward-only)
// Step 1 = starter, Step 2 = chamber, Step 3 = finisher.

inline void AnimatedCharacter::startCombatAction(const glm::vec2& moveDir, float moveMag) {
    const int ctxIdx = static_cast<int>(MotionContext::COMBAT);
    auto* db = contextDatabases_[ctxIdx].get();
    if (!db || db->GetAnimationCount() == 0) return;

    comboStep_ = 1;
    comboTimer_ = kComboWindow;
    combatMoveDir_ = moveDir;
    selectCombatClip(combatClipIndex_, comboStep_, moveDir);

    if (!combatClip_) return;
    combatActionActive_ = true;
    combatTimer_ = combatClip_->duration;
}

inline void AnimatedCharacter::selectCombatClip(int& outIdx, int step, const glm::vec2& dir) {
    const int ctxIdx = static_cast<int>(MotionContext::COMBAT);
    auto* db = contextDatabases_[ctxIdx].get();
    if (!db) {
        combatClip_.reset();
        return;
    }

    // Directional selection:
    //   forward (|dir.y| > |dir.x|, -Y is forward) → JumpAttack
    //   side (|dir.x| > |dir.y|)                      → Punch
    //   neutral (moveMag < 0.1)                      → Swipe
    const bool forward = std::abs(dir.y) > std::abs(dir.x) && dir.y < -0.1f;
    const bool sideways = std::abs(dir.x) > 0.1f && !forward;
    const bool neutral = std::abs(dir.x) < 0.1f && std::abs(dir.y) < 0.1f;

    std::string preferred;
    if (step == 1) {
        if (forward)      preferred = "JumpAttack";
        else              preferred = "Punch";
    } else if (step == 2) {
        if (sideways)     preferred = "Punch";
        else if (forward) preferred = "JumpAttack";
        else              preferred = "Swipe";
    } else {  // step 3 = finisher
        preferred = "JumpAttack";
    }

    std::shared_ptr<Animation> clip = db->GetAnimation(preferred);
    if (!clip) {
        // Fallback: cycle through all combat clips
        size_t idx = (comboStep_ - 1) % db->GetAnimationCount();
        clip = db->GetAnimation(idx);
    }

    if (clip) {
        combatClip_ = clip;
        outIdx = combatClipIndex_ = (combatClipIndex_ + 1) % 3;
    }
}

inline void AnimatedCharacter::endCombatAction() {
    combatActionActive_ = false;
    combatTimer_ = 0.0f;
    comboStep_ = 0;
    comboTimer_ = 0.0f;
    pendingAttack_ = false;
    combatClip_.reset();

    // Auto-return to Locomotion if we were in Combat.
    if (motionContext_ == MotionContext::COMBAT) {
        setMotionContext(MotionContext::LOCOMOTION);
    }
    state_ = AnimationState::IDLE;
    lastFsmState_ = state_;
}

// -------------------------------------------------------------------
// AAA Root Motion Extraction
// -------------------------------------------------------------------
// Extracts the root bone's displacement between the previous and current
// animation time, transforms it to world space, and applies it to the
// character's position.  This is the "Golden Rule" of AAA combat animation:
// the capsule follows the authored root motion, not procedural velocity.

inline bool AnimatedCharacter::combatClipNeedsRootMotion() const {
    // All Creature Pack combat clips have root bone position animation.
    // Return false only if the clip lacks position keys (then procedural
    // velocity handles movement).
    if (!combatClip_) return false;
    static const std::vector<std::string> rootNames = {
        "hips", "Hips", "mixamorig:hips", "mixamorig:Hips",
        "Root", "root", "pelvis", "Pelvis"
    };
    for (const auto& name : rootNames) {
        auto it = combatClip_->boneAnimations.find(NormalizeBoneName(name));
        if (it != combatClip_->boneAnimations.end()) {
            return !it->second.positionTimes.empty();
        }
    }
    return !combatClip_->boneAnimations.empty() &&
           !combatClip_->boneAnimations.begin()->second.positionTimes.empty();
}

inline glm::vec3 AnimatedCharacter::ComputeRawAssetRootDisplacement(float dt) {
    if (!combatClip_ || !animator_) return glm::vec3(0.0f);

    // 1. Find the root bone animation in the combat clip
    const BoneAnimation* rootBone = nullptr;
    static const std::vector<std::string> rootNames = {
        "hips", "Hips", "mixamorig:hips", "mixamorig:Hips",
        "Root", "root", "pelvis", "Pelvis"
    };
    for (const auto& name : rootNames) {
        auto it = combatClip_->boneAnimations.find(NormalizeBoneName(name));
        if (it != combatClip_->boneAnimations.end()) {
            rootBone = &it->second;
            break;
        }
    }
    if (!rootBone || rootBone->positionTimes.empty()) return glm::vec3(0.0f);

    // 2. Sample root bone position at current and previous time
    float currTime = animator_->GetCurrentTime();
    float prevTime = std::max(0.0f, currTime - dt);

    // Linear interpolation helper for raw bone track
    auto sampleRootPos = [](const BoneAnimation* bone, float t) -> glm::vec3 {
        if (bone->positionTimes.empty()) return glm::vec3(0.0f);
        if (t <= bone->positionTimes.front()) return bone->positionValues.front();
        if (t >= bone->positionTimes.back()) return bone->positionValues.back();
        for (size_t i = 0; i + 1 < bone->positionTimes.size(); ++i) {
            if (t >= bone->positionTimes[i] && t <= bone->positionTimes[i + 1]) {
                float span = bone->positionTimes[i + 1] - bone->positionTimes[i];
                if (span <= 0.0f) return bone->positionValues[i];
                float f = (t - bone->positionTimes[i]) / span;
                return glm::mix(bone->positionValues[i], bone->positionValues[i + 1], f);
            }
        }
        return bone->positionValues.front();
    };

    glm::vec3 posPrev = sampleRootPos(rootBone, prevTime);
    glm::vec3 posCurr = sampleRootPos(rootBone, currTime);
    glm::vec3 localDelta = posCurr - posPrev;

    // 3. Transform local delta → world space using character heading.
    // Local -Z is forward; the FBX root bone moves in the character's local
    // space, so we rotate by the inverse heading to get world-aligned motion.
    glm::mat4 rot = glm::rotate(glm::mat4(1.0f), heading + kPi,
                                glm::vec3(0.0f, 1.0f, 0.0f));
    glm::vec3 worldDelta = glm::vec3(rot * glm::vec4(localDelta, 1.0f))
                            * scale * visualScale;

    // XZ only — Y is handled by gravity/physics
    return glm::vec3(worldDelta.x, 0.0f, worldDelta.z);
}

inline void AnimatedCharacter::ApplyTargetWarpingToRootMotion(
        glm::vec3& outWorldDelta, float dt, const glm::vec3& enemyTargetPos) {
    if (!combatActionActive_ || !combatClip_) return;

    // 1. Current spatial error between capsule and target
    float currentDistance = glm::distance(position, enemyTargetPos);
    const float kIdealStrikeDistance = 1.4f; // metres — preferred attack range

    // Out of range: no warping needed
    if (currentDistance < 0.1f || currentDistance > 5.5f) return;

    // 2. Separate horizontal displacement from raw root motion
    glm::vec2 horizontalDelta(outWorldDelta.x, outWorldDelta.z);
    float rawStepLength = glm::length(horizontalDelta);
    if (rawStepLength < 0.001f) return; // don't warp stationary attacks

    // 3. Compute direction to target
    glm::vec3 targetDir = glm::normalize(enemyTargetPos - position);
    glm::vec2 warpDirection(targetDir.x, targetDir.z);

    // 4. Compute adaptive warp scale
    float remainingTime = combatTimer_;
    if (remainingTime <= dt) return;

    float requiredClosingSpeed = (currentDistance - kIdealStrikeDistance) / remainingTime;
    float baselineSpeed = rawStepLength / dt;

    float warpScalingFactor = 1.0f;
    if (baselineSpeed > 0.001f) {
        warpScalingFactor = requiredClosingSpeed / baselineSpeed;
        warpScalingFactor = glm::clamp(warpScalingFactor, 0.6f, 1.8f);
    }

    // 5. Soft magnetism: blend 45% toward warped direction
    glm::vec2 warpedHorizontal = glm::mix(horizontalDelta,
        warpDirection * rawStepLength * warpScalingFactor, 0.45f);

    outWorldDelta.x = warpedHorizontal.x;
    outWorldDelta.z = warpedHorizontal.y;
}

inline bool AnimatedCharacter::EngineQueryClosestLivingEnemy(
        const glm::vec3& from, const glm::vec2& dir,
        float maxDist, glm::vec3& outTargetPos) {
    // Delegate to WorldManager's NPC tracking system with Intent Matrix
    // Scoring + Line-of-Sight filtering.
    World::WorldManager& wm = World::WorldManager::getInstance();
    float score = 0.0f;
    // Lift dir from 2D (x, y) → 3D forward (x, 0, y) for world-space cone
    glm::vec3 forward3d(dir.x, 0.0f, dir.y);
    if (forward3d.length() < 0.001f) forward3d = glm::vec3(0, 0, -1.0f);  // default forward
    forward3d = glm::normalize(forward3d);
    return wm.getNearestEnemy(from, forward3d, maxDist, outTargetPos, score);
}

inline void AnimatedCharacter::UpdateProceduralFootPlanting(float dt) {
    if (!animator_ || !skeleton_ || dt <= 0.0001f) return;

    // Resolve foot bone indices (cached)
    static const int leftFootIdx =
        skeleton_->GetBoneIndex("LeftFoot") >= 0
            ? skeleton_->GetBoneIndex("LeftFoot")
            : skeleton_->GetBoneIndex("leftfoot");
    static const int rightFootIdx =
        skeleton_->GetBoneIndex("RightFoot") >= 0
            ? skeleton_->GetBoneIndex("RightFoot")
            : skeleton_->GetBoneIndex("rightfoot");

    if (leftFootIdx < 0 || rightFootIdx < 0) return;

    // Track last local foot positions across frames
    static glm::vec3 lastLocalPosLeft{0.0f};
    static glm::vec3 lastLocalPosRight{0.0f};

    const glm::mat4 ident(1.0f);

    // 1. Local foot positions relative to character capsule
    glm::vec3 currentLocalPosLeft  = animator_->GetBoneWorldPosition(leftFootIdx, ident) - position;
    glm::vec3 currentLocalPosRight = animator_->GetBoneWorldPosition(rightFootIdx, ident) - position;

    // 2. Derive foot velocity relative to capsule
    glm::vec3 velLeft  = (currentLocalPosLeft  - lastLocalPosLeft)  / dt;
    glm::vec3 velRight = (currentLocalPosRight - lastLocalPosRight) / dt;

    lastLocalPosLeft  = currentLocalPosLeft;
    lastLocalPosRight = currentLocalPosRight;

    // 3. Threshold gates for locking
    const float kVelocityThreshold = 0.18f;  // m/s — foot must be nearly stationary
    const float kHeightThreshold   = 0.08f;  // metres — foot must be near ground

    auto EvaluateStanceLock = [&](glm::vec3& vel, glm::vec3& localPos,
                                   auto& footIKData) {
        float horizontalSpeed = glm::length(glm::vec2(vel.x, vel.z));
        float absoluteHeight  = localPos.y;

        if (grounded && horizontalSpeed < kVelocityThreshold &&
            absoluteHeight < kHeightThreshold) {
            // Engage lock
            if (!footIKData.isLocked) {
                footIKData.isLocked = true;
                footIKData.lockedPosition =
                    animator_->GetBoneWorldPosition(
                        &footIKData == &animator_->leftFootIK ? leftFootIdx : rightFootIdx,
                        modelMatrix());
            }
            // Smooth weight ramp-up
            footIKData.lockWeight = std::min(1.0f,
                footIKData.lockWeight + (5.0f * dt));
        } else {
            // Release lock smoothly
            footIKData.lockWeight = std::max(0.0f,
                footIKData.lockWeight - (8.0f * dt));
            if (footIKData.lockWeight <= 0.0f) {
                footIKData.isLocked = false;
            }
        }
    };

    EvaluateStanceLock(velLeft,  currentLocalPosLeft,  animator_->leftFootIK);
    EvaluateStanceLock(velRight, currentLocalPosRight, animator_->rightFootIK);
}

inline void AnimatedCharacter::TrimClipTo(Animation& clip, float maxSeconds) {
    for (auto& kv : clip.boneAnimations) {
        BoneAnimation& ba = kv.second;
        auto trim = [&](std::vector<double>& times, auto& values) {
            size_t cut = times.size();
            for (size_t i = 0; i < times.size(); ++i) {
                if (times[i] > maxSeconds) {
                    cut = i;
                    break;
                }
            }
            times.resize(cut);
            values.resize(cut);
        };
        trim(ba.positionTimes, ba.positionValues);
        trim(ba.rotationTimes, ba.rotationValues);
        trim(ba.scaleTimes, ba.scaleValues);
    }
}
