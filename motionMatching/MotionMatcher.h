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
     * Get database (for testing)
     */
    MotionDatabase* GetDatabase() { return database.get(); }
    const MotionDatabase* GetDatabase() const { return database.get(); }

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
    void SetCharacterModelMatrix(const glm::mat4& m) { m_modelMatrix = m; }
    const glm::mat4& GetCharacterModelMatrix() const { return m_modelMatrix; }

private:
    // Core systems (using unique_ptr for proper ownership)
    std::unique_ptr<MotionDatabase> database;
    const MotionDatabase* currentDatabaseRef{nullptr};  // Reference for database switching
    TrajectoryPredictor trajectoryPredictor;
    FootPlantingSystem footPlanting;
    MotionKDTree searchTree;  // KD-Tree for fast search

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
    glm::vec3 characterVelocity{0.0f};
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
    // World-space character transform for the foot IK (identity until the
    // caller provides it via SetCharacterModelMatrix).
    glm::mat4 m_modelMatrix{1.0f};

    // Track current animation to avoid redundant Play() calls
    Animation* currentAnimationPtr{nullptr};

    // Internal methods
    void UpdateTrajectory();
    void SearchAndBlend(float dt);
    void ApplyFootIK(float dt);
    void UpdateDebugInfo();
    void UpdateDatabaseBlend(float dt);
    bool CheckStaticRoot();
};
