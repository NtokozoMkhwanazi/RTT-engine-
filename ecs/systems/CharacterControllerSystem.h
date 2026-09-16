#pragma once

/**
 * Character Controller System - Integrates with engine CharacterController
 * 
 * This system processes CharacterControllerComponent and syncs with
 * the engine's CharacterController for full movement, slopes, and root motion.
 */

#include "../ECS.h"
#include "../components/Components.h"
#include "../../playerSystem/CharacterController.h"
#include "../../physicsSystem/Physics.h"
#include "../../physicsSystem/RigidBody.h"
#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>

namespace ecs {

/**
 * Character Controller System - Integrates with engine CharacterController
 */
class CharacterControllerSystem : public TypedSystem<TransformComponent, CharacterControllerComponent> {
public:
    CharacterControllerSystem() = default;

    /**
     * Set the physics world (for character controller creation)
     */
    void setPhysicsWorld(PhysicsWorld* physicsWorld) {
        m_physicsWorld = physicsWorld;
    }

    void init() override {
        m_filter = SystemFilter::require<TransformComponent, CharacterControllerComponent>();
    }

    void update(float deltaTime) override {
        if (!m_entityManager || !m_componentManager) return;

        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID entityID, TransformComponent& transform, CharacterControllerComponent& controller) {
                updateCharacter(entityID, transform, controller, deltaTime);
            });
    }

    /**
     * Update a single character
     */
    void updateCharacter(EntityID entityID, TransformComponent& transform, CharacterControllerComponent& controller, float deltaTime) {
        // Get or create engine CharacterController for this entity
        auto it = m_controllers.find(entityID);
        if (it == m_controllers.end()) {
            // Create new controller
            createController(entityID, transform, controller);
            it = m_controllers.find(entityID);
        }

        if (it != m_controllers.end() && it->second) {
            auto& charController = it->second;

            // Set input from ECS component
            charController->setMoveInput(controller.inputDirection);

            // Handle jump
            if (controller.wantsJump && controller.canJump && controller.isGrounded) {
                charController->jump();
                controller.wantsJump = false;  // Consume jump input
            }

            // Handle crouch
            charController->crouch(controller.isCrouching);

            // Handle sprint
            charController->setSprint(controller.isSprinting);

            // Update character controller (applies movement, gravity, etc.)
            charController->update(deltaTime, controller.rootMotion);

            // Sync transform back from character controller's body
            if (charController->isGrounded()) {
                controller.isGrounded = true;
                controller.canJump = true;
            } else {
                controller.isGrounded = false;
                controller.canJump = false;
            }
        }
    }

    /**
     * Create engine CharacterController for an entity
     */
    void createController(EntityID entityID, TransformComponent& transform, CharacterControllerComponent& controller) {
        if (!m_physicsWorld) return;

        // Create rigidbody for character (added directly to contiguous storage)
        RigidBody body;
        body.position = transform.position;
        body.size = glm::vec3(controller.radius * 2.0f, controller.height, controller.radius * 2.0f);
        body.mass = 80.0f;  // Default character mass
        body.colliderType = ::ColliderType::CAPSULE;
        body.isModel = false;

        // Add to physics world (returns handle)
        BodyHandle handle = m_physicsWorld->addBody(body);

        // Create character controller with direct pointer
        auto charController = std::make_unique<CharacterController>(m_physicsWorld->getBody(handle), m_physicsWorld);
        
        // Configure character controller
        charController->setWalkSpeed(controller.moveSpeed);
        charController->setRunSpeed(controller.moveSpeed * controller.sprintMultiplier);
        charController->setCrouchSpeed(controller.moveSpeed * 0.5f);
        charController->setJumpSpeed(controller.jumpForce);
        charController->setMaxSlopeAngle(controller.slopeLimit);

        m_controllers[entityID] = std::move(charController);
        m_bodies[entityID] = handle;
    }

    /**
     * Set movement input for a character
     */
    void setMoveInput(Entity entity, const glm::vec3& direction) {
        auto* controller = m_componentManager->getComponent<CharacterControllerComponent>(entity.id);
        if (controller) {
            controller->inputDirection = direction;
        }
    }

    /**
     * Trigger jump for a character
     */
    void jump(Entity entity) {
        auto* controller = m_componentManager->getComponent<CharacterControllerComponent>(entity.id);
        if (controller) {
            controller->wantsJump = true;
        }
    }

    /**
     * Set crouching state
     */
    void setCrouching(Entity entity, bool crouching) {
        auto* controller = m_componentManager->getComponent<CharacterControllerComponent>(entity.id);
        if (controller) {
            controller->isCrouching = crouching;
        }
    }

    /**
     * Set sprinting state
     */
    void setSprinting(Entity entity, bool sprinting) {
        auto* controller = m_componentManager->getComponent<CharacterControllerComponent>(entity.id);
        if (controller) {
            controller->isSprinting = sprinting;
        }
    }

    /**
     * Teleport character to position
     */
    void teleport(Entity entity, const glm::vec3& position) {
        auto it = m_bodies.find(entity.id);
        if (it != m_bodies.end() && m_physicsWorld) {
            RigidBody* body = m_physicsWorld->getBody(it->second);
            if (body) {
                body->position = position;
            }
        }
        
        auto* transform = m_componentManager->getComponent<TransformComponent>(entity.id);
        if (transform) {
            transform->position = position;
        }
    }

    /**
     * Get character's grounded state
     */
    bool isGrounded(Entity entity) const {
        auto* controller = m_componentManager->getComponent<CharacterControllerComponent>(entity.id);
        return controller ? controller->isGrounded : false;
    }

    /**
     * Get character's velocity
     */
    glm::vec3 getVelocity(Entity entity) const {
        auto it = m_bodies.find(entity.id);
        if (it != m_bodies.end() && m_physicsWorld) {
            const RigidBody* body = m_physicsWorld->getBody(it->second);
            if (body) {
                return body->velocity;
            }
        }
        return glm::vec3(0.0f);
    }

    const char* getName() const override { return "CharacterControllerSystem (Integrated)"; }

private:
    PhysicsWorld* m_physicsWorld = nullptr;
    
    // Map from ECS entity ID to engine CharacterController
    std::unordered_map<EntityID, std::unique_ptr<CharacterController>> m_controllers;
    std::unordered_map<EntityID, BodyHandle> m_bodies;
};

} // namespace ecs
