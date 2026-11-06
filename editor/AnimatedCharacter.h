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
#include "../geospatial/GeoAPI.h"   // geo::AssetKind twin classification (Todo 7)

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
        LOCOMOTION = 0,  // Idle/Walk/Jog/Run/Jump/Fall/turns/catwalk
        CROUCH,          // Crouching + CrouchWalk (auto with crouch input)
        COMBAT,          // Boxing strikes/blocks/defeated
        CAPOEIRA,        // the ~39-clip capoeira set (built lazily on demand)
        DANCE,           // rumba/hip-hop/taunts/social clips
        COUNT
    };

    // Parse a motion-context name (as authored in JSON / CharacterInput)
    // into the enum. Mirrors the worker if-chain: "Crouch"/"CROUCH" -> CROUCH,
    // "Combat"/"COMBAT" -> COMBAT, "Capoeira"/"CAPOEIRA" -> CAPOEIRA,
    // "Dance"/"DANCE" -> DANCE, anything else -> LOCOMOTION. Pure refactor.
    static MotionContext parseMotionContext(const std::string& name) {
        if (name == "Crouch" || name == "CROUCH")       return MotionContext::CROUCH;
        if (name == "Combat" || name == "COMBAT")       return MotionContext::COMBAT;
        if (name == "Capoeira" || name == "CAPOEIRA")   return MotionContext::CAPOEIRA;
        if (name == "Dance" || name == "DANCE")         return MotionContext::DANCE;
        return MotionContext::LOCOMOTION;
    }

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
    // Geospatial twin classification - drives mesh selection in the editor
    // (Todo 7). Defaults to Personnel (the bot/human stand-in used while the
    // dedicated excavator/utility_car/miner/sensor_node assets are authored).
    geo::AssetKind m_assetKind = geo::AssetKind::Personnel;
    glm::vec3 terrainNormal{0.0f, 1.0f, 0.0f};

    // -----------------------------------------------------------------------
    // Loading
    // -----------------------------------------------------------------------
    bool load(const std::string& path);
    int loadLocomotion(const std::string& dir);
    // Dynamic Hot-Swap (forhotswap.txt, stage 1 - synchronous): flush all
    // motion systems in strict dependency order and reload from a new model +
    // clip folder IN PLACE (the AnimatedCharacter object is preserved so
    // external render-model refs stay valid). Returns false (leaving the
    // character non-ready) on failure. Tear-down order per the doc:
    // Animator -> MotionMatcher -> Contextual Databases -> Clip Assets.
    bool HotSwapCharacterPack(const std::string& fbxModelPath,
                              const std::string& clipsDirectory);

    // Stage 2 (forhotswap.txt, async): the live motion systems can be extracted
    // (moved out) and later re-applied (moved back in). This is the
    // double-buffer primitive behind the async hot-swap: a worker thread builds
    // a staging AnimatedCharacter and calls ExtractRuntimeSystems(); the main
    // thread then move-applies the result onto THIS live character. Extract
    // moves OUT the Animator/Skeleton/MotionMatcher/Databases/Clips + the
    // load-derived scalars (rawSizeY, targetHeight, idleClipDuration,
    // clipCount, clipsDir, mmReady, loaded) but preserves the SOURCE pose.
    // ApplyRuntimeSystems() is the inverse: it flushes the existing systems,
    // adopts the staging set, recomputes render `scale` from rawSizeY, and
    // preserves the LIVE pose so the bot never teleports on a pack swap.
    struct RuntimeSystems {
        std::unique_ptr<Animator> animator;
        std::unique_ptr<Skeleton> skeleton;
        std::unique_ptr<MotionMatcher> matcher;
        std::array<std::unique_ptr<MotionDatabase>,
                   (size_t)MotionContext::COUNT> databases;
        std::vector<std::unique_ptr<Animation>> clips;
        std::vector<float> clipSpeeds;
        float  rawSizeY         = 0.0f;
        float  targetHeight     = 1.8f;
        float  idleClipDuration  = 0.0f;
        int    clipCount        = 0;
        std::string clipsDir;
        bool   mmReady          = false;
        bool   loaded           = false;
    };
    RuntimeSystems ExtractRuntimeSystems();
    void ApplyRuntimeSystems(RuntimeSystems&& rs);

    static Animation* LoadClipFromFile(const std::string& path);

    // -----------------------------------------------------------------------
    // Queries
    // -----------------------------------------------------------------------
    size_t boneCount() const { return skeleton_ ? skeleton_->bones.size() : 0; }
    float rawSizeY() const { return rawSizeY_; }
    bool ready() const { return loaded_; }
    // Public handle to the authored model path (NPC details panel hot-reload).
    const std::string& modelPath() const { return activeModelPath_; }
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

    // Public handle to the motion matcher. The forAnime.txt §3 trajectory debug
    // overlay reaches in from ui.cpp to read MotionMatchingDebug without coupling
    // the UI to AnimatedCharacter's private member layout. Null when no matcher
    // is attached (e.g. the character is not yet motion-matching ready).
    MotionMatcher* getMatcher() const { return matcher_.get(); }

    // Geospatial twin classification accessor (Todo 7: mesh swap). Lets the
    // geo terminal / panel reclassify the play character so renderPlayCharacter
    // swaps the visible mesh accordingly.
    geo::AssetKind geospatialAssetKind() const { return m_assetKind; }
    void setGeospatialAssetKind(geo::AssetKind k) { m_assetKind = k; }

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

    // --- Performance metrics for the PlayMode HUD visual tiles ---
    struct MMDiagnosticTile {
        int posesSearched = 0;          // Poses searched this frame
        float searchTimeMs = 0.0f;      // KD-tree search duration (ms)
        int kdTreeNodes = 0;            // KD-tree node count
        int kdTreeDepth = 0;            // KD-tree max depth
        int kdTreeLeaves = 0;           // KD-tree leaf count
        int databaseClips = 0;          // Total clips in active database
        int databasePoses = 0;          // Total poses in active database
        float matchScore = 0.0f;        // Current best match score
        const char* simdStatus = "AVX2";// SIMD width label (compile-time)
    };
    MMDiagnosticTile mmDiagnosticTile() const;

    // -----------------------------------------------------------------------
    // Motion contexts (contextual database switching)
    // -----------------------------------------------------------------------
    // Switch the pose-search database to a context. Crossfades between the old
    // and new databases (blendDuration = 0 switches instantly); requests to
    // the already-active context are no-ops (safe to send every frame). The
    // capoeira database is built lazily on first request.
    void setMotionContext(MotionContext ctx, float blendDuration = 0.25f);
    MotionContext motionContext() const { return motionContext_; }
    std::string motionContextName() const;
    // Pose/clip counts of a context's database (0 = not built / empty).
    size_t contextPoseCount(MotionContext ctx) const;
    int contextClipCount(MotionContext ctx) const;
    // Name of the clip at index within a context's database (for UI listing).
    std::string contextClipName(MotionContext ctx, size_t index) const;

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

    // Rewind — reset the current animation clip time to the beginning.
    // Used by the toolbar "Rewind" button to replay the clip from start.
    void rewind() {
        if (animator_) animator_->Rewind();
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
    void addClipToContext(MotionContext ctx, const std::string& name,
                          const std::string& file, const std::string& dir);

    // Flush all motion systems in dependency order (see HotSwapCharacterPack).
    void FlushMotionSystems();

    // Destruction order matters: animator_/skeleton_ die first, then matcher_
    // (whose database holds non-owning aliases into clips_), then clips_ last.
    std::vector<std::unique_ptr<Animation>> clips_;
    std::vector<float> clipSpeeds_;

    // Contextual databases, one per MotionContext. The matcher holds RAW
    // pointers into these (non-owning SetDatabase), so they must be declared
    // BEFORE matcher_ (destroyed AFTER it) and AFTER clips_ (the context DBs
    // alias slot clips, so clips_ must outlive them).
    std::array<std::unique_ptr<MotionDatabase>, (size_t)MotionContext::COUNT> contextDatabases_;
    std::string clipsDir_;                        // for lazy capoeira building
    MotionContext motionContext_ = MotionContext::LOCOMOTION;
    bool wasCrouching_ = false;                   // crouch-context edge detect

    // Hot-swap tracking (forhotswap.txt): active asset paths + dirty flag.
    // isSystemDirty_ is reset to false once a sync reload completes; in the
    // async stage it stays true across frames until the background KD-tree
    // finishes.
    std::string activeModelPath_;
    std::string activeClipsDir_;
    bool isSystemDirty_ = false;

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

    // Movement tuning
    float jumpSpeed_ = 5.5f;
    float gravity_ = 9.8f;
};

// ============================================================================
// Implementation
// ============================================================================

inline bool AnimatedCharacter::load(const std::string& path) {
    loaded_ = false;
    std::atomic<float> progress{0.0f};
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
        path, aiProcess_Triangulate | aiProcess_GenUVCoords);
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
    // Also reset the context databases FIRST — they hold non-owning
    // shared_ptr aliases into the same clip memory.  Clearing clips_ before
    // resetting the DBs would leave those aliases dangling (use-after-free)
    // if any code path touches them during the reload window.
    matcher_.reset();
    for (auto& db : contextDatabases_) {
        db.reset(); // Clear all non-owning database aliases first!
    }
    clips_.clear();
    clips_.resize(CLIP_SLOTS);
    clipSpeeds_.assign(CLIP_SLOTS, 0.0f);

    struct SlotFile { ClipSlot slot; const char* name; const char* file; };
    static const SlotFile kSlots[] = {
        {CLIP_IDLE, "Idle", "Idle.fbx"},
        {CLIP_WALK, "Walk", "Walking.fbx"},
        {CLIP_RUN, "Run", "Run.fbx"},
        {CLIP_JUMP, "Jump", "Jump.fbx"},
        {CLIP_FALL, "Fall", "Fall.fbx"},
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

        // Activate the ENRICHED locomotion context (all 10 clips, including
        // Jog/turns/catwalk/run-look-back) as the default search domain so
        // MM handles the extra locomotion clips out of the box.
        if (contextDatabases_[(int)MotionContext::LOCOMOTION] &&
            contextDatabases_[(int)MotionContext::LOCOMOTION]->GetPoseCount() > 0) {
            matcher_->SetDatabase(*contextDatabases_[(int)MotionContext::LOCOMOTION], 0.0f);
        }
    }
    return clipCount_;
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
    if (input.motionContext >= 0 && input.motionContext < (int)MotionContext::COUNT) {
        setMotionContext(static_cast<MotionContext>(input.motionContext));
    } else if (motionContext_ == MotionContext::LOCOMOTION && crouching_ && !wasCrouching_) {
        setMotionContext(MotionContext::CROUCH);
    } else if (motionContext_ == MotionContext::CROUCH && !crouching_ && wasCrouching_) {
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

    // ---- Horizontal speed (walk / run / crouch bands) ----
    float speedCap = 2.0f;
    if (sprinting_) speedCap = 6.0f;
    else if (crouching_) speedCap = 1.45f;
    const float targetSpeed = speedCap * moveMag;

    const float horizSpeed = glm::length(glm::vec2(velocity.x, velocity.z));
    const float accel = crouching_ ? 10.0f : 16.0f;
    const float newHoriz = horizSpeed + (targetSpeed - horizSpeed) * std::min(1.0f, accel * dt);
    velocity.x = moveDir.x * newHoriz;
    velocity.z = moveDir.y * newHoriz;

    // ---- Jump / gravity ----
    const bool mmCanDriveAir = isMotionMatchingActive() && matcher_ && matcher_->HasAirbornePoses();
    if (grounded) {
        velocity.y = 0.0f;
        if (input.jump) {
            velocity.y = jumpSpeed_;
            grounded = false;
            // Legacy one-shot jump timer: only used when the pose search can't
            // represent the arc (no Jump/Fall poses in the database).
            if (!mmCanDriveAir) legacyJumpTimer_ = 0.4f;
        }
    } else {
        velocity.y -= gravity_ * dt;
        if (legacyJumpTimer_ > 0.0f) legacyJumpTimer_ -= dt;
    }

    // ---- Integrate ----
    position += velocity * dt;

    // ---- Terrain snap ----
    const float floorY = terrain ? terrain(position.x, position.z) : position.y;
    if (grounded) {
        position.y = floorY;
    } else if (position.y <= floorY) {
        position.y = floorY;
        grounded = true;
        velocity.y = 0.0f;
    }

    // ---- Locomotion FSM ----
    const float speed2d = glm::length(glm::vec2(velocity.x, velocity.z));
    if (!grounded) {
        state_ = (velocity.y > 0.5f) ? AnimationState::JUMP : AnimationState::FALL;
    } else if (crouching_) {
        state_ = (speed2d > 0.2f) ? AnimationState::CROUCH_WALK : AnimationState::CROUCH;
    } else if (sprinting_) {
        state_ = AnimationState::RUN;
    } else if (speed2d > 0.2f) {
        state_ = AnimationState::WALK;
    } else {
        state_ = AnimationState::IDLE;
    }

    // ---- Preview bookkeeping ----
    if (previewActive_) {
        previewTimer_ -= dt;
        if (moveMag > 0.05f || previewTimer_ <= 0.0f) previewActive_ = false;
    }

    // ---- Drive the animation ----
    if (previewActive_ && previewClipIndex_ >= 0 &&
        previewClipIndex_ < static_cast<int>(clips_.size()) && clips_[previewClipIndex_]) {
        activeClipIndex_ = previewClipIndex_;
        if (animator_->GetCurrentAnimation() != clips_[previewClipIndex_].get()) {
            animator_->Play(clips_[previewClipIndex_].get());
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
    // Set the model matrix BEFORE Update() so the clean reference pass in
    // Update() can transform bone positions to world space for the pelvis
    // reach comparison inside CalculatePelvisAdjustment.
    animator_->SetModelMatrix(modelMatrix());
    animator_->Update(dt);

    // ---- Foot IK (AFTER skeleton evaluation) -------------------------------
    // Running foot IK + pelvis adjustment here — after Animator::Update
    // computed currBoneWorldPos — eliminates the 1-frame lag that caused
    // jittery legs and stretched knees during pose switches. The IK targets
    // are computed against this frame's bone positions; the offsets are
    // applied on the NEXT frame's EvaluateNode (standard 1-frame IK latency,
    // masked by the smoothed ankle / pelvis interpolation).
    if ((motionMatchingEnabled && mmReady_) && matcher_) {
        // modelMatrix() matches what the renderer uses (180° flip included).
        matcher_->SetCharacterModelMatrix(modelMatrix());
        matcher_->ApplyFootIK(dt);
        // Re-evaluate the skeleton in the SAME frame so the visual pose
        // matches the IK solve (eliminates 1-frame lag → knee jitter during
        // motion-matching pose switches).
        animator_->RevalidateIK();
    } else {
        // FIX (forAnime.txt Fix 4): For the FSM / preview paths, run the
        // standard Animator foot IK solver + immediate skeleton revalidation.
        // Without RevalidateIK() here, the fresh IK offsets sit in the ik arrays
        // for one full frame before reaching the hardware bone matrices,
        // producing a visible 1-frame lag (Temporal Desync Loop) that causes
        // foot jitter during slow walks and idles.
        const bool characterIsMoving = (horizSpeed > 0.05f) || !grounded;
        animator_->UpdateFootIK(dt, modelMatrix(), /*isMoving=*/characterIsMoving);
        animator_->RevalidateIK();
    }
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
    const bool hijacked = animator_->GetActiveAnimationLayerCount() <= 1 &&
                          animator_->GetCurrentAnimation() != clip;
    if (fsmStateChanged || hijacked) {
        lastFsmState_ = state_;
        animator_->BlendToAt(clip, 0.0f, kFsmBlendDuration);
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
    matcher_->Update(dt, cs);

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
    const bool locomotionLike =
        motionContext_ == MotionContext::LOCOMOTION ||
        motionContext_ == MotionContext::CROUCH;
    const float speed2d = glm::length(glm::vec2(velocity.x, velocity.z));
    if (grounded && locomotionLike) {
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
                // FIX (forAnime.txt Fix 3): Only force-blend when there is
                // active movement. At true idle the motion matcher context
                // should own the pose; forcing a BlendToAt here repeatedly
                // appends layers, resets root motion, and disrupts the IK
                // targets every frame, causing feet to jitter in place.
                if (speed2d > 0.05f) {
                    animator_->BlendToAt(want, animator_->GetCurrentTime(), 0.2f);
                }
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
        {"Idle.fbx", CLIP_IDLE}, {"Walking.fbx", CLIP_WALK},
        {"Run.fbx", CLIP_RUN}, {"Jump.fbx", CLIP_JUMP}, {"Fall.fbx", CLIP_FALL},
        {"Crouching.fbx", CLIP_CROUCH}, {"chrouchWalk.fbx", CLIP_CROUCH_WALK},
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
    // Only the DEFAULT search domain is built eagerly - the other contexts
    // (crouch/combat/capoeira/dance) load lazily on first request, per the
    // suggestions' "only load what the active context needs".
    ensureContextBuilt(MotionContext::LOCOMOTION);
}

inline void AnimatedCharacter::ensureContextBuilt(MotionContext ctx) {
    const int idx = static_cast<int>(ctx);
    if (idx < 0 || idx >= static_cast<int>(MotionContext::COUNT)) return;
    if (contextDatabases_[idx]) return;  // already built

    struct ContextClip { const char* name; const char* file; };
    static const ContextClip kLocomotion[] = {
        {"Idle", "Idle.fbx"}, {"Walk", "Walking.fbx"}, {"Jog", "Jogging.fbx"},
        {"Run", "Run.fbx"}, {"Jump", "Jump.fbx"}, {"Fall", "Fall.fbx"},
        {"Catwalk", "Catwalk Idle To Walk Forward.fbx"},
        {"LeftTurn", "Left Turn.fbx"}, {"RightTurn", "Right Turn.fbx"},
        {"RunLookBack", "Run Look Back.fbx"},
    };
    static const ContextClip kCrouch[] = {
        {"Crouch", "Crouching.fbx"}, {"CrouchWalk", "chrouchWalk.fbx"},
    };
    static const ContextClip kCombat[] = {
        {"Boxing", "Boxing.fbx"}, {"BodyBlock", "Body Block.fbx"},
        {"BoxTurn", "Box Turn.fbx"}, {"Defeated", "Defeated.fbx"},
    };
    static const ContextClip kDance[] = {
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

    // Build the context's database on first request (crouch/combat/capoeira/
    // dance are lazy - only the default locomotion context is eager).
    ensureContextBuilt(ctx);
    if (!contextDatabases_[idx] || contextDatabases_[idx]->GetPoseCount() == 0) return;
    if (motionContext_ == ctx) return;  // already active - don't restart the blend

    matcher_->SetDatabase(*contextDatabases_[idx], blendDuration);
    motionContext_ = ctx;
    std::cout << "[AnimatedCharacter] Motion context -> " << motionContextName()
              << " (" << contextDatabases_[idx]->GetAnimationCount() << " clips)\n";
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

inline std::string AnimatedCharacter::contextClipName(MotionContext ctx, size_t index) const {
    const int idx = static_cast<int>(ctx);
    if (idx < 0 || idx >= static_cast<int>(MotionContext::COUNT)) return std::string();
    if (!contextDatabases_[idx]) return std::string();
    return contextDatabases_[idx]->GetAnimationName(index);
}

inline AnimatedCharacter::MMDiagnosticTile AnimatedCharacter::mmDiagnosticTile() const {
    MMDiagnosticTile tile;
    if (!matcher_ || !mmReady_) return tile;

    const MotionMatchingDebug& dbg = matcher_->GetDebugInfo();
    tile.posesSearched = dbg.posesSearched;
    tile.searchTimeMs = dbg.searchTimeMs;
    tile.matchScore = dbg.currentResult.score;

    // KD-Tree stats from the matcher's search tree
    if (matcher_->GetDatabase()) {
        tile.databaseClips = (int)matcher_->GetDatabase()->GetAnimationCount();
        tile.databasePoses = (int)matcher_->GetDatabase()->GetPoseCount();
    }
    MotionKDTree::TreeStats treeStats = matcher_->GetKDTreeStats();
    tile.kdTreeNodes = (int)treeStats.totalNodes;
    tile.kdTreeDepth = treeStats.maxDepth;
    tile.kdTreeLeaves = (int)treeStats.leafNodes;

    // SIMD status (compile-time)
#ifdef __AVX2__
    tile.simdStatus = "AVX2";
#elif defined(__SSE4_1__)
    tile.simdStatus = "SSE4.1";
#else
    tile.simdStatus = "Scalar";
#endif
    return tile;
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

// ============================================================================
// Hot-Swap: Synchronous Character Pack Reloading (forhotswap.txt, stage 1).
// Rebuilds the Animator/MotionMatcher/Databases/Clips in place so an external
// observer (e.g. PlayModeController -> EditorApplication render model) keeps
// valid refs across a pack change. Runs on the main thread, before render.
// ============================================================================
inline void AnimatedCharacter::FlushMotionSystems() {
    // Tear down in strict dependency order so no dependent system holds a
    // dangling alias into memory we've already released:
    //   Animator -> MotionMatcher -> Contextual Databases -> Clip Assets.
    if (animator_) {
        animator_.reset();
    }
    if (matcher_) {
        matcher_->SetTerrainFn(nullptr);  // detach live fn before reset
        matcher_.reset();
    }
    for (auto& db : contextDatabases_) {
        db.reset();
    }
    clips_.clear();
    clipSpeeds_.clear();
    mmReady_ = false;
    clipCount_ = 0;
    idleClipDuration_ = 0.0f;
    // A fully-flushed character has no systems/clips -> not loaded/ready.
    loaded_ = false;
}

inline bool AnimatedCharacter::HotSwapCharacterPack(const std::string& fbxModelPath,
                                                    const std::string& clipsDirectory) {
    if (fbxModelPath.empty() || clipsDirectory.empty()) {
        isSystemDirty_ = true;
        return false;
    }
    activeModelPath_ = fbxModelPath;
    activeClipsDir_  = clipsDirectory;
    isSystemDirty_   = true;

    // 1) Flush live motion systems (Animator / Matcher / DBs / Clips).
    FlushMotionSystems();

    // 2) Reload structure + clips + motion DBs. This reuses the verified
    //    load()/loadLocomotion() path, so the rebuilt character is
    //    byte-identical to a freshly-loaded one (skeleton/animator/matcher +
    //    the LOCOMOTION KD-tree all rebuilt and ready).
    if (!load(fbxModelPath)) {
        isSystemDirty_ = true;
        return false;
    }
    if (loadLocomotion(clipsDirectory) < 1) {
        isSystemDirty_ = true;
        return false;
    }

    // 3) Land cleanly: reset the FSM so driveFsm acquires the freshly-built
    //    clip slots on the next update (no stale state_ referencing freed clips).
    state_ = AnimationState::IDLE;
    lastFsmState_ = AnimationState::NONE;
    activeClipIndex_ = CLIP_IDLE;
    isSystemDirty_ = false;
    return true;
}

// ============================================================================
// Hot-Swap: Asynchronous Double-Buffered System Swap (forhotswap.txt, stage 2).
// The worker thread builds a staging AnimatedCharacter (load+loadLocomotion)
// and calls ExtractRuntimeSystems(); the main thread then move-applies the
// result onto the LIVE character. Extract preserves the source pose; Apply
// preserves the LIVE pose so a pack change never teleports the bot.
// ============================================================================
inline AnimatedCharacter::RuntimeSystems AnimatedCharacter::ExtractRuntimeSystems() {
    RuntimeSystems rs;
    rs.animator   = std::move(animator_);
    rs.skeleton   = std::move(skeleton_);
    rs.matcher    = std::move(matcher_);
    for (size_t i = 0; i < (size_t)MotionContext::COUNT; ++i) {
        rs.databases[i] = std::move(contextDatabases_[i]);
    }
    rs.clips      = std::move(clips_);
    rs.clipSpeeds = std::move(clipSpeeds_);

    // Copy the load-derived scalars BEFORE Flush zeroes the source.
    rs.rawSizeY         = rawSizeY_;
    rs.targetHeight     = targetHeight_;
    rs.idleClipDuration = idleClipDuration_;
    rs.clipCount        = clipCount_;
    rs.clipsDir         = std::move(clipsDir_);
    rs.mmReady          = mmReady_;
    rs.loaded           = loaded_;

    // Leave the source flushed/empty (mirrors FlushMotionSystems scalars) so
    // the staging character is non-ready while its systems live in transit.
    FlushMotionSystems();
    return rs;
}

inline void AnimatedCharacter::ApplyRuntimeSystems(RuntimeSystems&& rs) {
    // Preserve the LIVE world pose across the swap - the staging pack may be a
    // completely different model, but the bot must not teleport mid-play.
    const glm::vec3 posePos  = position;
    const float      poseHdg = heading;
    const glm::vec3  poseVel = velocity;
    const glm::vec3  poseNrm = terrainNormal;

    // Release the live motion systems (Animator -> Matcher -> DBs -> Clips).
    FlushMotionSystems();

    // Adopt the staging systems + load-derived scalars (mirrors load()+
    // loadLocomotion() on a freshly-built pack).
    animator_   = std::move(rs.animator);
    skeleton_   = std::move(rs.skeleton);
    matcher_    = std::move(rs.matcher);
    for (size_t i = 0; i < (size_t)MotionContext::COUNT; ++i) {
        contextDatabases_[i] = std::move(rs.databases[i]);
    }
    clips_      = std::move(rs.clips);
    clipSpeeds_ = std::move(rs.clipSpeeds);

    rawSizeY_         = rs.rawSizeY;
    targetHeight_     = rs.targetHeight;
    idleClipDuration_ = rs.idleClipDuration;
    clipCount_        = rs.clipCount;
    clipsDir_         = std::move(rs.clipsDir);
    mmReady_          = rs.mmReady;
    loaded_           = rs.loaded;

    // Re-derive render scale exactly like load() does.
    scale = (rawSizeY_ > 0.001f) ? (targetHeight_ / rawSizeY_) : 1.0f;

    // Restore the pose we saved (Flush never touches these, but be defensive).
    position      = posePos;
    heading       = poseHdg;
    velocity      = poseVel;
    terrainNormal = poseNrm;

    // Re-arm the FSM so driveFsm acquires the freshly-applied clip slots and
    // no stale clip pointers (from the old pack) linger in the FSM state.
    state_           = AnimationState::IDLE;
    lastFsmState_    = AnimationState::NONE;
    activeClipIndex_ = CLIP_IDLE;
    crouching_       = false;
    sprinting_       = false;
    wasCrouching_    = false;
    lastMoveDir_     = glm::vec2(0.0f);
    legacyJumpTimer_ = 0.0f;
    previewActive_   = false;
    previewClipIndex_ = -1;
    previewTimer_     = 0.0f;
    isSystemDirty_    = false;
}
