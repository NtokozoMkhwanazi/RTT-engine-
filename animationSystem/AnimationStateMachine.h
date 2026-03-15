#pragma once
#include "Animation.h"
#include "Animator.h"
#include <string>
#include <map>
#include <vector>
#include <functional>
#include <glm/glm.hpp>

// ========================================
// Animation States
// ========================================
enum class AnimationState {
    IDLE,
    WALK,
    RUN,
    JUMP,
    FALL,
    CROUCH,
    CROUCH_WALK,
    DEATH,
    NONE
};

inline std::string AnimationStateToString(AnimationState state) {
    switch (state) {
        case AnimationState::IDLE: return "Idle";
        case AnimationState::WALK: return "Walk";
        case AnimationState::RUN: return "Run";
        case AnimationState::JUMP: return "Jump";
        case AnimationState::FALL: return "Fall";
        case AnimationState::CROUCH: return "Crouch";
        case AnimationState::CROUCH_WALK: return "Crouch Walk";
        case AnimationState::DEATH: return "Death";
        default: return "None";
    }
}

// ========================================
// Animation State Transition
// ========================================
struct AnimationTransition {
    AnimationState fromState;
    AnimationState toState;
    float blendDuration = 0.2f;
    std::function<bool()> condition;  // Condition to trigger transition
    
    AnimationTransition() = default;
    AnimationTransition(AnimationState from, AnimationState to, float duration = 0.2f)
        : fromState(from), toState(to), blendDuration(duration) {}
};

// ========================================
// Animation State Data
// ========================================
struct AnimationStateData {
    Animation* animation = nullptr;
    float speed = 1.0f;
    bool loop = true;
    bool additive = false;
    std::string name;
};

// ========================================
// Character Movement Input
// ========================================
struct CharacterInput {
    glm::vec2 moveDirection{0.0f, 0.0f};  // WASD input (-1 to 1)
    float moveMagnitude = 0.0f;            // Length of moveDirection
    bool jump = false;
    bool crouch = false;
    bool sprint = false;
    bool grounded = true;
    float verticalVelocity = 0.0f;         // For jump/fall detection
    
    void reset() {
        moveDirection = glm::vec2(0.0f);
        moveMagnitude = 0.0f;
        jump = false;
        crouch = false;
        sprint = false;
        verticalVelocity = 0.0f;
    }
};

// ========================================
// Animation State Machine
// ========================================
class AnimationStateMachine {
public:
    AnimationStateMachine(Animator* animator);
    ~AnimationStateMachine() = default;

    // State management
    void setState(AnimationState state);
    AnimationState getCurrentState() const { return currentState; }
    AnimationState getPreviousState() const { return previousState; }

    // Initialization
    void initialize();  // Start with initial state (IDLE)

    // Animation registration
    void registerAnimation(AnimationState state, Animation* anim, float speed = 1.0f, bool loop = true);
    void registerAnimations(
        Animation* idle, Animation* walk, Animation* run,
        Animation* jump = nullptr, Animation* fall = nullptr,
        Animation* crouch = nullptr, Animation* crouchWalk = nullptr
    );

    // GRADIENT BAND INTERPOLATION (Blend Space)
    // Uses continuous blending instead of discrete states for locomotion
    void setBlendSpaceEnabled(bool enabled) { useBlendSpace = enabled; }
    bool isBlendSpaceEnabled() const { return useBlendSpace; }
    
    // Blend band configuration
    void setBlendBands(float idleToWalk, float walkToRun);
    void setWalkRunBlendThreshold(float walkThreshold, float runThreshold);  // Legacy support
    float getIdleToWalkThreshold() const { return idleToWalkThreshold; }
    float getWalkToRunThreshold() const { return walkToRunThreshold; }

    // Transition management
    void addTransition(const AnimationTransition& transition);
    void addTransition(AnimationState from, AnimationState to, float duration, std::function<bool()> condition);
    void clearTransitions();

    // Update
    void update(float dt, const CharacterInput& input);
    void update(float dt, float speed, bool grounded, bool jumping, bool crouching, bool sprinting);

    // Parameters
    void setSpeed(float speed) { movementSpeed = speed; }
    void setGrounded(bool grounded) { isGrounded = grounded; }
    void setVerticalVelocity(float velocity) { verticalVelocity = velocity; }

    // Blending
    void setBlendDuration(float duration) { defaultBlendDuration = duration; }

    // Debug
    std::string getDebugInfo() const;
    void printState() const;

    // State queries
    bool isInState(AnimationState state) const { return currentState == state; }
    bool isTransitioning() const { return isTransitioningState; }
    float getTransitionProgress() const { return transitionProgress; }
    
    // Blend space queries
    float getCurrentBlendWeight() const { return blendWeight; }
    float getTargetBlendWeight() const { return targetBlendWeight; }
    
    // GRADIENT BAND INTERPOLATION
    void applyBlendSpaceAnimation(float dt);  // Apply blended animations based on weight

    // Movement parameters
    float getMovementSpeed() const { return movementSpeed; }
    float getMaxWalkSpeed() const { return maxWalkSpeed; }
    float getMaxRunSpeed() const { return maxRunSpeed; }
    
private:
    Animator* animator;
    AnimationState currentState = AnimationState::NONE;  // Start with NONE, not IDLE
    AnimationState previousState = AnimationState::NONE;
    
    std::map<AnimationState, AnimationStateData> stateAnimations;
    std::vector<AnimationTransition> transitions;
    
    // Movement parameters
    float movementSpeed = 0.0f;
    float smoothedSpeed = 0.0f;
    float speedSmoothRate = 20.0f;  // How fast to smooth speed changes (INCREASED for responsiveness)
    bool isGrounded = true;
    float verticalVelocity = 0.0f;

    // Edge detection for key presses
    bool prevMoving = false;
    bool prevSprinting = false;
    bool prevJump = false;
    bool prevCrouch = false;

    // One-shot animation tracking (for jump, etc.)
    bool jumpAnimationPlaying = false;
    float jumpAnimationStartTime = 0.0f;

    // GRADIENT BAND INTERPOLATION (Blend Space)
    bool useBlendSpace = true;  // Enable continuous blending for locomotion
    float idleToWalkThreshold = 0.3f;   // Speed where idle→walk blend starts
    float walkToRunThreshold = 0.6f;    // Speed where walk→run blend starts
    float blendWeight = 0.0f;           // Current blend weight (0=idle, 0.5=walk, 1=run)
    float targetBlendWeight = 0.0f;     // Target blend weight
    float blendSmoothRate = 30.0f;      // How fast blend weight changes (INCREASED for instant response)

    // Blend thresholds
    float maxWalkSpeed = 2.0f;
    float maxRunSpeed = 6.0f;

    // Transition state
    bool isTransitioningState = false;
    float transitionProgress = 0.0f;
    float transitionDuration = 0.08f;  // REDUCED from 0.2f for snappy transitions
    AnimationState transitionFromState = AnimationState::NONE;
    AnimationState transitionToState = AnimationState::NONE;
    
    // Default blend duration
    float defaultBlendDuration = 0.15f;
    
    // Input tracking
    CharacterInput currentInput;
    
    // Internal methods
    void evaluateTransitions();
    void startTransition(AnimationState toState, float duration);
    void updateTransition(float dt);
    void applyAnimationBlend();
    
    // State-specific updates
    void updateIdle();
    void updateWalk();
    void updateRun();
    void updateJump();
    void updateFall();
    void updateCrouch();
    void updateCrouchWalk();
};

// ========================================
// Animation Graph (Advanced Blending)
// ========================================
class AnimationGraph {
public:
    struct BlendNode {
        Animation* animation;
        float weight;
        glm::vec2 blendPosition;  // Position in blend space
    };
    
    void addBlendNode(Animation* anim, const glm::vec2& position);
    void updateWeights(const glm::vec2& targetPosition);
    void applyToAnimator(Animator* animator, float dt);
    
private:
    std::vector<BlendNode> nodes;
    glm::vec2 currentPosition;
};
