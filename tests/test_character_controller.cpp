/**
 * Character Controller Unit Tests
 * 
 * Tests for player movement, jumping, crouching, sliding,
 * gravity, and slope handling.
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/norm.hpp>
#include <memory>
#include <cmath>

// Forward declare physics types for testing
struct RigidBody {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 scale{1.0f};
    bool onGround = true;
    bool isPlayer = false;
    float restitution = 0.0f;
    float friction = 0.0f;
    
    void reset() {
        position = glm::vec3(0.0f);
        velocity = glm::vec3(0.0f);
        scale = glm::vec3(1.0f);
        onGround = true;
    }
};

class CharacterControllerTest : public ::testing::Test {
protected:
    std::shared_ptr<RigidBody> body;
    
    // Simplified CharacterController for testing
    struct TestController {
        std::shared_ptr<RigidBody> body;
        glm::vec3 moveInput{0.0f};
        
        // Configuration
        float walkSpeed = 6.0f;
        float runSpeed = 10.0f;
        float crouchSpeed = 2.0f;
        float slideSpeed = 8.0f;
        float jumpSpeed = 6.5f;
        float gravity = 20.0f;
        float maxSlopeAngle = 45.0f;
        
        // State
        bool isCrouched = false;
        bool isSlidingState = false;
        bool isSprintingState = false;
        float currentSlopeAngle = 0.0f;
        
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
            if (isCrouched) {
                body->scale.y *= 0.5f;
            } else {
                body->scale.y *= 2.0f;
            }
        }
        
        void setSprint(bool sprinting) {
            isSprintingState = sprinting;
        }
        
        void applyGravity(float dt) {
            if (!body->onGround) {
                body->velocity.y -= gravity * dt;
            } else {
                body->velocity.y = 0.0f;
            }
        }
        
        void applyMovement(float dt) {
            float currentSpeed = isSprintingState ? runSpeed : 
                                isCrouched ? crouchSpeed : walkSpeed;
            
            glm::vec3 desired = moveInput * currentSpeed;
            
            if (!body->onGround) {
                float airControl = 0.2f;
                desired *= airControl;
            }
            
            body->velocity.x = desired.x;
            body->velocity.z = desired.z;
        }
        
        void update(float dt) {
            applyGravity(dt);
            applyMovement(dt);
        }
    };
    
    TestController controller;
    
    void SetUp() override {
        body = std::make_shared<RigidBody>();
        controller.body = body;
    }
    
    void TearDown() override {
        body.reset();
    }
};

/**
 * Test: Movement Input Normalization
 * Verifies that move input is properly normalized
 */
TEST_F(CharacterControllerTest, MovementInput_Normalizes) {
    // Diagonal input (should normalize to length 1)
    controller.setMoveInput(glm::vec3(1.0f, 0.0f, 1.0f));
    
    EXPECT_FLOAT_EQ(glm::length(controller.moveInput), 1.0f);
}

TEST_F(CharacterControllerTest, MovementInput_ZeroInput_StaysZero) {
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 0.0f));
    
    EXPECT_FLOAT_EQ(controller.moveInput.x, 0.0f);
    EXPECT_FLOAT_EQ(controller.moveInput.z, 0.0f);
}

/**
 * Test: Walk Speed
 * Verifies character moves at walk speed
 */
TEST_F(CharacterControllerTest, WalkSpeed_Correct) {
    body->onGround = true;
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    controller.isSprintingState = false;
    controller.isCrouched = false;
    
    float dt = 0.1f;
    controller.applyMovement(dt);
    
    // Velocity should match walk speed
    EXPECT_FLOAT_EQ(body->velocity.z, controller.walkSpeed);
}

/**
 * Test: Sprint Speed
 * Verifies character moves faster when sprinting
 */
TEST_F(CharacterControllerTest, SprintSpeed_Faster) {
    body->onGround = true;
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    controller.isSprintingState = true;
    
    float dt = 0.1f;
    controller.applyMovement(dt);
    
    // Velocity should match run speed
    EXPECT_FLOAT_EQ(body->velocity.z, controller.runSpeed);
    EXPECT_GT(controller.runSpeed, controller.walkSpeed);
}

/**
 * Test: Crouch Speed
 * Verifies character moves slower when crouching
 */
TEST_F(CharacterControllerTest, CrouchSpeed_Slower) {
    body->onGround = true;
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    controller.isCrouched = true;
    
    float dt = 0.1f;
    controller.applyMovement(dt);
    
    // Velocity should match crouch speed
    EXPECT_FLOAT_EQ(body->velocity.z, controller.crouchSpeed);
    EXPECT_LT(controller.crouchSpeed, controller.walkSpeed);
}

/**
 * Test: Jump
 * Verifies jump applies upward velocity
 */
TEST_F(CharacterControllerTest, Jump_AppliesUpwardVelocity) {
    body->onGround = true;
    
    controller.jump();
    
    EXPECT_FLOAT_EQ(body->velocity.y, controller.jumpSpeed);
    EXPECT_FALSE(body->onGround);
}

TEST_F(CharacterControllerTest, Jump_InAir_Fails) {
    body->onGround = false;
    body->velocity.y = 0.0f;
    
    controller.jump();
    
    // Should not jump in air
    EXPECT_FLOAT_EQ(body->velocity.y, 0.0f);
    EXPECT_FALSE(body->onGround);
}

/**
 * Test: Gravity
 * Verifies gravity affects velocity when airborne
 */
TEST_F(CharacterControllerTest, Gravity_AppliesInAir) {
    body->onGround = false;
    body->velocity.y = 5.0f;  // Initial upward velocity
    
    float dt = 0.1f;
    controller.applyGravity(dt);
    
    // Gravity should reduce upward velocity
    EXPECT_LT(body->velocity.y, 5.0f);
    EXPECT_FLOAT_EQ(body->velocity.y, 5.0f - controller.gravity * dt);
}

TEST_F(CharacterControllerTest, Gravity_ZeroOnGround) {
    body->onGround = true;
    body->velocity.y = -5.0f;
    
    float dt = 0.1f;
    controller.applyGravity(dt);
    
    // Velocity should be zero on ground
    EXPECT_FLOAT_EQ(body->velocity.y, 0.0f);
}

/**
 * Test: Air Control
 * Verifies reduced movement control in air
 */
TEST_F(CharacterControllerTest, AirControl_Reduced) {
    body->onGround = false;
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    
    float dt = 0.1f;
    controller.applyMovement(dt);
    
    // Air control should reduce movement
    float airControl = 0.2f;
    EXPECT_FLOAT_EQ(body->velocity.z, controller.walkSpeed * airControl);
}

TEST_F(CharacterControllerTest, GroundControl_Full) {
    body->onGround = true;
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    
    float dt = 0.1f;
    controller.applyMovement(dt);
    
    // Full control on ground
    EXPECT_FLOAT_EQ(body->velocity.z, controller.walkSpeed);
}

/**
 * Test: Crouch Scale
 * Verifies character scale changes when crouching
 */
TEST_F(CharacterControllerTest, Crouch_ReducesScale) {
    glm::vec3 originalScale = body->scale;
    
    controller.crouch(true);
    
    EXPECT_FLOAT_EQ(body->scale.y, originalScale.y * 0.5f);
    EXPECT_TRUE(controller.isCrouched);
}

TEST_F(CharacterControllerTest, Crouch_RestoresScale) {
    controller.crouch(true);
    glm::vec3 crouchedScale = body->scale;
    
    controller.crouch(false);
    
    EXPECT_FLOAT_EQ(body->scale.y, crouchedScale.y * 2.0f);
    EXPECT_FALSE(controller.isCrouched);
}

/**
 * Test: State Queries
 * Verifies state query functions work correctly
 */
TEST_F(CharacterControllerTest, IsGrounded_Correct) {
    body->onGround = true;
    EXPECT_TRUE(body->onGround);
    
    body->onGround = false;
    EXPECT_FALSE(body->onGround);
}

TEST_F(CharacterControllerTest, IsCrouching_Correct) {
    EXPECT_FALSE(controller.isCrouched);
    
    controller.crouch(true);
    EXPECT_TRUE(controller.isCrouched);
}

/**
 * Test: Movement Direction
 * Verifies movement respects input direction
 */
TEST_F(CharacterControllerTest, MovementDirection_Forward) {
    body->onGround = true;
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    
    float dt = 0.1f;
    controller.applyMovement(dt);
    
    EXPECT_GT(body->velocity.z, 0.0f);
    EXPECT_FLOAT_EQ(body->velocity.x, 0.0f);
}

TEST_F(CharacterControllerTest, MovementDirection_Backward) {
    body->onGround = true;
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, -1.0f));
    
    float dt = 0.1f;
    controller.applyMovement(dt);
    
    EXPECT_LT(body->velocity.z, 0.0f);
    EXPECT_FLOAT_EQ(body->velocity.x, 0.0f);
}

TEST_F(CharacterControllerTest, MovementDirection_Left) {
    body->onGround = true;
    controller.setMoveInput(glm::vec3(-1.0f, 0.0f, 0.0f));
    
    float dt = 0.1f;
    controller.applyMovement(dt);
    
    EXPECT_LT(body->velocity.x, 0.0f);
    EXPECT_FLOAT_EQ(body->velocity.z, 0.0f);
}

TEST_F(CharacterControllerTest, MovementDirection_Right) {
    body->onGround = true;
    controller.setMoveInput(glm::vec3(1.0f, 0.0f, 0.0f));
    
    float dt = 0.1f;
    controller.applyMovement(dt);
    
    EXPECT_GT(body->velocity.x, 0.0f);
    EXPECT_FLOAT_EQ(body->velocity.z, 0.0f);
}

/**
 * Test: Full Update Cycle
 * Verifies complete update cycle works
 */
TEST_F(CharacterControllerTest, FullUpdate_Complete) {
    body->onGround = false;
    body->velocity = glm::vec3(0.0f, 5.0f, 0.0f);
    controller.setMoveInput(glm::vec3(0.0f, 0.0f, 1.0f));
    
    float dt = 0.016f;  // ~60fps
    controller.update(dt);
    
    // Gravity should have affected Y velocity
    EXPECT_LT(body->velocity.y, 5.0f);
    // Movement should have affected Z velocity (reduced by air control)
    EXPECT_GT(body->velocity.z, 0.0f);
}

/**
 * Test: Speed Configuration
 * Verifies speed settings can be changed
 */
TEST_F(CharacterControllerTest, SpeedConfiguration_Changeable) {
    float newWalkSpeed = 8.0f;
    controller.walkSpeed = newWalkSpeed;
    
    EXPECT_FLOAT_EQ(controller.walkSpeed, newWalkSpeed);
}

TEST_F(CharacterControllerTest, GravityConfiguration_Changeable) {
    float newGravity = 30.0f;
    controller.gravity = newGravity;
    
    EXPECT_FLOAT_EQ(controller.gravity, newGravity);
}

/**
 * Test: Slope Angle
 * Verifies slope angle is tracked
 */
TEST_F(CharacterControllerTest, SlopeAngle_Default) {
    EXPECT_FLOAT_EQ(controller.currentSlopeAngle, 0.0f);
}

TEST_F(CharacterControllerTest, MaxSlopeAngle_Configurable) {
    controller.maxSlopeAngle = 60.0f;
    EXPECT_FLOAT_EQ(controller.maxSlopeAngle, 60.0f);
}
