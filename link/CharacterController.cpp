#include "CharacterController.h"
#include <glm/gtx/norm.hpp>
#include <glm/gtx/vector_angle.hpp>

// ---------------- Constructor ----------------

CharacterController::CharacterController(
    const std::shared_ptr<RigidBody>& b,
    PhysicsWorld* w)
    : body(b), world(w)
{
    body->isPlayer = true;
    body->restitution = 0.0f;
    body->friction = 0.9f;
}

// ---------------- Input ----------------

void CharacterController::setMoveInput(const glm::vec3& dir) {
    moveInput = dir;
    moveInput.y = 0.0f;

    if (glm::length2(moveInput) > 1e-6f)
        moveInput = glm::normalize(moveInput);
}

void CharacterController::jump() {
    if (!body->onGround) return;

    body->velocity.y = jumpSpeed;
    body->onGround = false;
}

// ---------------- Update ----------------

void CharacterController::update(float dt, const glm::vec3& rootMotion)
{
    // Root motion drives the body
    body->position += glm::vec3(rootMotion.x, 0.0f, rootMotion.z);

//    applyGravity(dt);
   // applyMovement(dt);
}




// ---------------- Gravity ----------------

void CharacterController::applyGravity(float dt)
{
    if (!body->onGround) {
        body->velocity.y -= gravity * dt;
    } else {
        body->velocity.y = 0.0f;
    }
}


// ---------------- Movement ----------------

void CharacterController::applyMovement(float /*dt*/) {
    glm::vec3 desired = moveInput * walkSpeed;

    if (body->onGround) {
        // Prevent climbing steep slopes
        glm::vec3 up(0,1,0);
        float slopeAngle = glm::degrees(glm::angle(up, up));

        if (slopeAngle <= maxSlopeAngle) {
            // Movement projected onto ground plane (simple slope support)
            desired = desired - up * glm::dot(desired, up);
        }
    }

    body->velocity.x = desired.x;
    body->velocity.z = desired.z;
}

