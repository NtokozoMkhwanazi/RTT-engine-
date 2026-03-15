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
    float blendProgress{0.0f};
    int blendFromPose{-1};
    int blendToPose{-1};

    // Character state
    glm::vec3 characterPosition{0.0f};
    glm::vec3 characterVelocity{0.0f};
    float characterRotation{0.0f};  // Yaw in radians
    glm::vec2 moveDirection{0.0f, 1.0f};
    bool isGrounded{true};
    bool isCrouching{false};

    // Debug
    MotionMatchingDebug debug;

    // Database switching (for hybrid MM+FSM)
    std::string currentDatabaseName{"Default"};
    std::unique_ptr<MotionDatabase> pendingDatabase;  // Database being blended to
    float databaseBlendProgress{0.0f};
    float databaseBlendDuration{0.0f};
    bool isBlendingDatabases{false};

    // Static root detection
    float staticRootThreshold{0.1f};  // Velocity below this is considered "static"
    bool enableStaticRootFallback{false};  // DISABLED - always search for best pose
    bool hasStaticRoot{false};  // Current pose has static root

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
