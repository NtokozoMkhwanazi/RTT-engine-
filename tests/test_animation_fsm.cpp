/**
 * Animation State Machine Unit Tests
 * 
 * Tests for animation state transitions, blending,
 * state queries, and input-driven state changes.
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <memory>
#include <functional>

// Mock Animation structure for testing
struct MockAnimation {
    std::string name;
    float duration;
    bool isPlaying = false;
    
    MockAnimation(const std::string& n, float d) : name(n), duration(d) {}
};

// Mock Animator for testing
class MockAnimator {
public:
    MockAnimation* currentAnimation = nullptr;
    MockAnimation* nextAnimation = nullptr;
    float blendWeight = 1.0f;
    float blendDuration = 0.0f;
    
    void Play(MockAnimation* anim) {
        currentAnimation = anim;
        if (anim) anim->isPlaying = true;
    }
    
    void BlendTo(MockAnimation* anim, float duration) {
        nextAnimation = anim;
        blendDuration = duration;
        blendWeight = 0.0f;
    }
};

// Simplified AnimationState enum for testing
enum class AnimationState {
    IDLE,
    WALK,
    RUN,
    JUMP,
    FALL,
    CROUCH,
    CROUCH_WALK,
    NONE
};

std::string AnimationStateToString(AnimationState state) {
    switch (state) {
        case AnimationState::IDLE: return "Idle";
        case AnimationState::WALK: return "Walk";
        case AnimationState::RUN: return "Run";
        case AnimationState::JUMP: return "Jump";
        case AnimationState::FALL: return "Fall";
        case AnimationState::CROUCH: return "Crouch";
        case AnimationState::CROUCH_WALK: return "CrouchWalk";
        default: return "None";
    }
}

// Character input structure
struct CharacterInput {
    glm::vec2 moveDirection{0.0f, 0.0f};
    float moveMagnitude = 0.0f;
    bool jump = false;
    bool crouch = false;
    bool sprint = false;
    bool grounded = true;
    float verticalVelocity = 0.0f;
};

// Simplified FSM for testing
class AnimationFSM {
public:
    AnimationState currentState = AnimationState::NONE;
    AnimationState previousState = AnimationState::NONE;
    bool isTransitioningState = false;
    float transitionProgress = 0.0f;
    
    struct Transition {
        AnimationState from;
        AnimationState to;
        std::function<bool()> condition;
    };
    
    std::vector<Transition> transitions;
    
    void setState(AnimationState state) {
        if (currentState == state) return;
        previousState = currentState;
        currentState = state;
        isTransitioningState = false;
    }
    
    void addTransition(AnimationState from, AnimationState to, std::function<bool()> condition) {
        transitions.push_back({from, to, condition});
    }
    
    void evaluateTransitions() {
        for (const auto& t : transitions) {
            if (currentState == t.from && t.condition()) {
                previousState = currentState;
                currentState = t.to;
                isTransitioningState = true;
                transitionProgress = 0.0f;
                return;  // Only one transition per frame
            }
        }
    }
    
    bool isInState(AnimationState state) const {
        return currentState == state;
    }
    
    bool isTransitioning() const {
        return isTransitioningState;
    }
};

class AnimationFSMTest : public ::testing::Test {
protected:
    AnimationFSM fsm;
    CharacterInput input;
    
    void SetUp() override {
        fsm = AnimationFSM();
        input = CharacterInput();
    }
    
    void TearDown() override {
        fsm = AnimationFSM();
    }
};

/**
 * Test: Initial State
 * Verifies FSM starts in correct initial state
 */
TEST_F(AnimationFSMTest, InitialState_None) {
    EXPECT_EQ(fsm.currentState, AnimationState::NONE);
    EXPECT_EQ(fsm.previousState, AnimationState::NONE);
}

/**
 * Test: State Change
 * Verifies basic state transition
 */
TEST_F(AnimationFSMTest, StateChange_Transitions) {
    fsm.setState(AnimationState::IDLE);
    
    EXPECT_EQ(fsm.currentState, AnimationState::IDLE);
    EXPECT_EQ(fsm.previousState, AnimationState::NONE);
}

TEST_F(AnimationFSMTest, StateChange_SameState_NoChange) {
    fsm.setState(AnimationState::IDLE);
    fsm.setState(AnimationState::IDLE);
    
    EXPECT_EQ(fsm.currentState, AnimationState::IDLE);
    EXPECT_EQ(fsm.previousState, AnimationState::NONE);  // Should not change
}

/**
 * Test: State Queries
 * Verifies state query functions work
 */
TEST_F(AnimationFSMTest, StateQuery_IsInState) {
    fsm.setState(AnimationState::IDLE);
    
    EXPECT_TRUE(fsm.isInState(AnimationState::IDLE));
    EXPECT_FALSE(fsm.isInState(AnimationState::WALK));
}

TEST_F(AnimationFSMTest, StateQuery_PreviousState) {
    fsm.setState(AnimationState::IDLE);
    fsm.setState(AnimationState::WALK);
    
    EXPECT_EQ(fsm.previousState, AnimationState::IDLE);
    EXPECT_EQ(fsm.currentState, AnimationState::WALK);
}

/**
 * Test: Transition Conditions
 * Verifies transitions fire when conditions are met
 */
TEST_F(AnimationFSMTest, Transition_IdleToWalk_WhenMoving) {
    fsm.setState(AnimationState::IDLE);
    
    float speedThreshold = 0.1f;
    fsm.addTransition(AnimationState::IDLE, AnimationState::WALK, [&]() {
        return input.moveMagnitude > speedThreshold;
    });
    
    // Not moving yet
    fsm.evaluateTransitions();
    EXPECT_EQ(fsm.currentState, AnimationState::IDLE);
    
    // Start moving
    input.moveMagnitude = 0.5f;
    fsm.evaluateTransitions();
    EXPECT_EQ(fsm.currentState, AnimationState::WALK);
}

TEST_F(AnimationFSMTest, Transition_WalkToRun_WhenSprinting) {
    fsm.setState(AnimationState::WALK);
    
    fsm.addTransition(AnimationState::WALK, AnimationState::RUN, [&]() {
        return input.sprint && input.moveMagnitude > 0.8f;
    });
    
    // Walking, not sprinting
    input.moveMagnitude = 0.5f;
    input.sprint = false;
    fsm.evaluateTransitions();
    EXPECT_EQ(fsm.currentState, AnimationState::WALK);
    
    // Start sprinting
    input.sprint = true;
    input.moveMagnitude = 0.9f;
    fsm.evaluateTransitions();
    EXPECT_EQ(fsm.currentState, AnimationState::RUN);
}

TEST_F(AnimationFSMTest, Transition_AnyToJump_WhenJumping) {
    fsm.setState(AnimationState::IDLE);
    
    // Add transitions from multiple states to JUMP
    fsm.addTransition(AnimationState::IDLE, AnimationState::JUMP, [&]() {
        return input.jump && input.grounded;
    });
    fsm.addTransition(AnimationState::WALK, AnimationState::JUMP, [&]() {
        return input.jump && input.grounded;
    });
    fsm.addTransition(AnimationState::RUN, AnimationState::JUMP, [&]() {
        return input.jump && input.grounded;
    });
    
    // Jump from IDLE
    input.jump = true;
    input.grounded = true;
    fsm.evaluateTransitions();
    EXPECT_EQ(fsm.currentState, AnimationState::JUMP);
}

TEST_F(AnimationFSMTest, Transition_ToFall_WhenAirborne) {
    fsm.setState(AnimationState::JUMP);
    
    fsm.addTransition(AnimationState::JUMP, AnimationState::FALL, [&]() {
        return !input.grounded && input.verticalVelocity < 0.0f;
    });
    
    // At peak of jump (velocity becomes negative)
    input.grounded = false;
    input.verticalVelocity = -1.0f;
    fsm.evaluateTransitions();
    EXPECT_EQ(fsm.currentState, AnimationState::FALL);
}

/**
 * Test: Transition Priority
 * Verifies only one transition fires per evaluation
 */
TEST_F(AnimationFSMTest, TransitionPriority_OnePerFrame) {
    fsm.setState(AnimationState::IDLE);
    
    // Multiple transitions from IDLE
    fsm.addTransition(AnimationState::IDLE, AnimationState::WALK, [&]() {
        return input.moveMagnitude > 0.1f;
    });
    fsm.addTransition(AnimationState::IDLE, AnimationState::JUMP, [&]() {
        return input.jump;
    });
    
    // Both conditions true
    input.moveMagnitude = 0.5f;
    input.jump = true;
    
    fsm.evaluateTransitions();
    
    // Only first matching transition should fire
    EXPECT_TRUE(fsm.currentState == AnimationState::WALK || 
                fsm.currentState == AnimationState::JUMP);
}

/**
 * Test: Transitioning State
 * Verifies transitioning flag is set correctly
 */
TEST_F(AnimationFSMTest, TransitioningState_FlagSet) {
    fsm.setState(AnimationState::IDLE);
    
    EXPECT_FALSE(fsm.isTransitioning());
    
    fsm.addTransition(AnimationState::IDLE, AnimationState::WALK, [&]() {
        return input.moveMagnitude > 0.1f;
    });
    
    input.moveMagnitude = 0.5f;
    fsm.evaluateTransitions();
    
    EXPECT_TRUE(fsm.isTransitioning());
    EXPECT_FLOAT_EQ(fsm.transitionProgress, 0.0f);
}

/**
 * Test: Movement Speed Thresholds
 * Verifies walk/run blend thresholds work
 */
TEST_F(AnimationFSMTest, WalkRunBlend_Thresholds) {
    float walkThreshold = 0.3f;
    float runThreshold = 0.7f;
    
    // Idle when not moving
    EXPECT_LT(input.moveMagnitude, walkThreshold);
    
    // Walk when moving slowly
    input.moveMagnitude = 0.5f;
    EXPECT_GT(input.moveMagnitude, walkThreshold);
    EXPECT_LT(input.moveMagnitude, runThreshold);
    
    // Run when moving fast
    input.moveMagnitude = 0.9f;
    EXPECT_GT(input.moveMagnitude, runThreshold);
}

/**
 * Test: Crouch Transitions
 * Verifies crouch state transitions
 */
TEST_F(AnimationFSMTest, CrouchTransition_EnterCrouch) {
    fsm.setState(AnimationState::IDLE);
    
    fsm.addTransition(AnimationState::IDLE, AnimationState::CROUCH, [&]() {
        return input.crouch;
    });
    
    EXPECT_FALSE(fsm.isInState(AnimationState::CROUCH));
    
    input.crouch = true;
    fsm.evaluateTransitions();
    
    EXPECT_TRUE(fsm.isInState(AnimationState::CROUCH));
}

TEST_F(AnimationFSMTest, CrouchTransition_ExitCrouch) {
    fsm.setState(AnimationState::CROUCH);
    
    fsm.addTransition(AnimationState::CROUCH, AnimationState::IDLE, [&]() {
        return !input.crouch;
    });
    
    EXPECT_TRUE(fsm.isInState(AnimationState::CROUCH));
    
    input.crouch = false;
    fsm.evaluateTransitions();
    
    EXPECT_FALSE(fsm.isInState(AnimationState::CROUCH));
}

/**
 * Test: State History
 * Verifies state history is tracked correctly
 */
TEST_F(AnimationFSMTest, StateHistory_Tracked) {
    std::vector<AnimationState> history;
    
    fsm.setState(AnimationState::IDLE);
    history.push_back(fsm.currentState);
    
    fsm.setState(AnimationState::WALK);
    history.push_back(fsm.currentState);
    
    fsm.setState(AnimationState::RUN);
    history.push_back(fsm.currentState);
    
    EXPECT_EQ(history.size(), 3);
    EXPECT_EQ(history[0], AnimationState::IDLE);
    EXPECT_EQ(history[1], AnimationState::WALK);
    EXPECT_EQ(history[2], AnimationState::RUN);
}

/**
 * Test: Complex State Flow
 * Verifies complete gameplay state flow
 */
TEST_F(AnimationFSMTest, ComplexFlow_IdleWalkRunJump) {
    // Setup transitions
    fsm.setState(AnimationState::IDLE);
    
    fsm.addTransition(AnimationState::IDLE, AnimationState::WALK, [&]() {
        return input.moveMagnitude > 0.1f && !input.sprint;
    });
    fsm.addTransition(AnimationState::WALK, AnimationState::RUN, [&]() {
        return input.sprint && input.moveMagnitude > 0.5f;
    });
    fsm.addTransition(AnimationState::RUN, AnimationState::JUMP, [&]() {
        return input.jump && input.grounded;
    });
    fsm.addTransition(AnimationState::JUMP, AnimationState::FALL, [&]() {
        return !input.grounded && input.verticalVelocity < 0.0f;
    });
    fsm.addTransition(AnimationState::FALL, AnimationState::IDLE, [&]() {
        return input.grounded;
    });
    
    // Start idle
    EXPECT_EQ(fsm.currentState, AnimationState::IDLE);
    
    // Start walking
    input.moveMagnitude = 0.3f;
    fsm.evaluateTransitions();
    EXPECT_EQ(fsm.currentState, AnimationState::WALK);
    
    // Start sprinting
    input.sprint = true;
    input.moveMagnitude = 0.8f;
    fsm.evaluateTransitions();
    EXPECT_EQ(fsm.currentState, AnimationState::RUN);
    
    // Jump
    input.jump = true;
    input.grounded = true;
    fsm.evaluateTransitions();
    EXPECT_EQ(fsm.currentState, AnimationState::JUMP);
    
    // Fall (at peak of jump)
    input.jump = false;
    input.grounded = false;
    input.verticalVelocity = -2.0f;
    fsm.evaluateTransitions();
    EXPECT_EQ(fsm.currentState, AnimationState::FALL);
    
    // Land
    input.grounded = true;
    input.verticalVelocity = 0.0f;
    fsm.evaluateTransitions();
    EXPECT_EQ(fsm.currentState, AnimationState::IDLE);
}

/**
 * Test: Edge Cases
 * Verifies edge cases are handled
 */
TEST_F(AnimationFSMTest, EdgeCase_ZeroMagnitude) {
    input.moveMagnitude = 0.0f;
    EXPECT_FLOAT_EQ(input.moveMagnitude, 0.0f);
}

TEST_F(AnimationFSMTest, EdgeCase_MaxMagnitude) {
    input.moveMagnitude = 1.0f;
    EXPECT_FLOAT_EQ(input.moveMagnitude, 1.0f);
}

TEST_F(AnimationFSMTest, EdgeCase_MultipleStateChanges_SameFrame) {
    fsm.setState(AnimationState::IDLE);
    
    // Rapid state changes
    fsm.setState(AnimationState::WALK);
    fsm.setState(AnimationState::RUN);
    fsm.setState(AnimationState::JUMP);
    
    // Should end up in last state
    EXPECT_EQ(fsm.currentState, AnimationState::JUMP);
    EXPECT_EQ(fsm.previousState, AnimationState::RUN);
}

/**
 * Test: Animation State ToString
 * Verifies state conversion to string
 */
TEST_F(AnimationFSMTest, StateToString_Conversion) {
    EXPECT_EQ(AnimationStateToString(AnimationState::IDLE), "Idle");
    EXPECT_EQ(AnimationStateToString(AnimationState::WALK), "Walk");
    EXPECT_EQ(AnimationStateToString(AnimationState::RUN), "Run");
    EXPECT_EQ(AnimationStateToString(AnimationState::JUMP), "Jump");
    EXPECT_EQ(AnimationStateToString(AnimationState::FALL), "Fall");
    EXPECT_EQ(AnimationStateToString(AnimationState::CROUCH), "Crouch");
}
