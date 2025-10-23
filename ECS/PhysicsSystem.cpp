#include "PhysicsSystem.h"
#include "Entity.h"
#include "PhysicsComponent.h"
#include "TransformComponent.h"
#include "Physics.h" // RigidBody, PhysicsWorld
#include <glm/glm.hpp>
#include <iostream>

PhysicsSystem::PhysicsSystem(PhysicsWorld* world) : physicsWorld(world) {}

void PhysicsSystem::ensureBodyForEntity(std::shared_ptr<Entity>& e) {
    auto physComp = e->getComponent<PhysicsComponent>();
    auto tf = e->getComponent<TransformComponent>();
    if (!physComp || !tf) return;

    // If no body yet, create one with defaults:
    if (!physComp->body) {
        auto body = std::make_shared<RigidBody>(
            tf->position,   // pos
            tf->scale,      // scale
            1.0f,           // mass default
            false,          // isStatic default
            ColliderType::BOX // default
        );
        physComp->body = body;
        if (physicsWorld) physicsWorld->addBody(body);
    }
}


void PhysicsSystem::step(float dt, std::vector<std::shared_ptr<Entity>>& entities) {
    if (!physicsWorld) return;

    // Ensure all entities with PhysicsComponent have RigidBody registered
    for (auto &e : entities) ensureBodyForEntity(e);

    // Let the low-level physics world step (handles integration/collisions)
    physicsWorld->step(dt);

    // After physics step, copy back positions -> transform components
    for (auto& e : entities) {
        auto physComp = e->getComponent<PhysicsComponent>();
        auto tf = e->getComponent<TransformComponent>();
        if (!physComp || !tf || !physComp->body) continue;

        tf->position = physComp->body->position;
        tf->rotation = physComp->body->rotation;
        tf->scale    = physComp->body->scale;
    }
}
