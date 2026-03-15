#pragma once

#include "../ECS.h"
#include "../components/Components.h"
#include <glm/glm.hpp>
#include <vector>
#include <memory>

// Forward declaration
class PhysicsWorld;

namespace ecs {

/**
 * Physics System - Handles physics simulation for rigid bodies
 */
class PhysicsSystem : public TypedSystem<TransformComponent, RigidBodyComponent> {
public:
    PhysicsSystem() = default;
    
    /**
     * Set external physics world (optional)
     */
    void setPhysicsWorld(PhysicsWorld* physicsWorld) {
        m_physicsWorld = physicsWorld;
    }
    
    /**
     * Enable/disable gravity
     */
    void setGravity(const glm::vec3& gravity) {
        m_gravity = gravity;
    }
    
    /**
     * Get current gravity
     */
    glm::vec3 getGravity() const { return m_gravity; }
    
    void init() override {
        m_filter = SystemFilter::require<TransformComponent, RigidBodyComponent>();
    }
    
    void update(float deltaTime) override {
        // Cap delta time to prevent instability
        float dt = std::min(deltaTime, 0.1f);
        
        // Substep for stability
        const float substepDt = 0.016f;  // 60Hz
        int substeps = static_cast<int>(dt / substepDt) + 1;
        float actualDt = dt / static_cast<float>(substeps);
        
        for (int step = 0; step < substeps; ++step) {
            updatePhysics(actualDt);
        }
    }
    
    /**
     * Update physics simulation
     */
    void updatePhysics(float deltaTime) {
        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID entityID, TransformComponent& transform, 
                              RigidBodyComponent& rigidbody) {
                integrateRigidBody(transform, rigidbody, deltaTime);
            });
    }
    
    /**
     * Integrate a single rigid body
     */
    void integrateRigidBody(TransformComponent& transform, 
                           RigidBodyComponent& rigidbody, 
                           float deltaTime) {
        // Skip static and kinematic bodies
        if (rigidbody.bodyType == RigidBodyType::STATIC) return;
        
        if (rigidbody.bodyType == RigidBodyType::KINEMATIC) {
            // Kinematic bodies are moved directly, just update inverse mass
            rigidbody.invMass = 0.0f;
            return;
        }
        
        // Apply gravity
        if (rigidbody.useGravity && rigidbody.invMass > 0.0f) {
            rigidbody.force += m_gravity * (1.0f / rigidbody.invMass);
        }
        
        // Apply linear damping
        rigidbody.linearVelocity *= std::max(0.0f, 1.0f - rigidbody.linearDamping * deltaTime);
        
        // Apply angular damping
        rigidbody.angularVelocity *= std::max(0.0f, 1.0f - rigidbody.angularDamping * deltaTime);
        
        // Integrate linear velocity
        if (rigidbody.invMass > 0.0f) {
            glm::vec3 acceleration = rigidbody.force * rigidbody.invMass;
            rigidbody.linearVelocity += acceleration * deltaTime;
        }
        
        // Integrate position
        transform.position += rigidbody.linearVelocity * deltaTime;
        
        // Integrate angular velocity (simplified)
        if (rigidbody.invMass > 0.0f) {
            glm::vec3 angularAccel = rigidbody.torque * rigidbody.invMass;
            rigidbody.angularVelocity += angularAccel * deltaTime;
            
            // Apply rotation from angular velocity
            float angle = glm::length(rigidbody.angularVelocity) * deltaTime;
            if (angle > 0.001f) {
                glm::vec3 axis = glm::normalize(rigidbody.angularVelocity);
                transform.rotate(angle, axis);
            }
        }
        
        // Clear forces
        rigidbody.force = glm::vec3(0.0f);
        rigidbody.torque = glm::vec3(0.0f);
        
        // Simple ground check
        if (transform.position.y < 0.0f) {
            transform.position.y = 0.0f;
            rigidbody.isGrounded = true;
            
            // Simple bounce
            if (rigidbody.linearVelocity.y < 0.0f) {
                rigidbody.linearVelocity.y *= -rigidbody.restitution;
            }
        } else {
            rigidbody.isGrounded = false;
        }
    }
    
    /**
     * Apply force to an entity
     */
    void applyForce(Entity entity, const glm::vec3& force) {
        auto* rigidbody = getComponent<RigidBodyComponent>(entity.id);
        if (rigidbody) {
            rigidbody->applyForce(force);
        }
    }
    
    /**
     * Apply impulse to an entity
     */
    void applyImpulse(Entity entity, const glm::vec3& impulse) {
        auto* rigidbody = getComponent<RigidBodyComponent>(entity.id);
        if (rigidbody) {
            rigidbody->applyImpulse(impulse);
        }
    }
    
    /**
     * Set velocity for an entity
     */
    void setVelocity(Entity entity, const glm::vec3& velocity) {
        auto* rigidbody = getComponent<RigidBodyComponent>(entity.id);
        if (rigidbody) {
            rigidbody->setLinearVelocity(velocity);
        }
    }
    
    /**
     * Teleport an entity
     */
    void teleport(Entity entity, const glm::vec3& position) {
        auto* transform = getComponent<TransformComponent>(entity.id);
        if (transform) {
            transform->position = position;
            transform->markDirty();
        }
    }
    
    /**
     * Raycast against physics bodies
     */
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, 
                 float maxDistance, Entity& hitEntity, glm::vec3& hitPoint, 
                 glm::vec3& hitNormal) {
        // Simple raycast implementation
        // In production, this would use a spatial data structure
        
        float closestDist = maxDistance;
        bool hit = false;
        
        forEach(*m_entityManager, *m_componentManager,
            [this, &origin, &direction, &closestDist, &hit, &hitEntity, &hitPoint, &hitNormal]
            (EntityID entityID, TransformComponent& transform, RigidBodyComponent& rigidbody) {
                
                glm::vec3 hitLocal;
                glm::vec3 normalLocal;
                float dist;
                
                if (raycastVsBody(origin, direction, transform, rigidbody, 
                                  dist, hitLocal, normalLocal)) {
                    if (dist < closestDist) {
                        closestDist = dist;
                        hit = true;
                        hitEntity = Entity{entityID};
                        hitPoint = hitLocal;
                        hitNormal = normalLocal;
                    }
                }
            });
        
        return hit;
    }
    
    const char* getName() const override { return "PhysicsSystem"; }

private:
    PhysicsWorld* m_physicsWorld = nullptr;
    glm::vec3 m_gravity{0.0f, -9.81f, 0.0f};
    
    /**
     * Raycast vs a single body
     */
    bool raycastVsBody(const glm::vec3& origin, const glm::vec3& direction,
                       const TransformComponent& transform,
                       const RigidBodyComponent& rigidbody,
                       float& outDistance, glm::vec3& outHit, glm::vec3& outNormal) {
        
        switch (rigidbody.colliderType) {
            case ColliderType::SPHERE:
                return raycastVsSphere(origin, direction, transform.position, 
                                       rigidbody.sphereRadius, 
                                       outDistance, outHit, outNormal);
            case ColliderType::BOX:
                return raycastVsBox(origin, direction, transform, rigidbody.boxSize,
                                    outDistance, outHit, outNormal);
            default:
                return false;
        }
    }
    
    /**
     * Raycast vs sphere
     */
    bool raycastVsSphere(const glm::vec3& origin, const glm::vec3& direction,
                         const glm::vec3& center, float radius,
                         float& outDistance, glm::vec3& outHit, glm::vec3& outNormal) {
        
        glm::vec3 oc = origin - center;
        float b = glm::dot(oc, direction);
        float c = glm::dot(oc, oc) - radius * radius;
        float h = b * b - c;
        
        if (h < 0.0f) return false;
        
        h = glm::sqrt(h);
        float t = -b - h;
        
        if (t < 0.0f) {
            t = -b + h;
            if (t < 0.0f) return false;
        }
        
        outDistance = t;
        outHit = origin + direction * t;
        outNormal = glm::normalize(outHit - center);
        
        return true;
    }
    
    /**
     * Raycast vs box (AABB for simplicity)
     */
    bool raycastVsBox(const glm::vec3& origin, const glm::vec3& direction,
                      const TransformComponent& transform, const glm::vec3& size,
                      float& outDistance, glm::vec3& outHit, glm::vec3& outNormal) {
        
        // Transform ray to local space
        glm::mat4 invModel = glm::inverse(transform.getModelMatrix());
        glm::vec3 localOrigin = glm::vec3(invModel * glm::vec4(origin, 1.0f));
        glm::vec3 localDir = glm::normalize(glm::vec3(invModel * glm::vec4(direction, 0.0f)));
        
        glm::vec3 halfSize = size * 0.5f;
        glm::vec3 invDir = 1.0f / localDir;
        
        glm::vec3 t1 = (-halfSize - localOrigin) * invDir;
        glm::vec3 t2 = (halfSize - localOrigin) * invDir;
        
        glm::vec3 tmin = glm::min(t1, t2);
        glm::vec3 tmax = glm::max(t1, t2);
        
        float tNear = glm::max(glm::max(tmin.x, tmin.y), tmin.z);
        float tFar = glm::min(glm::min(tmax.x, tmax.y), tmax.z);
        
        if (tNear > tFar || tFar < 0.0f) return false;
        
        outDistance = (tNear > 0.0f) ? tNear : tFar;
        outHit = origin + direction * outDistance;
        
        // Calculate normal based on hit face
        glm::vec3 localHit = localOrigin + localDir * outDistance;
        glm::vec3 absHit = glm::abs(localHit);
        
        if (absHit.x > halfSize.x - 0.001f) {
            outNormal = glm::vec3(glm::sign(localHit.x), 0.0f, 0.0f);
        } else if (absHit.y > halfSize.y - 0.001f) {
            outNormal = glm::vec3(0.0f, glm::sign(localHit.y), 0.0f);
        } else {
            outNormal = glm::vec3(0.0f, 0.0f, glm::sign(localHit.z));
        }
        
        // Transform normal to world space
        glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform.getModelMatrix())));
        outNormal = glm::normalize(normalMatrix * outNormal);
        
        return true;
    }
};

/**
 * Character Controller System - Handles player movement
 */
class CharacterControllerSystem : public TypedSystem<TransformComponent, CharacterControllerComponent> {
public:
    CharacterControllerSystem() = default;
    
    void init() override {
        m_filter = SystemFilter::require<TransformComponent, CharacterControllerComponent>();
    }
    
    void update(float deltaTime) override {
        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID entityID, TransformComponent& transform, 
                              CharacterControllerComponent& controller) {
                updateCharacter(transform, controller, deltaTime);
            });
    }
    
    void updateCharacter(TransformComponent& transform, 
                        CharacterControllerComponent& controller,
                        float deltaTime) {
        // Apply gravity
        if (!controller.isGrounded) {
            controller.velocity.y += -9.81f * deltaTime;
        }
        
        // Apply input movement
        glm::vec3 moveDir = controller.inputDirection;
        if (glm::length(moveDir) > 0.0f) {
            moveDir = glm::normalize(moveDir);
            transform.position += moveDir * controller.moveSpeed * deltaTime;
        }
        
        // Apply velocity
        transform.position += controller.velocity * deltaTime;
        
        // Ground check
        if (transform.position.y <= 0.0f) {
            transform.position.y = 0.0f;
            controller.velocity.y = 0.0f;
            controller.isGrounded = true;
            controller.canJump = true;
        } else {
            controller.isGrounded = false;
        }
    }
    
    /**
     * Make character jump
     */
    void jump(Entity entity) {
        auto* controller = getComponent<CharacterControllerComponent>(entity.id);
        if (controller && controller->canJump && controller->isGrounded) {
            controller->velocity.y = controller->jumpForce;
            controller->canJump = false;
            controller->isGrounded = false;
        }
    }
    
    /**
     * Set movement input
     */
    void setMovementInput(Entity entity, const glm::vec3& input) {
        auto* controller = getComponent<CharacterControllerComponent>(entity.id);
        if (controller) {
            controller->inputDirection = input;
        }
    }
    
    const char* getName() const override { return "CharacterControllerSystem"; }
};

} // namespace ecs
