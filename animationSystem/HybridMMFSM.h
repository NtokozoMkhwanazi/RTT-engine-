#pragma once
#include "AnimationStateMachine.h"
#include "HybridState.h"
#include "../motionMatching/MotionMatcher.h"
#include "../motionMatching/MotionDatabase.h"
#include "../motionMatching/MotionTransitionGraph.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <functional>
#include <string>

// ============================================================================
// HYBRID MM + FSM SYSTEM (FIXED - Proper shared_ptr ownership)
// ============================================================================

/**
 * Hybrid Transition with Inertialization Support
 */
struct HybridTransition {
    HybridState from;
    HybridState to;
    float blendDuration = 0.1f;
    float inertializationDuration = 0.15f;  // For momentum-based blending
    std::function<bool()> condition;
    bool useInertialization = true;  // Use inertialization instead of crossfade
    
    // Explicit default constructor to ensure proper initialization
    HybridTransition() 
        : from(HybridState::LOCOMOTION)
        , to(HybridState::LOCOMOTION)
        , blendDuration(0.1f)
        , inertializationDuration(0.15f)
        , condition(nullptr)
        , useInertialization(true) {}
};

/**
 * Character State Input (for Hybrid MM+FSM)
 */
struct HybridMMFSMState {
    // Input
    glm::vec2 moveDirection{0.0f, 0.0f};
    float moveMagnitude{0.0f};
    bool jump{false};
    bool crouch{false};
    bool sprint{false};

    // State
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::vec3 velocity{0.0f, 0.0f, 0.0f};
    float rotation{0.0f};
    bool grounded{true};

    // Explicit default constructor to ensure proper initialization
    HybridMMFSMState()
        : moveDirection(0.0f, 0.0f)
        , moveMagnitude(0.0f)
        , jump(false)
        , crouch(false)
        , sprint(false)
        , position(0.0f, 0.0f, 0.0f)
        , velocity(0.0f, 0.0f, 0.0f)
        , rotation(0.0f)
        , grounded(true) {}
};

/**
 * Inertialization State - Tracks momentum during transitions
 *
 * This is CRITICAL for realistic state changes. Instead of simple crossfading,
 * we preserve momentum and let animations drift back into sync naturally.
 */
struct InertializationState {
    bool active{false};
    float progress{0.0f};
    float duration{0.0f};

    // Source state (where we're transitioning FROM)
    glm::vec3 sourceRootPos{0.0f, 0.0f, 0.0f};
    glm::quat sourceRootRot{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 sourceVelocity{0.0f, 0.0f, 0.0f};

    // Target state (where we're transitioning TO)
    glm::vec3 targetRootPos{0.0f, 0.0f, 0.0f};
    glm::quat targetRootRot{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 targetVelocity{0.0f, 0.0f, 0.0f};

    // Momentum preservation
    glm::vec3 preservedMomentum{0.0f, 0.0f, 0.0f};
    float driftRecoveryRate{5.0f};  // How fast to recover from drift

    // InertializationState no longer carries stature offset or pose snapshot
    // fields. These were runtime patches for walk→crouch snap that are
    // replaced by the pre-computed transition clips in MotionTransitionGraph.
    // The transition clips structurally handle height differences, root
    // alignment, and all-joint blending at load time — no runtime patches needed.

    void Reset() {
        active = false;
        progress = 0.0f;
        duration = 0.0f;
        sourceRootPos = glm::vec3(0.0f, 0.0f, 0.0f);
        sourceRootRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        sourceVelocity = glm::vec3(0.0f, 0.0f, 0.0f);
        targetRootPos = glm::vec3(0.0f, 0.0f, 0.0f);
        targetRootRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        targetVelocity = glm::vec3(0.0f, 0.0f, 0.0f);
        preservedMomentum = glm::vec3(0.0f, 0.0f, 0.0f);
        driftRecoveryRate = 5.0f;
    }
};

/**
 * REMOVED: Standstill Posture Lock (StableIdleSnapshot).
 *
 * Previously captured a frozen skeleton snapshot on the first frame of rest
 * and replayed it for all subsequent idle frames, bypassing the live IK/loop
 * evaluation to prevent sub-millimetre knee jitter ("IK Loop Churn").
 *
 * Now replaced by: continuous motion matching through the structural
 * transition graph. Idle poses are selected from the motion database each
 * frame, and foot IK applies sub-frame damping (ApplyFootIK) to eliminate
 * jitter without freezing — all clips flow continuously through the graph.
 */

class HybridMMFSM {
public:
    HybridMMFSM();
    ~HybridMMFSM();

    // =========================================================================
    // INITIALIZATION
    // =========================================================================

    void Initialize(const Skeleton* skeleton, Animator* animator);

    // =========================================================================
    // ANIMATION LOADING (FIXED - Proper shared_ptr ownership)
    // =========================================================================

    /**
     * Load locomotion animations (for MM)
     * 
     * CRITICAL: This takes OWNERSHIP via shared_ptr.
     * The animation will be stored in the motion matcher's database.
     * 
     * Usage:
     *   auto walkAnim = std::make_shared<Animation>("Walk", duration, fps);
     *   hybridFSM.LoadLocomotionAnimation("Walk", walkAnim);
     *   // walkAnim is now owned by the system - don't delete it!
     * 
     * @param name Animation name (e.g., "Walk", "Run")
     * @param anim Animation to load (MUST be std::shared_ptr<Animation>)
     */
    void LoadLocomotionAnimation(const std::string& name, std::shared_ptr<Animation> anim);

    /**
     * Load state-specific animation (for FSM states)
     * 
     * CRITICAL: This takes OWNERSHIP via shared_ptr.
     * The animation is stored in the state's motion database.
     * 
     * Usage:
     *   auto jumpAnim = std::make_shared<Animation>("Jump", duration, fps);
     *   hybridFSM.LoadStateAnimation(HybridState::JUMP, "Jump", jumpAnim);
     *   // jumpAnim is now owned by the system
     * 
     * @param state Which FSM state this belongs to
     * @param name Animation name
     * @param anim Animation to load (MUST be std::shared_ptr<Animation>)
     */
    void LoadStateAnimation(HybridState state, const std::string& name, std::shared_ptr<Animation> anim);

    /**
     * Build all motion databases (call after loading animations)
     * 
     * This constructs KD-Trees for fast pose searching.
     */
    void BuildDatabases();

    // =========================================================================
    // STATE MANAGEMENT
    // =========================================================================

    /**
     * Add transition with inertialization support
     */
    void AddTransition(HybridState from, HybridState to, float duration,
                       std::function<bool()> condition, bool useInertialization = true);

    /**
     * Add transition with custom inertialization duration
     */
    void AddTransitionWithInertialization(HybridState from, HybridState to,
                                          float blendDuration, float inertializationDuration,
                                          std::function<bool()> condition);

    HybridState GetCurrentState() const { return currentState; }
    bool IsInState(HybridState state) const { return currentState == state; }
    bool IsTransitioning() const { return isTransitioning || inertialization.active || m_transitionClipPlaying; }
    bool IsUsingInertialization() const { return inertialization.active; }
    
    // Get current character state (for transition conditions)
    const HybridMMFSMState& GetCharacterState() const { return characterState; }
    
    // Get motion matcher debug info
    std::string GetMotionMatcherDebug() const;
    
    // Get motion matcher database size
    size_t GetMotionMatcherDatabaseSize() const;

    // =========================================================================
    // MAIN UPDATE
    // =========================================================================

    void Update(float dt, const HybridMMFSMState& state);

    // =========================================================================
    // DEBUG
    // =========================================================================

    std::string GetDebugInfo() const;
    void SetDebugEnabled(bool enabled) { debugEnabled = enabled; }

    // =========================================================================
    // DATABASE ACCESS (for advanced usage)
    // =========================================================================

    /**
     * Get motion database for a specific state
     */
    const MotionDatabase* GetStateDatabase(HybridState state) const;

    /**
     * Check if state has a valid motion database
     */
    bool HasStateDatabase(HybridState state) const;

private:
    const Skeleton* skeleton{nullptr};
    Animator* animator{nullptr};

    // Motion Matching for locomotion
    MotionMatcher motionMatcher;
    bool mmActive{false};

    // FSM states
    HybridState currentState{HybridState::LOCOMOTION};
    HybridState previousState{HybridState::LOCOMOTION};
    std::vector<HybridTransition> transitions;
    bool isTransitioning{false};
    float transitionProgress{0.0f};
    float transitionDuration{0.0f};
    HybridState transitionFrom{HybridState::LOCOMOTION};
    HybridState transitionTo{HybridState::LOCOMOTION};

    // Inertialization state (for smooth transitions)
    InertializationState inertialization;

    // State-specific MM databases (OWN their animations via shared_ptr).
    // FIX (v11 Section 2): Each state now owns its own pre-built KD-Tree so
    // that state switches are zero-allocation pointer swaps instead of
    // synchronous tree rebuilds that cause FPS dips.
    struct MotionDatabaseSlot {
        std::unique_ptr<MotionDatabase> database;
        std::unique_ptr<MotionKDTree> searchTree;
        bool isBuilt = false;

        // FIX (v12 Section 2): Pre-warm the database's internal SIMD SoA cache
        // at load time so that runtime SearchSIMD calls never trigger
        // mid-frame heap allocations when a database switch changes
        // simdCache_.poseCount.
        void WarmSIMDCache() {
            if (database)
                database->RebuildSIMDCacheIfNeeded();
        }
    };
    std::unordered_map<HybridState, MotionDatabaseSlot> hybridStateSlots;

    // Character state
    HybridMMFSMState characterState;

    // ---- Structural Motion Graph Transition State ----
    // Pre-computed transition clips replace the instant SetDatabaseExplicit
    // swap. When a state transition fires, HybridMMFSM looks up the pre-baked
    // transition clip from the transition graph and plays it via
    // MotionMatcher::PlayTransitionClip(). The clip smoothly blends ALL joints
    // (slerp + linear root + 2D alignment) over ~0.33s. When the clip finishes,
    // the MotionMatcher swaps to the target database seamlessly.
    bool m_transitionClipPlaying{false};
    HybridState m_transitionClipTarget{HybridState::LOCOMOTION};

    // Pre-computed transition clips between state databases (Kovar & Gleicher §3)
    MotionTransitionGraph transitionGraph;

    // Debug
    bool debugEnabled{false};

    // Internal methods
    void UpdateStateMachine(float dt);
    void UpdateLocomotion(float dt);
    void UpdateJump(float dt);
    void UpdateFall(float dt);
    void UpdateCrouch(float dt);
    void EvaluateTransitions();
    void StartTransition(HybridState toState);
    void UpdateInertialization(float dt);
    void ApplyInertializationBlending(float dt);
    
    /**
     * Calculate inertialization blend weight
     * Uses exponential decay for natural-looking momentum
     */
    float CalculateInertializationWeight(float progress) const;
};
