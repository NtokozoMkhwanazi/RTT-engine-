#pragma once
#include "Animation.h"
#include "Animator.h"
#include "AnimationStateMachine.h"
#include "../motionMatching/MotionMatcher.h"
#include <memory>
#include <vector>
#include <functional>
#include <glm/glm.hpp>

// ============================================================================
// HYBRID ANIMATION GRAPH (MM + FSM)
// ============================================================================
//
// This system combines Motion Matching and Finite State Machines:
//
// 1. LOCOMOTION (Motion Matching):
//    - Handles idle, walk, run, crouch movement
//    - Smooth continuous blending via pose search
//    - Database switching for different contexts (ground/air/crouch)
//
// 2. STATE MANAGEMENT (FSM):
//    - Handles discrete state transitions
//    - States: Grounded, Falling, Jumping, Vaulting, Combat, etc.
//    - Each state can have its own Motion Matching database
//
// 3. LAYERED BLENDING:
//    - Base Layer: MM locomotion (lower body)
//    - Upper Layer: FSM actions (attacks, gestures, etc.)
//    - Additive Layer: Aim offsets, recoil, etc.
//
// Why MM doesn't work with static root bones:
// - MM searches based on ROOT VELOCITY features
// - Static root = no velocity = all poses look identical
// - Solution: Use FSM for static-root animations, MM for dynamic-root
// ============================================================================

/**
 * Hybrid State - High-level character state
 */
enum class HybridState {
    LOCOMOTION_GROUNDED,    // MM handles movement on ground
    LOCOMOTION_AIR,         // FSM handles jump/fall (static root or limited MM)
    LOCOMOTION_CROUCH,      // MM with crouch database
    VAULTING,               // FSM handles vault/climb animation
    COMBAT,                 // FSM handles combat state
    INTERACTION,            // FSM handles door open, pickup, etc.
    CUSTOM                  // Custom state for user-defined behavior
};

inline std::string HybridStateToString(HybridState state) {
    switch (state) {
        case HybridState::LOCOMOTION_GROUNDED: return "Locomotion_Grounded";
        case HybridState::LOCOMOTION_AIR: return "Locomotion_Air";
        case HybridState::LOCOMOTION_CROUCH: return "Locomotion_Crouch";
        case HybridState::VAULTING: return "Vaulting";
        case HybridState::COMBAT: return "Combat";
        case HybridState::INTERACTION: return "Interaction";
        case HybridState::CUSTOM: return "Custom";
        default: return "Unknown";
    }
}

/**
 * Hybrid State Transition Rule
 */
struct HybridTransition {
    HybridState fromState;
    HybridState toState;
    float blendDuration = 0.2f;
    std::function<bool()> condition;  // When to trigger this transition

    HybridTransition() = default;
    HybridTransition(HybridState from, HybridState to, float duration = 0.2f)
        : fromState(from), toState(to), blendDuration(duration) {}
};

/**
 * Animation Layer Configuration
 */
enum class AnimationLayerType {
    BASE_LOCOMOTION,    // Motion Matching (lower body or full body)
    UPPER_BODY_ACTION,  // FSM actions (attacks, gestures)
    ADDITIVE,           // Additive overlays (aim offset, recoil)
    FULL_BODY_OVERRIDE  // Full body FSM override (vault, climb)
};

struct AnimationLayerConfig {
    AnimationLayerType type = AnimationLayerType::BASE_LOCOMOTION;
    float weight = 1.0f;
    float blendInDuration = 0.1f;
    float blendOutDuration = 0.1f;
    bool enabled = true;

    // Bone mask for partial body blending
    std::vector<bool> boneMask;  // true = affected by this layer

    // For additive layers
    bool additive = false;
};

/**
 * Motion Database Slot
 *
 * Allows switching between different MM databases per state.
 * E.g., separate databases for: walking, crouching, combat movement
 */
struct MotionDatabaseSlot {
    std::string name;
    MotionDatabase database;
    bool isBuilt = false;

    void clear() {
        database.Clear();
        isBuilt = false;
    }
};

/**
 * Hybrid Animation Graph Configuration
 */
struct HybridAnimGraphConfig {
    // Motion Matching configuration
    bool enableMotionMatching = true;
    float mmBlendDuration = 0.1f;
    bool enableFootIK = true;

    // FSM configuration
    float fsmTransitionDuration = 0.2f;
    bool allowMMInAir = false;  // Use MM for falling (requires air animations)

    // Layering configuration
    bool enableUpperBodyLayer = true;
    bool enableAdditiveLayer = true;
    float layerBlendDuration = 0.15f;

    // Root bone handling
    bool useDynamicRootForMM = true;  // Only use MM for animations with root motion
    bool lockRootForStaticAnimations = true;
};

/**
 * Character Motion State (input to hybrid graph)
 */
struct HybridCharacterState {
    // Position/velocity
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 acceleration{0.0f};
    float rotation{0.0f};  // Yaw in radians

    // Input
    glm::vec2 moveDirection{0.0f, 1.0f};
    float moveMagnitude{0.0f};
    bool jumpPressed{false};
    bool crouchPressed{false};
    bool sprintPressed{false};

    // State flags
    bool grounded{true};
    bool crouching{false};
    bool sprinting{false};
    bool jumping{false};
    bool falling{false};

    // Combat/interaction
    bool attackPressed{false};
    bool interactPressed{false};
    glm::vec2 aimDirection{0.0f, 1.0f};

    // Derived
    float speed() const { return glm::length(velocity); }
    bool isMoving() const { return moveMagnitude > 0.1f; }
};

/**
 * Hybrid Animation Graph - Main Class
 *
 * Usage:
 *   HybridAnimGraph graph;
 *   graph.Initialize(skeleton, animator);
 *
 *   // Load animations into appropriate databases
 *   graph.LoadLocomotionAnimation("Walk", walkAnim);
 *   graph.LoadStateAnimation("Jump", jumpAnim, HybridState::LOCOMOTION_AIR);
 *
 *   // Build databases
 *   graph.BuildDatabases();
 *
 *   // Each frame:
 *   graph.Update(dt, characterState);
 */
class HybridAnimGraph {
public:
    HybridAnimGraph();
    ~HybridAnimGraph();

    // =========================================================================
    // INITIALIZATION
    // =========================================================================

    /**
     * Initialize hybrid animation graph
     *
     * @param skeleton Character skeleton
     * @param animator Animator to control
     */
    void Initialize(const Skeleton* skeleton, Animator* animator);

    /**
     * Set configuration
     */
    void SetConfig(const HybridAnimGraphConfig& config) { this->config = config; }
    const HybridAnimGraphConfig& GetConfig() const { return config; }

    // =========================================================================
    // ANIMATION LOADING
    // =========================================================================

    /**
     * Load animation into locomotion database (for MM)
     *
     * Use for: Idle, Walk, Run, CrouchWalk
     * These should have DYNAMIC root motion.
     *
     * @param name Animation name
     * @param anim Animation to load
     */
    void LoadLocomotionAnimation(const std::string& name, std::shared_ptr<Animation> anim);

    /**
     * Load animation into state-specific database
     *
     * Use for: Jump, Fall, Vault, Climb, etc.
     * Can have STATIC or DYNAMIC root motion.
     *
     * @param name Animation name
     * @param anim Animation to load
     * @param state Which state this animation belongs to
     */
    void LoadStateAnimation(const std::string& name, std::shared_ptr<Animation> anim, HybridState state);

    /**
     * Load upper-body action animation
     *
     * Use for: Attacks, gestures, reloads, etc.
     * These blend over locomotion.
     *
     * @param name Animation name
     * @param anim Animation to load
     * @param boneMask Which bones are affected (true = affected)
     */
    void LoadUpperBodyAnimation(const std::string& name, std::shared_ptr<Animation> anim,
                                const std::vector<bool>& boneMask);

    /**
     * Load additive animation
     *
     * Use for: Aim offsets, recoil, breathing
     *
     * @param name Animation name
     * @param anim Animation to load (should be additive pose)
     */
    void LoadAdditiveAnimation(const std::string& name, std::shared_ptr<Animation> anim);

    /**
     * Build all databases (call after loading animations)
     */
    void BuildDatabases();

    // =========================================================================
    // STATE MANAGEMENT
    // =========================================================================

    /**
     * Add transition rule
     */
    void AddTransition(const HybridTransition& transition);

    /**
     * Add transition with condition
     */
    void AddTransition(HybridState from, HybridState to, float duration, std::function<bool()> condition);

    /**
     * Get current state
     */
    HybridState GetCurrentState() const { return currentState; }

    /**
     * Get previous state
     */
    HybridState GetPreviousState() const { return previousState; }

    /**
     * Check if in specific state
     */
    bool IsInState(HybridState state) const { return currentState == state; }

    /**
     * Check if transitioning
     */
    bool IsTransitioning() const { return isTransitioning; }

    // =========================================================================
    // MAIN UPDATE
    // =========================================================================

    /**
     * Update hybrid animation graph
     *
     * Call every frame to update character pose.
     *
     * @param dt Delta time
     * @param state Character state (input)
     */
    void Update(float dt, const HybridCharacterState& state);

    // =========================================================================
    // LAYER CONTROL
    // =========================================================================

    /**
     * Play upper-body action
     *
     * @param animName Animation to play
     * @param weight Blend weight (0-1)
     * @param loop Should it loop?
     */
    void PlayUpperBodyAction(const std::string& animName, float weight = 1.0f, bool loop = false);

    /**
     * Stop upper-body action
     *
     * @param animName Animation to stop
     * @param fadeOutDuration Fade out time
     */
    void StopUpperBodyAction(const std::string& animName, float fadeOutDuration = 0.2f);

    /**
     * Set upper-body layer weight
     */
    void SetUpperBodyWeight(float weight, float duration = 0.1f);

    /**
     * Play additive animation
     */
    void PlayAdditive(const std::string& animName, float weight = 1.0f);

    /**
     * Stop additive animation
     */
    void StopAdditive(const std::string& animName, float fadeOutDuration = 0.2f);

    // =========================================================================
    // DATABASE SWITCHING
    // =========================================================================

    /**
     * Switch to a different motion database
     *
     * Use for: Changing from walk to crouch database
     *
     * @param databaseName Name of database to switch to
     * @param blendDuration How long to blend
     */
    void SwitchDatabase(const std::string& databaseName, float blendDuration = 0.2f);

    /**
     * Get current database name
     */
    std::string GetCurrentDatabase() const { return currentDatabaseName; }

    // =========================================================================
    // DEBUG
    // =========================================================================

    /**
     * Get debug info string
     */
    std::string GetDebugInfo() const;

    /**
     * Print debug info to console
     */
    void PrintDebugInfo() const;

    /**
     * Enable/disable debug mode
     */
    void SetDebugEnabled(bool enabled) { debugEnabled = enabled; }
    bool IsDebugEnabled() const { return debugEnabled; }

private:
    // Core systems
    Animator* animator{nullptr};
    const Skeleton* skeleton{nullptr};

    // Configuration
    HybridAnimGraphConfig config;

    // State machine
    HybridState currentState{HybridState::LOCOMOTION_GROUNDED};
    HybridState previousState{HybridState::LOCOMOTION_GROUNDED};
    std::vector<HybridTransition> transitions;
    bool isTransitioning{false};
    float transitionProgress{0.0f};
    float transitionDuration{0.0f};
    HybridState transitionFromState{HybridState::LOCOMOTION_GROUNDED};
    HybridState transitionToState{HybridState::LOCOMOTION_GROUNDED};

    // Motion Matching (for locomotion)
    MotionMatcher motionMatcher;
    MotionDatabaseSlot primaryDatabase;  // Main locomotion database
    std::unordered_map<HybridState, MotionDatabaseSlot> stateDatabases;  // Per-state databases
    std::string currentDatabaseName{"Primary"};

    // FSM animations (for non-locomotion states)
    std::unordered_map<std::string, std::shared_ptr<Animation>> stateAnimations;

    // Layered animations
    struct ActiveLayer {
        std::string name;
        std::shared_ptr<Animation> animation;
        AnimationLayerConfig config;
        float currentTime{0.0f};
        float currentWeight{0.0f};
        float targetWeight{0.0f};
        bool loop{false};
        bool finished{false};
    };
    std::vector<ActiveLayer> upperBodyLayers;
    std::vector<ActiveLayer> additiveLayers;

    // Character state
    HybridCharacterState characterState;

    // Debug
    bool debugEnabled{false};

    // Internal methods
    void UpdateStateMachine(float dt);
    void UpdateMotionMatching(float dt);
    void UpdateFSMAnimation(float dt);
    void UpdateLayers(float dt);
    void EvaluateTransitions();
    void StartTransition(HybridState toState);
    void ApplyLayerBlending(float dt);
    void SetupDefaultTransitions();

    // Bone mask helpers
    std::vector<bool> CreateLowerBodyMask() const;
    std::vector<bool> CreateUpperBodyMask() const;
    std::vector<bool> CreateFullBodyMask() const;
};
