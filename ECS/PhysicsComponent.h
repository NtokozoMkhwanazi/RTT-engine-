#pragma once
#include "Component.h"
#include <memory>
#include "link/Physics/RigidBody.h"

// PhysicsComponent.h
struct PhysicsComponent : public Component {
    std::shared_ptr<RigidBody> body;
    PhysicsComponent(std::shared_ptr<RigidBody> b) : body(b) {}
};
