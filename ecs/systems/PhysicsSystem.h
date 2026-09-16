#pragma once

/**
 * Integrated Physics System - Uses engine's PhysicsWorld
 * 
 * This system syncs ECS RigidBodyComponent with PhysicsWorld
 * and uses engine's GJK collision detection.
 */

#include "../ECS.h"
#include "../components/Components.h"
#include "../../physicsSystem/Physics.h"
#include "../../physicsSystem/RigidBody.h"
#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>

namespace ecs {

/**
 * Physics System - Integrates ECS with engine PhysicsWorld
 */
class PhysicsSystem : public TypedSystem<TransformComponent, RigidBodyComponent> {
public:
    PhysicsSystem() = default;
    ~PhysicsSystem() = default;

    /**
     * Set the engine's physics world
     */
    void setPhysicsWorld(PhysicsWorld* physicsWorld) { 
        m_physicsWorld = physicsWorld; 
    }

    PhysicsWorld* getPhysicsWorld() const { return m_physicsWorld; }

    void setGravity(const glm::vec3& gravity) { 
        m_gravity = gravity;
        if (m_physicsWorld) {
            m_physicsWorld->gravity = gravity;
        }
    }
    glm::vec3 getGravity() const { return m_gravity; }
    
    void setSubsteps(int substeps) { m_substeps = substeps; }
    int getSubsteps() const { return m_substeps; }

    void init() override {
        m_filter = SystemFilter::require<TransformComponent, RigidBodyComponent>();
    }

    void update(float deltaTime) override {
        if (!m_physicsWorld) return;

        // Step 1: Sync ECS components TO engine bodies
        syncToEngine();

        // Step 2: Step the physics simulation (uses GJK, constraints, etc.)
        float dt = std::min(deltaTime, 0.1f);
        m_physicsWorld->step(dt);

        // Step 3: Sync engine bodies BACK to ECS components
        syncFromEngine();
    }

    /**
     * Sync ECS RigidBodyComponents to PhysicsWorld bodies
     * Full sync including all physics properties
     */
    void syncToEngine() {
        if (!m_entityManager || !m_componentManager) return;

        // Clear old bodies
        m_physicsWorld->bodies.clear();
        m_bodyMap.clear();

        // Create engine bodies from ECS components
        forEach(*m_entityManager, *m_componentManager,
            [this](EntityID entityID, TransformComponent& transform, RigidBodyComponent& ecsRb) {

                // Create engine RigidBody with full physics properties
                auto engineBody = std::make_shared<::RigidBody>();

                // Sync transform
                engineBody->position = transform.position;
                engineBody->rotation = transform.rotation;
                engineBody->scale = transform.scale;
                engineBody->prevPosition = transform.position;
                engineBody->prevRotation = transform.rotation;

                // Sync rigidbody properties
                engineBody->mass = ecsRb.mass;
                engineBody->isStatic = (ecsRb.bodyType == RigidBodyType::STATIC);
                engineBody->restitution = ecsRb.restitution;
                engineBody->friction = ecsRb.friction;
                engineBody->linearDamping = ecsRb.linearDamping;
                engineBody->angularDamping = ecsRb.angularDamping;
                
                // Sync advanced physics properties
                engineBody->buoyancyFactor = ecsRb.buoyancyFactor;
                engineBody->density = ecsRb.density;
                engineBody->staticFriction = ecsRb.staticFriction;
                engineBody->dynamicFriction = ecsRb.dynamicFriction;
                engineBody->rollingResistance = ecsRb.rollingResistance;
                engineBody->useContinuousCollisionDetection = ecsRb.useCCD;

                // Set collider dimensions based on type
                switch (ecsRb.colliderType) {
                    case ColliderType::BOX:
                        engineBody->size = ecsRb.boxSize;
                        break;
                    case ColliderType::SPHERE:
                        engineBody->size = glm::vec3(ecsRb.sphereRadius * 2.0f);
                        break;
                    case ColliderType::CAPSULE:
                        engineBody->size = glm::vec3(ecsRb.capsuleRadius * 2.0f, ecsRb.capsuleHeight, ecsRb.capsuleRadius * 2.0f);
                        break;
                    default:
                        break;
                }

                // Sync velocity and accumulators
                engineBody->velocity = ecsRb.linearVelocity;
                engineBody->angularVelocity = ecsRb.angularVelocity;
                engineBody->forceAccumulator = ecsRb.force;
                engineBody->torqueAccumulator = ecsRb.torque;

                // Add to physics world
                m_physicsWorld->addBody(engineBody);

                // Store mapping
                m_bodyMap[entityID] = engineBody;
            });
    }

    /**
     * Sync PhysicsWorld results back to ECS components
     * Full sync including all physics state
     */
    void syncFromEngine() {
        if (!m_entityManager || !m_componentManager) return;

        for (auto& [entityID, engineBody] : m_bodyMap) {
            auto* ecsTransform = m_componentManager->getComponent<TransformComponent>(entityID);
            auto* ecsRb = m_componentManager->getComponent<RigidBodyComponent>(entityID);

            if (!ecsTransform || !ecsRb) continue;

            // Sync transform from engine body
            ecsTransform->position = engineBody->position;
            ecsTransform->rotation = engineBody->rotation;

            // Sync velocity
            ecsRb->linearVelocity = engineBody->velocity;
            ecsRb->angularVelocity = engineBody->angularVelocity;

            // Sync grounded state
            ecsRb->isGrounded = engineBody->onGround;

            // Sync advanced physics state
            ecsRb->force = engineBody->forceAccumulator;
            ecsRb->torque = engineBody->torqueAccumulator;
        }
    }

    /**
     * Apply force to an entity's rigidbody
     */
    void applyForce(Entity entity, const glm::vec3& force) {
        auto it = m_bodyMap.find(entity.id);
        if (it != m_bodyMap.end() && it->second) {
            it->second->forceAccumulator += force;
        }
    }

    /**
     * Apply impulse to an entity's rigidbody
     */
    void applyImpulse(Entity entity, const glm::vec3& impulse) {
        auto it = m_bodyMap.find(entity.id);
        if (it != m_bodyMap.end() && it->second) {
            auto* body = it->second.get();
            if (body->mass > 0.0f) {
                body->velocity += impulse / body->mass;
            }
        }
    }

    /**
     * Get engine body for an entity
     */
    std::shared_ptr<::RigidBody> getEngineBody(Entity entity) {
        auto it = m_bodyMap.find(entity.id);
        return (it != m_bodyMap.end()) ? it->second : nullptr;
    }

    const char* getName() const override { return "PhysicsSystem (Integrated)"; }

private:
    PhysicsWorld* m_physicsWorld = nullptr;
    glm::vec3 m_gravity{0.0f, -9.81f, 0.0f};
    int m_substeps = 4;
    
    // Map from ECS entity ID to engine RigidBody
    std::unordered_map<EntityID, std::shared_ptr<::RigidBody>> m_bodyMap;
};

} // namespace ecs
