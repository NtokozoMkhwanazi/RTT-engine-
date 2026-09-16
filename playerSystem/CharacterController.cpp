#include "CharacterController.h"
#include <glm/gtx/norm.hpp>
#include <glm/gtx/vector_angle.hpp>
#include <glm/gtc/constants.hpp>

// ---------------- Constructor ----------------

CharacterController::CharacterController(
    RigidBody* b,
    PhysicsWorld* w)
    : body(b), world(w)
{
    if (body) {
        body->isPlayer = true;
        body->restitution = 0.0f;
        body->friction = 0.9f;
    }
}

// ---------------- Input ----------------

void CharacterController::setMoveInput(const glm::vec3& dir) {
    moveInput = dir;
    moveInput.y = 0.0f;

    // FIX (v12 Section 3): Preserve raw stick-deflection magnitude before
    // normalising. Without this, WASD-taps and partial stick deflection
    // both snap to full speed, causing the MotionMatcher query to see a
    // high velocity and select Run/Sprint poses instead of Walk.
    moveMagnitude = glm::length(dir);

    if (glm::length2(moveInput) > 1e-6f)
        moveInput = glm::normalize(moveInput);
}

void CharacterController::jump() {
    if (!body || !body->onGround) return;

    body->velocity.y = jumpSpeed;
    body->onGround = false;
}

void CharacterController::crouch(bool crouched) {
    isCrouched = crouched;
    
    // Adjust body scale when crouching
    if (body) {
        if (isCrouched) {
            body->scale.y *= 0.5f; // Make character shorter when crouching
        } else {
            body->scale.y *= 2.0f; // Return to normal height
        }
    }
}

void CharacterController::slide() {
    if (!body) return;
    if (body->onGround && glm::length(moveInput) > 0.1f) {
        isSlidingState = true;
        // Apply a burst of speed in the movement direction
        body->velocity.x = moveInput.x * slideSpeed;
        body->velocity.z = moveInput.z * slideSpeed;
    }
}

void CharacterController::setSprint(bool sprinting) {
    isSprintingState = sprinting;
}

// ---------------- Update ----------------

void CharacterController::update(float dt, const glm::vec3& rootMotion)
{
    if (!body) return;
    // Root motion drives the body
    body->position += glm::vec3(rootMotion.x, 0.0f, rootMotion.z);

    applyGravity(dt);
    applyMovement(dt);
    
    // Handle sliding state
    if (isSlidingState) {
        // Gradually reduce sliding speed
        float slideReduction = 5.0f * dt; // Rate of slide deceleration
        float currentSpeed = glm::length(glm::vec2(body->velocity.x, body->velocity.z));
        
        if (currentSpeed > 0.0f) {
            float newSpeed = std::max(0.0f, currentSpeed - slideReduction);
            if (newSpeed <= 0.0f) {
                isSlidingState = false;
            } else {
                // Maintain direction but reduce magnitude
                glm::vec2 direction = glm::normalize(glm::vec2(body->velocity.x, body->velocity.z));
                body->velocity.x = direction.x * newSpeed;
                body->velocity.z = direction.y * newSpeed;
            }
        } else {
            isSlidingState = false;
        }
    }
}

void CharacterController::applyRootMotion(RigidBody* playerBody, const glm::vec3& delta, float dt)
{
    // Simply move the player body
    if(playerBody)
    {
        playerBody->position += delta;

        // Optional: update velocity for physics-based movement
        playerBody->velocity = delta / dt;
    }
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

void CharacterController::applyMovement(float dt) {
    if (!body) return;
    
    // Determine current speed based on state
    float currentSpeed = walkSpeed;
    if (isSprintingState) {
        currentSpeed = runSpeed;
    } else if (isCrouched) {
        currentSpeed = crouchSpeed;
    } else if (isSlidingState) {
        // Sliding handled separately
        return;
    }
    
    glm::vec3 desired = moveInput * currentSpeed;

    // FIX (v12 Section 3): Scale by stick deflection so the capsule physics
    // velocity reflects the player's actual input intensity. This keeps the
    // MotionMatcher query speed proportional to intended movement, so light
    // stick taps select Walk poses (not Run).
    desired *= moveMagnitude;

    if (body->onGround) {
        // Handle slope traversal
        handleSlope(dt);
        
        // Apply movement on the ground plane
        glm::vec3 groundPlaneNormal(0.0f, 1.0f, 0.0f); // Assuming up is Y+
        desired = desired - groundPlaneNormal * glm::dot(desired, groundPlaneNormal);
    } else {
        // Apply air control (reduced movement in air)
        float airControl = 0.2f;
        desired *= airControl;
    }

    // Smooth the velocity ramp-up AFTER slope/air-control adjustments so the
    // capsule accelerates naturally toward the target velocity.
    float velT = accelerationRate * dt;
    body->velocity.x = body->velocity.x + (desired.x - body->velocity.x) * velT;
    body->velocity.z = body->velocity.z + (desired.z - body->velocity.z) * velT;
}

// ---------------- Slope Handling ----------------

bool CharacterController::checkSlopeTraversal(float dt) {
    if (!body || !world) return false;
    
    // Perform a raycast downward to check the slope angle
    glm::vec3 rayOrigin = body->position + glm::vec3(0.0f, body->scale.y * 0.5f + 0.1f, 0.0f);
    glm::vec3 rayDirection = glm::vec3(0.0f, -1.0f, 0.0f);
    float maxDist = body->scale.y * 0.5f + 0.5f; // Allow some tolerance
    
    glm::vec3 hitPoint, hitNormal;
    std::shared_ptr<RigidBody> hitBody;
    
    if (world->raycast(rayOrigin, rayDirection, maxDist, hitPoint, hitNormal, hitBody)) {
        // Calculate the angle between the surface normal and up vector
        glm::vec3 up(0.0f, 1.0f, 0.0f);
        currentSlopeAngle = glm::degrees(acosf(glm::dot(up, hitNormal)));
        
        // Check if the slope is traversable
        return currentSlopeAngle <= maxSlopeAngle;
    }
    
    // If no hit, assume flat ground
    currentSlopeAngle = 0.0f;
    return true;
}

void CharacterController::handleSlope(float dt) {
    if (checkSlopeTraversal(dt)) {
        // On traversable slope - allow movement
        // The movement is already calculated in applyMovement
    } else {
        // On steep slope - prevent upward movement
        // This is handled by not allowing the movement to happen
    }
}

