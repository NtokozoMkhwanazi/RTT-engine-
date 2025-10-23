#pragma once
#include <vector>
#include <memory>
#include <glm/glm.hpp>
#include "RigidBody.h"

class PhysicsWorld {
public:
    PhysicsWorld();

    std::vector<std::shared_ptr<RigidBody>> bodies;
    glm::vec3 gravity;

    void addBody(const std::shared_ptr<RigidBody>& body);
    void step(float deltaTime);
    void clear();

private:
    void integrateBody(std::shared_ptr<RigidBody>& b, float dt);
    void handleCollisions();
    bool checkAABBCollision(const std::shared_ptr<RigidBody>& a,
                            const std::shared_ptr<RigidBody>& b);
    void resolveCollision(std::shared_ptr<RigidBody>& a,
                          std::shared_ptr<RigidBody>& b);
    void applyCollisionResponse(std::shared_ptr<RigidBody>& a,
                            std::shared_ptr<RigidBody>& b,
                            const glm::vec3& normal,
                            float penetration);

};

