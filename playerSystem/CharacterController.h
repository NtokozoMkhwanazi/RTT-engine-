#pragma once
#include "physicsSystem/RigidBody.h"
#include "physicsSystem/Physics.h"
#include <glm/glm.hpp>
#include <memory>

class CharacterController {
public:
    CharacterController(RigidBody* body, PhysicsWorld* world);

    // --- Input ---
    void setMoveInput(const glm::vec3& dir); // XZ direction
    void jump();
    void crouch(bool crouched);
    void slide();
    void setSprint(bool sprinting);

    // --- Update ---
    // 🔥 ROOT MOTION AWARE UPDATE
    void update(float dt, const glm::vec3& rootMotion);
    void applyRootMotion(RigidBody* playerBody, const glm::vec3& delta, float dt);
    
    // --- State queries ---
    bool isGrounded() const { return body && body->onGround; }
    bool isCrouching() const { return isCrouched; }
    bool isCurrentlySliding() const { return isSlidingState; }
    bool isCurrentlySprinting() const { return isSprintingState; }
    float getSlopeAngle() const { return currentSlopeAngle; }
    
    // --- Configuration ---
    void setWalkSpeed(float speed) { walkSpeed = speed; }
    void setRunSpeed(float speed) { runSpeed = speed; }
    void setCrouchSpeed(float speed) { crouchSpeed = speed; }
    void setJumpSpeed(float speed) { jumpSpeed = speed; }
    void setGravity(float g) { gravity = g; }
    void setMaxSlopeAngle(float angle) { maxSlopeAngle = angle; }
    void setSlideSpeed(float speed) { slideSpeed = speed; }

private:
    RigidBody* body = nullptr;
    PhysicsWorld* world = nullptr;

    // Movement tuning
    float walkSpeed = 6.0f;
    float runSpeed = 10.0f;
    float crouchSpeed = 2.0f;
    float slideSpeed = 8.0f;
    float jumpSpeed = 6.5f;
    float gravity = 20.0f;

    // Input
    glm::vec3 moveInput {0.0f};
    float moveMagnitude {0.0f};  // Raw stick deflection magnitude (0..1)
    float accelerationRate {12.0f};  // Velocity ramp-up smoothing rate

    // State
    bool isCrouched = false;
    bool isSlidingState = false;
    bool isSprintingState = false;
    float currentSlopeAngle = 0.0f;

    // Slope
    float maxSlopeAngle = 45.0f;

    void applyMovement(float dt);
    void applyGravity(float dt);
    bool checkSlopeTraversal(float dt);
    void handleSlope(float dt);
};


