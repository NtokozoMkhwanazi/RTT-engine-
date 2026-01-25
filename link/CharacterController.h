#pragma once
#include "RigidBody.h"
#include "Physics.h"
#include <glm/glm.hpp>
#include <memory>

class CharacterController {
public:
    CharacterController(const std::shared_ptr<RigidBody>& body,
                        PhysicsWorld* world);

    // --- Input ---
    void setMoveInput(const glm::vec3& dir); // XZ direction
    void jump();

    // --- Update ---
    // 🔥 ROOT MOTION AWARE UPDATE
    void update(float dt, const glm::vec3& rootMotion);

    bool isGrounded() const { return body->onGround; }

private:
    std::shared_ptr<RigidBody> body;
    PhysicsWorld* world = nullptr;

    // Movement tuning
    float walkSpeed = 6.0f;
    float jumpSpeed = 6.5f;
    float gravity   = 20.0f;

    // Input
    glm::vec3 moveInput {0.0f};

    // Slope
    float maxSlopeAngle = 45.0f;

    void applyMovement(float dt);
    void applyGravity(float dt);
};


