#pragma once

#include "../ECS.h"
#include <glm/glm.hpp>

namespace ecs {

/**
 * Rigid Body Types
 */
enum class RigidBodyType {
    STATIC,      // Immovable objects (terrain, buildings)
    DYNAMIC,     // Fully simulated objects
    KINEMATIC    // Moved by code, but affects dynamics
};

/**
 * Collider Types
 */
enum class ColliderType {
    NONE,
    SPHERE,
    BOX,
    CAPSULE,
    MESH
};

/**
 * RigidBody Component - Physics simulation
 */
struct RigidBodyComponent : public Component {
    RigidBodyType bodyType = RigidBodyType::DYNAMIC;
    ColliderType colliderType = ColliderType::BOX;
    
    // Physical properties
    float mass = 1.0f;
    float restitution = 0.5f;     // Bounciness
    float friction = 0.5f;
    float linearDamping = 0.01f;
    float angularDamping = 0.05f;
    
    // Collider dimensions
    float sphereRadius = 0.5f;
    glm::vec3 boxSize{1.0f};
    float capsuleRadius = 0.5f;
    float capsuleHeight = 1.0f;
    
    // Physics state
    glm::vec3 linearVelocity{0.0f};
    glm::vec3 angularVelocity{0.0f};
    glm::vec3 force{0.0f};
    glm::vec3 torque{0.0f};
    
    // State flags
    bool isKinematic = false;
    bool isStatic = false;
    bool useGravity = true;
    bool isGrounded = false;
    
    // Inverse properties (calculated)
    float invMass = 1.0f;
    glm::mat3 invInertiaTensor{1.0f};
    
    RigidBodyComponent() = default;
    
    /**
     * Initialize inverse properties
     */
    void initialize() {
        if (bodyType == RigidBodyType::STATIC || bodyType == RigidBodyType::KINEMATIC) {
            invMass = 0.0f;
        } else {
            invMass = (mass > 0.0f) ? 1.0f / mass : 0.0f;
        }
        
        calculateInertiaTensor();
    }
    
    /**
     * Calculate inertia tensor based on collider type
     */
    void calculateInertiaTensor() {
        if (invMass == 0.0f) {
            invInertiaTensor = glm::mat3(0.0f);
            return;
        }
        
        glm::vec3 inertia{0.0f};
        
        switch (colliderType) {
            case ColliderType::SPHERE: {
                float rr = sphereRadius * sphereRadius;
                float factor = 0.4f * mass * rr;
                inertia = glm::vec3(factor);
                break;
            }
            case ColliderType::BOX: {
                float x2 = boxSize.x * boxSize.x;
                float y2 = boxSize.y * boxSize.y;
                float z2 = boxSize.z * boxSize.z;
                inertia.x = (1.0f / 12.0f) * mass * (y2 + z2);
                inertia.y = (1.0f / 12.0f) * mass * (x2 + z2);
                inertia.z = (1.0f / 12.0f) * mass * (x2 + y2);
                break;
            }
            case ColliderType::CAPSULE: {
                float r2 = capsuleRadius * capsuleRadius;
                float h2 = capsuleHeight * capsuleHeight;
                float factor = mass * (0.25f * r2 + h2 / 12.0f);
                inertia.x = factor;
                inertia.y = 0.5f * mass * r2;
                inertia.z = factor;
                break;
            }
            default:
                inertia = glm::vec3(1.0f);
        }
        
        // Calculate inverse inertia tensor (diagonal in local space)
        invInertiaTensor = glm::mat3(0.0f);
        invInertiaTensor[0][0] = (inertia.x > 0.0f) ? 1.0f / inertia.x : 0.0f;
        invInertiaTensor[1][1] = (inertia.y > 0.0f) ? 1.0f / inertia.y : 0.0f;
        invInertiaTensor[2][2] = (inertia.z > 0.0f) ? 1.0f / inertia.z : 0.0f;
    }
    
    /**
     * Apply a force
     */
    void applyForce(const glm::vec3& f) {
        if (bodyType != RigidBodyType::DYNAMIC) return;
        force += f;
    }
    
    /**
     * Apply an impulse (instant velocity change)
     */
    void applyImpulse(const glm::vec3& impulse) {
        if (bodyType != RigidBodyType::DYNAMIC || invMass == 0.0f) return;
        linearVelocity += impulse * invMass;
    }
    
    /**
     * Apply a torque
     */
    void applyTorque(const glm::vec3& t) {
        if (bodyType != RigidBodyType::DYNAMIC) return;
        torque += t;
    }
    
    /**
     * Set velocity
     */
    void setLinearVelocity(const glm::vec3& vel) {
        linearVelocity = vel;
    }
    
    /**
     * Get velocity
     */
    glm::vec3 getLinearVelocity() const {
        return linearVelocity;
    }
    
    /**
     * Check if body is dynamic
     */
    bool isDynamic() const {
        return bodyType == RigidBodyType::DYNAMIC && invMass > 0.0f;
    }
};

/**
 * Character Controller Component - Special kinematic body for player
 */
struct CharacterControllerComponent : public Component {
    float height = 1.8f;
    float radius = 0.4f;
    float stepHeight = 0.5f;
    float slopeLimit = 45.0f;
    
    // Movement
    glm::vec3 velocity{0.0f};
    glm::vec3 inputDirection{0.0f};
    bool isGrounded = false;
    bool canJump = true;
    
    // Jump settings
    float jumpForce = 5.0f;
    float moveSpeed = 5.0f;
    float sprintMultiplier = 1.5f;
    
    CharacterControllerComponent() = default;
};

} // namespace ecs
