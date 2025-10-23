#pragma once
#include <vector>
#include <memory>
#include "Entity.h"
#include "PhysicsComponent.h"
#include "TransformComponent.h"
#include "Physics.h"

class PhysicsSystem {
public:
    PhysicsSystem(PhysicsWorld* world);
    void step(float dt, std::vector<std::shared_ptr<Entity>>& entities);
    void ensureBodyForEntity(std::shared_ptr<Entity>& e);

private:
    PhysicsWorld* physicsWorld = nullptr;
};
