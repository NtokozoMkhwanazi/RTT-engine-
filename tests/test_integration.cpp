/**
 * Integration Tests: Animation FSM + Character Controller
 * 
 * Tests that verify the Animation State Machine and Character Controller
 * work together correctly. These tests simulate real gameplay scenarios
 * where input drives both movement and animation state.
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/norm.hpp>
#include <memory>
#include <functional>
#include <string>
#include <vector>

// ============================================================================
// Mock Animation System (simplified for integration testing)
// ============================================================================

enum class AnimationState {
    IDLE, WALK, RUN, JUMP, FALL, CROUCH, CROUCH_WALK, NONE
};

inline std::string AnimationStateToString(AnimationState state) {
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

struct MockAnimation {
    std::string name;
    bool isPlaying = false;
};

class MockAnimator {
public:
    MockAnimation* currentAnimation = nullptr;
    
    void Play(MockAnimation* anim) {
        currentAnimation = anim;
        if (anim) anim->isPlaying = true;
    }
};

// Simplified Animation FSM for integration testing
class IntegrationAnimationFSM {
public:
    AnimationState currentState = AnimationState::NONE;
    AnimationState previousState = AnimationState::NONE;
    MockAnimator* animator = nullptr;
    
    struct AnimationData {
        MockAnimation* anim;
        bool loop;
    };
    
    std::map<AnimationState, AnimationData> animations;
    
    void setAnimator(MockAnimator* a) { animator = a; }
    
    void registerAnimation(AnimationState state, MockAnimation* anim, bool loop = true) {
        animations[state] = {anim, loop};
    }
    
    void setState(AnimationState state) {
        if (currentState == state) return;
        previousState = currentState;
        currentState = state;
        
        // Play animation for new state
        auto it = animations.find(state);
        if (it != animations.end() && animator && it->second.anim) {
            animator->Play(it->second.anim);
        }
    }
    
    bool isInState(AnimationState state) const {
        return currentState == state;
    }
};

// ============================================================================
// Mock Physics System (simplified for integration testing)
// ============================================================================

struct RigidBody {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 scale{1.0f};
    bool onGround = true;
    float restitution = 0.0f;
    float friction = 0.0f;
    bool isPlayer = false;
};

// ============================================================================
// Character Controller (integration version)
// ============================================================================

class IntegrationCharacterController {
public:
    std::shared_ptr<RigidBody> body;
    glm::vec3 moveInput{0.0f};
    
    // Configuration
    float walkSpeed = 6.0f;
    float runSpeed = 10.0f;
    float crouchSpeed = 2.0f;
    float jumpSpeed = 6.5f;
    float gravity = 20.0f;
    
    // State
    bool isCrouched = false;
    bool isSprinting = false;
    
    void setMoveInput(const glm::vec3& dir) {
        moveInput = dir;
        if (glm::length2(moveInput) > 1e-6f)
            moveInput = glm::normalize(moveInput);
    }
    
    void jump() {
        if (!body->onGround) return;
        body->velocity.y = jumpSpeed;
        body->onGround = false;
    }
    
    void crouch(bool crouched) {
        isCrouched = crouched;
    }
    
    void setSprint(bool sprinting) {
        isSprinting = sprinting;
    }
    
    void update(float dt) {
        // Apply gravity
        if (!body->onGround) {
            body->velocity.y -= gravity * dt;
        } else {
            body->velocity.y = 0.0f;
        }
        
        // Apply movement
        float currentSpeed = isSprinting ? runSpeed : 
                            isCrouched ? crouchSpeed : walkSpeed;
        
        glm::vec3 desired = moveInput * currentSpeed;
        
        if (!body->onGround) {
            float airControl = 0.2f;
            desired *= airControl;
        }
        
        body->velocity.x = desired.x;
        body->velocity.z = desired.z;
        
        // Update position
        body->position += body->velocity * dt;
    }
    
    float getSpeed() const {
        return glm::length(glm::vec2(body->velocity.x, body->velocity.z));
    }
    
    bool isMoving() const {
        return glm::length2(moveInput) > 0.1f;
    }
};

// ============================================================================
// Character Input (shared between systems)
// ============================================================================

struct GameInput {
    glm::vec2 moveDirection{0.0f, 0.0f};
    float moveMagnitude = 0.0f;
    bool jump = false;
    bool jumpPressed = false;  // Edge-triggered
    bool crouch = false;
    bool sprint = false;
    bool grounded = true;
    float verticalVelocity = 0.0f;
    
    void updateFromController(const IntegrationCharacterController& controller) {
        moveDirection = glm::vec2(controller.moveInput.x, controller.moveInput.z);
        moveMagnitude = glm::length(moveDirection);
        grounded = controller.body->onGround;
        verticalVelocity = controller.body->velocity.y;
    }
};

// ============================================================================
// Integration Test Fixture
// ============================================================================

class IntegrationTest : public ::testing::Test {
protected:
    std::shared_ptr<RigidBody> body;
    IntegrationCharacterController controller;
    IntegrationAnimationFSM fsm;
    MockAnimator animator;
    GameInput input;
    
    // Mock animations
    MockAnimation idleAnim{"Idle", false};
    MockAnimation walkAnim{"Walk", false};
    MockAnimation runAnim{"Run", false};
    MockAnimation jumpAnim{"Jump", false};
    MockAnimation fallAnim{"Fall", false};
    MockAnimation crouchAnim{"Crouch", false};
    
    void SetUp() override {
        body = std::make_shared<RigidBody>();
        controller.body = body;
        
        // Setup FSM with animator
        fsm.setAnimator(&animator);
        
        // Register all animations
        fsm.registerAnimation(AnimationState::IDLE, &idleAnim);
        fsm.registerAnimation(AnimationState::WALK, &walkAnim);
        fsm.registerAnimation(AnimationState::RUN, &runAnim);
        fsm.registerAnimation(AnimationState::JUMP, &jumpAnim);
        fsm.registerAnimation(AnimationState::FALL, &fallAnim);
        fsm.registerAnimation(AnimationState::CROUCH, &crouchAnim);
        
        // Initialize to IDLE
        fsm.setState(AnimationState::IDLE);
        
        // Reset input
        input = GameInput();
    }
    
    void TearDown() override {
        body.reset();
    }
    
    void updateSystem(float dt) {
        // Update controller physics
        controller.update(dt);
        
        // Sync input from controller state
        input.updateFromController(controller);
        input.sprint = controller.isSprinting;
        input.crouch = controller.isCrouched;
        
        // Update FSM based on input
        updateFSM();
    }
    
    void updateFSM() {
        // Determine desired state based on input
        if (!input.grounded && input.verticalVelocity < 0.0f) {
            fsm.setState(AnimationState::FALL);
        } else if (!input.grounded && input.verticalVelocity > 0.0f) {
            fsm.setState(AnimationState::JUMP);
        } else if (input.crouch) {
            fsm.setState(AnimationState::CROUCH);
        } else if (input.sprint && input.moveMagnitude > 0.1f) {
            fsm.setState(AnimationState::RUN);
        } else if (input.moveMagnitude > 0.1f) {
            fsm.setState(AnimationState::WALK);
        } else {
            fsm.setState(AnimationState::IDLE);
        }
    }
    
    void pressJump() {
        input.jumpPressed = true;
        input.jump = true;
    }
    
    void releaseJump() {
        input.jumpPressed = false;
        input.jump = false;
    }
};

// ============================================================================
// Integration Tests
// ============================================================================

/**
 * Test: Idle State
 * Verifies character is idle when no input
 */
TEST_F(IntegrationTest, Idle_NoInput) {
    float dt = 0.016f;
    
    // No input
    updateSystem(dt);
    
    EXPECT_TRUE(fsm.isInState(AnimationState::IDLE));
    EXPECT_FLOAT_EQ(controller.getSpeed(), 0.0f);
    EXPECT_TRUE(body->onGround);
}

/**
 * Test: Walk Animation Triggers on Movement
 * Verifies walking starts when character moves
 */
TEST_F(IntegrationTest, Walk_AnimationTriggers) {
    float dt = 0.016f;
    
    // Start walking
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    updateSystem(dt);
    
    EXPECT_TRUE(fsm.isInState(AnimationState::WALK));
    EXPECT_GT(controller.getSpeed(), 0.0f);
    EXPECT_FLOAT_EQ(controller.getSpeed(), controller.walkSpeed);
}

/**
 * Test: Sprint Triggers Run Animation
 * Verifies running starts when sprinting
 */
TEST_F(IntegrationTest, Run_SprintTriggers) {
    float dt = 0.016f;
    
    // Start sprinting
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    controller.setSprint(true);
    updateSystem(dt);
    
    EXPECT_TRUE(fsm.isInState(AnimationState::RUN));
    EXPECT_GT(controller.getSpeed(), controller.walkSpeed);
    EXPECT_FLOAT_EQ(controller.getSpeed(), controller.runSpeed);
}

/**
 * Test: Jump Sequence
 * Verifies complete jump animation sequence
 */
TEST_F(IntegrationTest, Jump_CompleteSequence) {
    float dt = 0.016f;
    
    // Start: Idle
    EXPECT_TRUE(fsm.isInState(AnimationState::IDLE));
    
    // Press jump
    controller.jump();
    updateSystem(dt);
    
    // Should be in jump state
    EXPECT_TRUE(fsm.isInState(AnimationState::JUMP));
    EXPECT_GT(body->velocity.y, 0.0f);
    EXPECT_FALSE(body->onGround);
    
    // At peak of jump (velocity becomes negative)
    body->velocity.y = -1.0f;
    updateSystem(dt);
    
    // Should transition to fall
    EXPECT_TRUE(fsm.isInState(AnimationState::FALL));
    
    // Land
    body->onGround = true;
    body->velocity.y = 0.0f;
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 0.0f));
    updateSystem(dt);
    
    // Back to idle
    EXPECT_TRUE(fsm.isInState(AnimationState::IDLE));
}

/**
 * Test: Walk to Run Transition
 * Verifies smooth transition from walk to run
 */
TEST_F(IntegrationTest, WalkToRun_Transition) {
    float dt = 0.016f;
    
    // Start walking
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    updateSystem(dt);
    EXPECT_TRUE(fsm.isInState(AnimationState::WALK));
    
    // Start sprinting
    controller.setSprint(true);
    updateSystem(dt);
    
    EXPECT_TRUE(fsm.isInState(AnimationState::RUN));
    EXPECT_EQ(fsm.previousState, AnimationState::WALK);
}

/**
 * Test: Crouch While Moving
 * Verifies crouch state while character is moving
 */
TEST_F(IntegrationTest, CrouchWhileMoving) {
    float dt = 0.016f;
    
    // Start walking
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    updateSystem(dt);
    EXPECT_TRUE(fsm.isInState(AnimationState::WALK));
    
    // Crouch while moving
    controller.crouch(true);
    updateSystem(dt);
    
    EXPECT_TRUE(fsm.isInState(AnimationState::CROUCH));
    EXPECT_FLOAT_EQ(controller.getSpeed(), controller.crouchSpeed);
}

/**
 * Test: Air Control
 * Verifies reduced movement control in air
 */
TEST_F(IntegrationTest, AirControl_ReducedMovement) {
    float dt = 0.016f;
    
    // Jump into air
    controller.jump();
    updateSystem(dt);
    
    EXPECT_FALSE(body->onGround);
    
    // Try to move in air
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    updateSystem(dt);
    
    // Air control should reduce movement
    float airSpeed = controller.getSpeed();
    float groundSpeed = controller.walkSpeed;
    
    EXPECT_LT(airSpeed, groundSpeed);
}

/**
 * Test: Jump Cancel on Crouch
 * Verifies crouching affects animation state
 */
TEST_F(IntegrationTest, JumpToCrouch_Transition) {
    float dt = 0.016f;
    
    // Jump
    controller.jump();
    updateSystem(dt);
    EXPECT_TRUE(fsm.isInState(AnimationState::JUMP));
    
    // Land
    body->onGround = true;
    body->velocity.y = 0.0f;
    
    // Immediately crouch
    controller.crouch(true);
    updateSystem(dt);
    
    EXPECT_TRUE(fsm.isInState(AnimationState::CROUCH));
}

/**
 * Test: Directional Movement
 * Verifies animation matches movement direction
 */
TEST_F(IntegrationTest, DirectionalMovement_Forward) {
    float dt = 0.016f;
    
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    updateSystem(dt);
    
    EXPECT_TRUE(fsm.isInState(AnimationState::WALK));
    EXPECT_GT(body->velocity.z, 0.0f);
}

TEST_F(IntegrationTest, DirectionalMovement_Backward) {
    float dt = 0.016f;
    
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, -1.0f));
    updateSystem(dt);
    
    EXPECT_TRUE(fsm.isInState(AnimationState::WALK));
    EXPECT_LT(body->velocity.z, 0.0f);
}

/**
 * Test: Stop Moving Returns to Idle
 * Verifies character returns to idle when stopping
 */
TEST_F(IntegrationTest, StopMoving_ReturnsToIdle) {
    float dt = 0.016f;
    
    // Walk
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    updateSystem(dt);
    EXPECT_TRUE(fsm.isInState(AnimationState::WALK));
    
    // Stop
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 0.0f));
    updateSystem(dt);
    
    EXPECT_TRUE(fsm.isInState(AnimationState::IDLE));
    EXPECT_FLOAT_EQ(controller.getSpeed(), 0.0f);
}

/**
 * Test: Sprint Toggle
 * Verifies sprint on/off transitions
 */
TEST_F(IntegrationTest, SprintToggle_OnOff) {
    float dt = 0.016f;
    
    // Start moving
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    
    // Sprint on
    controller.setSprint(true);
    updateSystem(dt);
    EXPECT_TRUE(fsm.isInState(AnimationState::RUN));
    
    // Sprint off
    controller.setSprint(false);
    updateSystem(dt);
    EXPECT_TRUE(fsm.isInState(AnimationState::WALK));
}

/**
 * Test: Fall After Jump Peak
 * Verifies automatic transition to fall state
 */
TEST_F(IntegrationTest, Fall_AfterJumpPeak) {
    float dt = 0.016f;
    
    // Jump
    controller.jump();
    updateSystem(dt);
    EXPECT_TRUE(fsm.isInState(AnimationState::JUMP));
    
    // Simulate ascending
    body->velocity.y = 2.0f;
    updateSystem(dt);
    EXPECT_TRUE(fsm.isInState(AnimationState::JUMP));
    
    // At peak (velocity ~0)
    body->velocity.y = 0.0f;
    updateSystem(dt);
    
    // Start falling
    body->velocity.y = -2.0f;
    updateSystem(dt);
    EXPECT_TRUE(fsm.isInState(AnimationState::FALL));
}

/**
 * Test: Multiple Jump Attempts in Air
 * Verifies jump is blocked when airborne
 */
TEST_F(IntegrationTest, JumpBlocked_InAir) {
    float dt = 0.016f;
    
    // Jump
    controller.jump();
    float firstJumpVel = body->velocity.y;
    updateSystem(dt);
    
    // Try to jump again in air
    controller.jump();
    updateSystem(dt);
    
    // Velocity should not reset to jump speed
    EXPECT_LT(body->velocity.y, firstJumpVel);
    EXPECT_FALSE(body->onGround);
}

/**
 * Test: Animation Plays on State Change
 * Verifies animator receives play commands
 */
TEST_F(IntegrationTest, AnimationPlays_OnStateChange) {
    float dt = 0.016f;
    
    MockAnimation* initialAnim = animator.currentAnimation;
    
    // Walk
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    updateSystem(dt);
    
    // Animation should have changed
    EXPECT_NE(animator.currentAnimation, initialAnim);
    EXPECT_TRUE(animator.currentAnimation->isPlaying);
}

/**
 * Test: Complex Gameplay Sequence
 * Verifies complete gameplay flow
 */
TEST_F(IntegrationTest, ComplexSequence_WalkRunJumpFallLand) {
    float dt = 0.016f;
    std::vector<std::string> stateHistory;
    
    // 1. Start idle
    stateHistory.push_back(AnimationStateToString(fsm.currentState));
    EXPECT_TRUE(fsm.isInState(AnimationState::IDLE));
    
    // 2. Walk
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    updateSystem(dt);
    stateHistory.push_back(AnimationStateToString(fsm.currentState));
    EXPECT_TRUE(fsm.isInState(AnimationState::WALK));
    
    // 3. Run (sprint)
    controller.setSprint(true);
    updateSystem(dt);
    stateHistory.push_back(AnimationStateToString(fsm.currentState));
    EXPECT_TRUE(fsm.isInState(AnimationState::RUN));
    
    // 4. Jump
    controller.jump();
    updateSystem(dt);
    stateHistory.push_back(AnimationStateToString(fsm.currentState));
    EXPECT_TRUE(fsm.isInState(AnimationState::JUMP));
    
    // 5. Fall
    body->velocity.y = -5.0f;
    updateSystem(dt);
    stateHistory.push_back(AnimationStateToString(fsm.currentState));
    EXPECT_TRUE(fsm.isInState(AnimationState::FALL));
    
    // 6. Land
    body->onGround = true;
    body->velocity.y = 0.0f;
    controller.setSprint(false);
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 0.0f));
    updateSystem(dt);
    stateHistory.push_back(AnimationStateToString(fsm.currentState));
    EXPECT_TRUE(fsm.isInState(AnimationState::IDLE));
    
    // Verify state history
    EXPECT_EQ(stateHistory.size(), 6);
    EXPECT_EQ(stateHistory[0], "Idle");
    EXPECT_EQ(stateHistory[1], "Walk");
    EXPECT_EQ(stateHistory[2], "Run");
    EXPECT_EQ(stateHistory[3], "Jump");
    EXPECT_EQ(stateHistory[4], "Fall");
    EXPECT_EQ(stateHistory[5], "Idle");
}

/**
 * Test: Speed Consistency
 * Verifies animation speed matches movement speed
 */
TEST_F(IntegrationTest, SpeedConsistency_Walk) {
    float dt = 0.016f;
    
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    updateSystem(dt);
    
    EXPECT_FLOAT_EQ(controller.getSpeed(), controller.walkSpeed);
}

TEST_F(IntegrationTest, SpeedConsistency_Run) {
    float dt = 0.016f;
    
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    controller.setSprint(true);
    updateSystem(dt);
    
    EXPECT_FLOAT_EQ(controller.getSpeed(), controller.runSpeed);
}

/**
 * Test: Grounded State Sync
 * Verifies FSM receives correct grounded state
 */
TEST_F(IntegrationTest, GroundedState_Sync) {
    float dt = 0.016f;
    
    // On ground
    EXPECT_TRUE(body->onGround);
    EXPECT_TRUE(input.grounded);
    
    // Jump
    controller.jump();
    updateSystem(dt);
    
    EXPECT_FALSE(body->onGround);
    EXPECT_FALSE(input.grounded);
}
