#pragma once
#include "MotionMatchingTypes.h"
#include "MotionDatabase.h"
#include "TrajectoryPredictor.h"
#include "FootPlantingSystem.h"
#include "MotionKDTree.h"
#include "../animationSystem/Animator.h"
#include "../animationSystem/AnimationStateMachine.h"
#include "../boneSystem/Skeleton.h"

// ============================================================================
// CHARACTER STATE (for motion matching input - MUST BE BEFORE MotionMatcher)
// ============================================================================

struct CharacterState {
    // WORLD-space character state (in the caller's chosen units - the pose
    // database's feature space; the editor character feeds source units).
    //
    // velocity is the world-space XZ velocity, plus the vertical term in .y.
    // The matcher internally rotates it into the CLIP/root frame with
    // R(-rotation) and negates it, so a character moving along its LOCAL
    // FORWARD (-Z, the engine's model convention) produces a clip-frame
    // velocity along the clips' +Z root motion. Callers feeding velocities
    // that are already in the clip frame would be double-transformed - feed
    // world-space velocity and let the matcher convert.
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    // Raw world-space velocity in metres/second (NOT scaled by 1/scale).
    // The matcher uses this for speed-dependent smoothing filters whose
    // thresholds are written in world m/s. Without it, velocity is in model
    // units (e.g. 200 for 2 m/s at scale=0.01) and every threshold is
    // bypassed, disabling all low-speed damping.
    glm::vec3 worldVelocity{0.0f};
    float rotation{0.0f};  // Yaw in radians
    glm::vec2 moveDirection{0.0f, 1.0f};
    bool grounded{true};
    bool crouching{false};
    bool jumping{false};
};

// ============================================================================
// MOTION MATCHER - MAIN CLASS
// ============================================================================
// 
// This is the CORE system that replaces your FSM.
// Every frame it:
// 1. Queries current character state
// 2. Predicts future trajectory
// 3. Searches database for best matching pose
// 4. Blends to that pose
// 5. Applies foot IK to prevent sliding
// 
// Usage:
//   MotionMatcher matcher;
//   matcher.Initialize(skeleton);
//   matcher.LoadAnimation("Walk", walkAnim);
//   
//   // Each frame:
//   matcher.Update(dt, input, characterState);
// ============================================================================

class MotionMatcher {
public:
    MotionMatcher();
    ~MotionMatcher();
    
    // =========================================================================
    // INITIALIZATION
    // =========================================================================
    
    /**
     * Initialize motion matcher
     * 
     * @param skeleton Character skeleton
     * @param animator Animator to control
     */
    void Initialize(const Skeleton* skeleton, Animator* animator);
    
    /**
     * Load animation into database
     *
     * @param name Animation name (e.g., "Walk", "Run")
     * @param anim Animation to load (database takes ownership)
     */
    void LoadAnimation(const std::string& name, std::shared_ptr<Animation> anim);
    
    /**
     * Build KD-Tree search index
     * Call after all animations are loaded
     */
    void BuildSearchIndex();
    
    /**
     * Set configuration
     */
    void SetConfig(const MotionMatchingConfig& config) { this->config = config; }
    const MotionMatchingConfig& GetConfig() const { return config; }

    /**
     * Live-tune the KD-tree pose-search feature weights.
     *
     * The KD-tree is built on feature VALUES, so weights only reshape the
     * distance metric - changes apply immediately with no index rebuild.
     * trajectoryScale multiplies the near->far trajectory falloff
     * {4, 2.5, 1.5, 1.0} uniformly.
     */
    /**
     * Tune the clip-switch persistence bands used by SelectPoseWithPersistence
     * (the pose search's clip hysteresis) on BOTH search paths (KD-tree and
     * brute-force fallback):
     *  - speedBandFactor: the speed band is clipNominalSpeed * this (with a
     *    1 m/s floor so a stationary Idle never trips it). Lower = switch
     *    sooner on gait changes (run<->walk, walk<->stop).
     *  - directionBandRadians: angular band on the movement direction. When
     *    the query heads more than this far from the current clip's nominal
     *    move direction (reversal / backpedal / moving away), the margin is
     *    dropped and the best other-clip candidate is played at once. Lower =
     *    the character turns around sooner.
     * A negative factor/band disables that check entirely.
     */
    void SetClipSwitchBands(float speedBandFactor, float directionBandRadians) {
        config.speedBandFactor = speedBandFactor;
        config.directionBandRadians = directionBandRadians;
    }
    float GetSpeedBandFactor() const { return config.speedBandFactor; }
    float GetDirectionBandRadians() const { return config.directionBandRadians; }

    void SetSearchWeights(float speed, float velX, float velZ, float direction,
                          float footPlant, float trajectoryScale,
                          float verticalVelocity = 2.0f) {
        searchTree.SetSpeedWeight(speed);
        searchTree.SetVelocityWeights(velX, velZ);
        searchTree.SetDirectionWeight(direction);
        searchTree.SetFootPlantWeight(footPlant);
        searchTree.SetVerticalVelocityWeight(verticalVelocity);
        float w[kTrajectorySteps];
        for (int i = 0; i < kTrajectorySteps; ++i) {
            // Scale the shared near->far default falloff uniformly.
            w[i] = MotionKDTree::kDefaultTrajectoryWeights[i] * trajectoryScale;
        }
        searchTree.SetTrajectoryWeights(w);
    }

    /**
     * Set current motion database
     * Used by HybridAnimGraph for database switching
     * Note: This does NOT take ownership, just sets a reference
     * 
     * @param database Motion database to use for searching
     */
    void SetCurrentDatabase(const MotionDatabase& database);

    /**
     * Check if initialized
     */
    bool IsInitialized() const { return initialized; }
    
    // =========================================================================
    // MAIN UPDATE
    // =========================================================================
    
    /**
     * Update motion matching
     * 
     * Call every frame to update character pose.
     * 
     * @param dt Delta time
     * @param input Character input (move direction, etc.)
     * @param characterState Current character state
     */
    void Update(float dt,
               const CharacterInput& input,
               const CharacterState& characterState);
    
    /**
     * Update with explicit parameters
     * 
     * Alternative to CharacterState version.
     */
    void Update(float dt,
               const glm::vec3& position,
               const glm::vec3& velocity,
               const glm::vec2& moveDirection,
               float rotation,
               bool grounded,
               bool crouching);
    
    /**
     * Update with CharacterState only
     */
    void Update(float dt, const CharacterState& characterState);
    
    // =========================================================================
    // STATE QUERIES
    // =========================================================================
    
    /**
     * Get current animation time
     */
    float GetCurrentAnimationTime() const { return currentAnimTime; }
    
    /**
     * Get current animation
     */
    std::shared_ptr<Animation> GetCurrentAnimation() const;
    
    /**
     * Get current pose index in database
     */
    int GetCurrentPoseIndex() const { return currentPoseIndex; }

    /**
     * Enable/disable "clip lock" mode.
     *
     * When the character is at rest (idle), the motion matcher would normally
     * search for the best-matching pose on every frame. On a looping idle clip
     * this causes the animation time to jump between nearby frames ("cut mid
     * clip"), because the pose search selects different frames of the same
     * cycle every tick. This looks like a subtle but visible discontinuity in
     * an otherwise smooth idle.
     *
     * Clip lock mode solves this: when enabled, the matcher skips the KD-tree
     * search entirely and simply advances the current clip's play time at real
     * speed, letting the idle cycle play through naturally. The motion matcher
     * resumes normal pose search when the character's movement state changes
     * (velocity > threshold) or when a transition clip starts playing.
     */
    void SetClipLock(bool lock) { clipLockActive = lock; }
    bool IsClipLocked() const { return clipLockActive; }
    // Returns true when the matcher has a valid clip to play while clip-lock
    // is engaged. After a transition clip completes, CompleteTransitionNow()
    // resets currentPoseIndex to 0 AND currentAnimationPtr to nullptr (forcing
    // re-selection). Clip-lock must NOT engage until BOTH are valid — otherwise
    // the animator is left with no clip to play (clipDur=-1). We require
    // currentApplicationPtr != nullptr (set in the "switch clip" or "same
    // animation" branch of SearchAndBlend) rather than just currentPoseIndex
    // because the latter is set to 0 by CompleteTransitionNow but the former
    // remains null until the next search populates it.
    bool IsClipLockReady() const {
        return currentPoseIndex >= 0 && currentAnimationPtr != nullptr;
    }
    
    /**
     * Check if motion matching is active
     */
    bool IsActive() const { return initialized; }
    
    // =========================================================================
    // DEBUG
    // =========================================================================
    
    /**
     * Enable/disable debug mode
     */
    void SetDebugEnabled(bool enabled) { debug.enabled = enabled; }
    bool IsDebugEnabled() const { return debug.enabled; }
    
    /**
     * Get debug info
     */
    const MotionMatchingDebug& GetDebugInfo() const { return debug; }
    
    /**
     * Print debug info to console
     */
    void PrintDebugInfo() const;

    /**
     * Aggregated stats for the Profiler / console flush.
     * Combines KD-tree geometry, search timing, and database scale into one
     * snapshot that creators can read at a glance.  No editor dependency —
     * the caller (AnimatedCharacter) pushes these into the Profiler.
     */
    struct Stats {
        // KD-Tree geometry
        size_t kdTreeNodes     = 0;
        size_t kdTreeLeaves    = 0;
        size_t kdTreeInternal  = 0;
        int    kdTreeMaxDepth  = 0;
        float  kdTreeAvgLeafSize = 0.0f;

        // Database scale
        size_t poseCount       = 0;
        size_t animationCount  = 0;

        // Last-frame search metrics (from debug struct)
        int    posesSearched   = 0;
        float  searchTimeMs    = 0.0f;
        float  searchScore     = 0.0f;

        // SIMD
        bool   simdAvailable   = false;
        bool   simdActive      = false;

        // Current pose
        std::string currentClip;
        float  currentClipTime = 0.0f;
        float  querySpeed      = 0.0f;
    };

    Stats GetStats() const;

    /**
     * Get database (for testing)
     */
    MotionDatabase* GetDatabase() { return database.get(); }
    const MotionDatabase* GetDatabase() const { return database.get(); }

    /**
     * Get the KD-tree (for accessing pre-built search structures).
     */
    const MotionKDTree& GetSearchTree() const { return searchTree; }
    MotionKDTree& GetSearchTree() { return searchTree; }

    /**
     * Get database stats
     */
    std::string GetDatabaseStats() const;

    /**
     * Does the current database contain airborne (Jump/Fall) poses?
     *
     * The character controller uses this to decide whether the pose search can
     * represent a jump arc (and drive it through MM) or whether it must fall
     * back to the legacy one-shot Jump override.
     */
    bool HasAirbornePoses() const {
        const MotionDatabase* db = currentDatabaseRef ? currentDatabaseRef : database.get();
        return db && db->HasAirbornePoses();
    }

    // =========================================================================
    // DATABASE SWITCHING (for Hybrid MM+FSM system)
    // =========================================================================

    /**
     * Set a new motion database
     *
     * Use for switching between different contexts:
     * - Walking database
     * - Crouching database
     * - Combat movement database
     * - Airborne database
     *
     * @param newDatabase Database to switch to (takes ownership via unique_ptr)
     * @param blendDuration How long to blend between databases (0 = instant)
     */
    void SetDatabase(std::unique_ptr<MotionDatabase> newDatabase, float blendDuration = 0.2f);

    /**
     * Non-owning overload: switch to a database the caller continues to own.
     *
     * Use this when the database is owned by an external container (e.g., a
     * map of state-specific databases that the caller needs to keep populated).
     * The MotionMatcher holds only a raw pointer and reads from the external
     * storage every frame. The caller MUST keep the referenced database alive
     * for as long as the MotionMatcher is using it (or call SetDatabase again
     * with a different owner).
     *
     * Semantically equivalent to SetDatabase(unique_ptr, ...) but without the
     * ownership transfer — fixes a class of use-after-move bugs where the
     * external container's slot got emptied by std::move and could never be
     * used again.
     *
     * @param newDatabase Database to switch to (caller retains ownership)
     * @param blendDuration How long to blend between databases (0 = instant)
     */
    void SetDatabase(const MotionDatabase& newDatabase, float blendDuration = 0.2f);

    /**
     * Zero-allocation database switch with a pre-built search tree.
     *
     * Instead of rebuilding the KD-Tree every frame (the source of FPS dips
     * during state switches), this method swaps in a reference to an
     * externally pre-built tree. The tree is built once at load time and
     * reused forever — the switch itself is a two-pointer address swap
     * costing 0.00 ms.
     *
     * @param newDatabase       Database the caller retains ownership of
     * @param preBuiltTree      KD-Tree already built against newDatabase's poses
     */
    void SetDatabaseExplicit(const MotionDatabase& newDatabase, const MotionKDTree& preBuiltTree);

    /**
     * FIX (v12 Section 2): Reset the database back to the primary owned DB and
     * the primary pre-built KD-Tree.  Called when leaving a state (e.g. Crouch→
     * Locomotion) so the matcher stops using the swapped-in crouch database/tree.
     * Zero allocation — just nulls the reference pointers so the owned
     * database.get() and searchTree are used again.
     */
    void ResetDatabaseExplicit();

    // =========================================================================
    // TRANSITION CLIP PLAYBACK (Structural Motion Graph — replaces instant
    // SetDatabaseExplicit swap with pre-computed transition clips)
    // =========================================================================

    /**
     * Play a pre-computed transition clip instead of instantly swapping
     * databases.
     *
     * Structural replacement for SetDatabaseExplicit(): instead of instantly
     * swapping the database pointer (which causes the walk→crouch snap because
     * all bone shapes jump to the target database's keys on frame 0), this
     * method plays a pre-baked transition clip that smoothly blends ALL joints
     * via slerp + linear root interpolation with 2D coordinate alignment
     * (Kovar & Gleicher §3.3, equations 1, 5, 6, 7).
     *
     * When the transition clip finishes, the database pointer is updated to
     * the target database, and normal motion matching resumes from the target
     * clip's starting pose.
     *
     * @param transitionClip Pre-computed transition Animation (from
     *        MotionTransitionGraph)
     * @param targetDatabase The database to switch to after the clip finishes
     * @param preBuiltTree KD-tree pre-built for the target database
     * @param targetClipIndex The pose in the target database to resume from
     *                        (the pose the transition clip ends at)
     */
    void PlayTransitionClip(
        std::shared_ptr<Animation> transitionClip,
        const MotionDatabase& targetDatabase,
        const MotionKDTree& preBuiltTree,
        int targetClipIndex = -1);

    /**
     * Check if a transition clip is currently playing.
     */
    bool IsPlayingTransitionClip() const { return transitionClipActive; }

    /**
     * Get the target database that will be active after transition clip
     * completes (for the HybridMMFSM to know which state it's transitioning to).
     */
    std::string GetTransitionTargetDatabase() const { return transitionTargetDBName; }

    /**
     * Get the target HybridState for transition tracking (as an opaque int
     * to avoid a circular include dependency with HybridMMFSM.h).
     */
    int GetTransitionTargetState() const { return transitionTargetState; }

    /**
     * Force-complete the transition (switch to target database immediately).
     * Called by HybridMMFSM when it's confident the transition has resolved.
     */
    void CompleteTransitionNow();

    /**
     * Set the target HybridState for transition tracking (as an opaque int
     * to avoid a circular include dependency with HybridMMFSM.h).
     */
    void SetTransitionTargetState(int state) { transitionTargetState = state; }

    /**
     * Get the target pose index in the target database to resume from after
     * the transition clip completes.
     */
    int GetTransitionTargetPoseIndex() const { return transitionTargetPoseIndex; }

    /**
     * Get the duration of the currently playing transition clip (seconds).
     * Returns 0 if no clip is playing.
     */
    float GetTransitionClipDuration() const {
        return transitionClipActive ? transitionClipDuration : 0.0f;
    }

    /**
     * Get the target database pointer (for HybridMMFSM to pass to other systems).
     */
    const MotionDatabase* GetTargetDatabase() const { return transitionTargetDB; }

    /**
     * Get the target KD-tree pointer (for HybridMMFSM database slot lookup).
     */
    const MotionKDTree* GetTargetSearchTree() const { return transitionTargetTree; }

    /**
     * Get current database name
     */
    std::string GetCurrentDatabaseName() const { return currentDatabaseName; }

    /**
     * Check if database is valid for motion matching
     *
     * Motion matching requires DYNAMIC root bones (root motion).
     * If all poses have near-zero velocity, MM won't work well.
     *
     * @return true if database has sufficient root motion
     */
    bool IsDatabaseValidForMM() const;

    /**
     * Get root motion threshold
     *
     * Poses with root velocity below this are considered "static"
     * and may not work well with motion matching.
     */
    float GetStaticRootThreshold() const { return staticRootThreshold; }

    /**
     * Set root motion threshold
     */
    void SetStaticRootThreshold(float threshold) { staticRootThreshold = threshold; }

    /**
     * Check if current animation has static root
     *
     * @return true if current pose has near-zero root velocity
     */
    bool HasStaticRoot() const;

    /**
     * Enable/disable motion matching fallback
     *
     * When enabled, MM will fall back to FSM-style playback
     * for animations with static root bones.
     */
    void SetStaticRootFallback(bool enabled) { enableStaticRootFallback = enabled; }
    bool GetStaticRootFallback() const { return enableStaticRootFallback; }

    /**
     * Enable/disable verbose console logging.
     *
     * The matcher logs per-frame chatter (Update ENTER, query speed, animation
     * switches). Editor character controllers silence their own matcher while
     * standalone tools keep the diagnostics. Default: enabled.
     */
    void SetVerbose(bool enabled) { verbose = enabled; }
    bool GetVerbose() const { return verbose; }

    /**
     * Set the world-space floor height used by foot IK.
     *
     * Defaults to 0.0f (flat floor). Set it to the terrain height under the
     * character each frame so planted feet lock at the ground level instead of
     * y=0.
     */
    void SetFloorHeight(float height) { m_floorHeight = height; }
    float GetFloorHeight() const { return m_floorHeight; }

    // World-space transform of the character (translate * rotate * scale),
    // used by the foot IK. The IK must evaluate feet in WORLD space: in model
    // space a planted foot slides backward under a walking body (the root
    // advances), which defeated every stationary/plant check. With the world
    // transform, a planted foot is genuinely stationary and the lock engages.
    void SetCharacterModelMatrix(const glm::mat4& m) {
        m_prevModelMatrix = m_modelMatrix;  // save prior frame's matrix
        m_modelMatrix = m;
        // ── Phase 1 (Retargeting): Scale-Normalize SIMD Queries ─────────
        // Extract the uniform scale from the model matrix so SearchAndBlend
        // can normalize query velocities to the database's asset space. A
        // character retargeted 2× larger has root velocity that doubles in
        // world space; without normalization the KD-tree distance treats a
        // walk-speed query as a run (speed weight=25 → huge penalty),
        // mis-selecting clips. Dividing by characterScale keeps the query
        // metric scale-invariant.
        m_characterScale = glm::max(glm::length(glm::vec3(m[0])), 0.0001f);
    }
    const glm::mat4& GetCharacterModelMatrix() const { return m_modelMatrix; }
    float GetCharacterScale() const { return m_characterScale; }

    // Terrain heightmap (world x,z -> y) forwarded to the animator's foot IK for
    // per-foot ground-normal tilt (todo Part 3, Option A). Set by AnimatedCharacter.
    void SetTerrainFn(const std::function<float(float,float)>& fn) { m_terrainFn = fn; }

    // PUBLIC: called by AnimatedCharacter AFTER Animator::Update() so the
    // IK/pelvis-adjustment runs against this frame's bone positions (not
    // the 1-frame-stale poses that caused jittery legs / stretched knees
    // during motion-matching pose switches).
    void ApplyFootIK(float dt);

private:
    // Core systems (using unique_ptr for proper ownership)
    std::unique_ptr<MotionDatabase> database;
    const MotionDatabase* currentDatabaseRef{nullptr};  // Reference for database switching
    TrajectoryPredictor trajectoryPredictor;
    FootPlantingSystem footPlanting;
    MotionKDTree searchTree;  // KD-Tree for fast search
    const MotionKDTree* activeSearchTreeRef{nullptr};  // Pre-built tree ref for zero-alloc switches

    // Animator reference (order matters for initialization list)
    const Skeleton* skeleton{nullptr};  // Store skeleton for feature extraction
    Animator* animator{nullptr};

    // Configuration
    MotionMatchingConfig config;

    // Current state
    bool initialized{false};
    int currentPoseIndex{-1};
    float currentAnimTime{0.0f};

    // Character state
    glm::vec3 characterPosition{0.0f};
    glm::vec3 characterVelocity{0.0f};  // Model-space velocity (for KD-tree query)
    glm::vec3 characterWorldVelocity{0.0f};  // World-space m/s (for speed-dependent smoothing)
    float characterRotation{0.0f};  // Yaw in radians
    glm::vec2 moveDirection{0.0f, 1.0f};
    bool isGrounded{true};
    bool isCrouching{false};
    bool isJumping{false};  // Airborne this frame (UE-style state gate input)

    // Debug
    MotionMatchingDebug debug;

    // Database switching (for hybrid MM+FSM)
    std::string currentDatabaseName{"Default"};
    std::unique_ptr<MotionDatabase> pendingDatabase;  // Database being blended to (owning path)
    const MotionDatabase* pendingDatabaseRef{nullptr};  // Database being blended to (non-owning path)
    float databaseBlendProgress{0.0f};
    float databaseBlendDuration{0.0f};
    bool isBlendingDatabases{false};
    bool pendingDatabaseIsRef{false};  // Which pending slot is active

    // Static root detection
    float staticRootThreshold{0.1f};  // Velocity below this is considered "static"
    bool enableStaticRootFallback{false};  // DISABLED - always search for best pose
    bool hasStaticRoot{false};  // Current pose has static root

    // Verbose per-frame logging + world-space floor height for foot IK
    bool verbose{true};
    float m_floorHeight{0.0f};

    // Character scale (from model matrix), used to normalize query velocities
    // so retargeted characters of different sizes match the same database clips.
    float m_characterScale{1.0f};
    // World-space character transform for the foot IK (identity until the
    // caller provides it via SetCharacterModelMatrix).
    glm::mat4 m_modelMatrix{1.0f};
    // Previous frame's world transform — needed to undo last frame's
    // model-space bone positions correctly. Using the CURRENT frame's
    // modelMatrix on prevBoneWorldPos computes foot velocity as if the
    // body hadn't moved, dropping the foot-speed signal and breaking foot
    // planting (the leg-stretching bug). See part 1 of the audit.
    glm::mat4 m_prevModelMatrix{1.0f};
    // Terrain heightmap forwarded from AnimatedCharacter (world x,z->y).
    std::function<float(float,float)> m_terrainFn;

    // Track current animation to avoid redundant Play() calls
    Animation* currentAnimationPtr{nullptr};

    // Last-known foot-contact state (gait phase). Carried across context
    // database switches: the pose index is reset on a switch, so the first
    // query in the new context would otherwise seed the foot-plant features
    // to false and let the search pick an arbitrary mid-swing pose (visible
    // pose mismatch / footskate when entering crouch). Persisting the phase
    // makes the entry pose land in the same foot-contact state (UE motion
    // phase across a database swap).
    bool lastFootPlantL_{false};
    bool lastFootPlantR_{false};

    // ---- Transition Clip Playback State (Structural Motion Graph) ----
    // When a pre-computed transition clip is playing (instead of normal
    // KD-tree search), these fields track playback progress. Replaces the
    // instant SetDatabaseExplicit swap with smooth all-joint blending.
    std::shared_ptr<Animation> transitionClip;  // The pre-baked transition
    bool transitionClipActive{false};           // Is a transition clip playing?
    float transitionClipTime{0.0f};             // Elapsed time in transition clip
    float transitionClipDuration{0.0f};         // Total duration of the clip
    const MotionDatabase* transitionTargetDB{nullptr};  // DB to activate when done
    const MotionKDTree* transitionTargetTree{nullptr};   // Tree to activate when done
    std::string transitionTargetDBName{"Default"};
    int transitionTargetState{0};  // Opaque HybridState (avoids circular include)
    int transitionTargetPoseIndex{-1};  // Pose to resume at in target DB

    // Clip lock mode: when true, skips KD-tree search and advances the current
    // clip's play time at real speed. Used for smooth idle cycle playback.
    bool clipLockActive{false};

    // Internal methods
    void UpdateTrajectory();
    void SearchAndBlend(float dt);

    void UpdateDebugInfo();
    void UpdateDatabaseBlend(float dt);
    bool CheckStaticRoot();

    /**
     * Unreal-style clip persistence with band override, shared by the KD-tree
     * and brute-force fallback search paths so they behave identically.
     *
     * Given the candidates sorted by distance, prefer the best pose in the
     * CURRENT clip unless a different clip clearly wins: it must beat the
     * current clip's best by the 15% distance margin, OR the query has left
     * the current clip's nominal speed band (gait change: run<->walk,
     * walk<->stop; band = nominalSpeed * config.speedBandFactor, min 1 m/s),
     * OR it is moving more than config.directionBandRadians away from the
     * clip's nominal heading (reversal / backpedal). Returns the chosen pose
     * index (-1 if no candidates) and writes its distance to outBestDist.
     */
    int SelectPoseWithPersistence(const MotionDatabase* db, int currentPoseIndex,
                                  const MotionFeatures& query,
                                  const std::vector<KDTSearchResult>& candidates,
                                  float& outBestDist) const;

    // The database the search tree is built from: the non-owning reference
    // (set by SetDatabase(const MotionDatabase&)) when a context switch is
    // active, else the owned database. EVERY pose lookup (search results,
    // persistence, debug, foot IK) must go through this - the tree indices
    // refer to this pose array, and reading the owned database instead would
    // index into a different array (garbage poses or out-of-bounds).
    const MotionDatabase* EffectiveDatabase() const {
        return currentDatabaseRef ? currentDatabaseRef : database.get();
    }
};
