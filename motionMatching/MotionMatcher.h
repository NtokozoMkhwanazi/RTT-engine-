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
    MotionDatabase& GetDatabase() { return database; }
    const MotionDatabase& GetDatabase() const { return database; }
    
    /**
     * Get database stats
     */
    std::string GetDatabaseStats() const;
    
private:
    // Core systems
    MotionDatabase database;
    TrajectoryPredictor trajectoryPredictor;
    FootPlantingSystem footPlanting;
    MotionKDTree searchTree;  // KD-Tree for fast search
    
    // Animator reference
    Animator* animator{nullptr};
    const Skeleton* skeleton{nullptr};  // Store skeleton for feature extraction
    
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
    
    // Internal methods
    void UpdateTrajectory();
    void SearchAndBlend(float dt);
    void ApplyFootIK(float dt);
    void UpdateDebugInfo();
};
