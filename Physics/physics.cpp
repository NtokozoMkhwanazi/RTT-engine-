#include "Physics.h"
#include "RigidBody.h"
#include <algorithm>
#include <glm/gtx/norm.hpp>
#include <iostream>


PhysicsWorld::PhysicsWorld()
{

}


void PhysicsWorld::addBody(const std::shared_ptr<RigidBody>& body) {
    bodies.push_back(body);
}

void PhysicsWorld::clear() {
    bodies.clear();
}

void PhysicsWorld::step(float dt) {
    if (dt <= 0.0f) return;

    // --- Apply gravity and integrate motion ---
    for (auto& b : bodies) {
        if (!b || b->isStatic) continue;

        // apply gravity
        b->velocity += gravity * dt;

        // apply forces
        glm::vec3 acc = b->forceAccumulator / b->mass;
        b->velocity += acc * dt;

        // integrate position
        b->position += b->velocity * dt;

        // simple drag to prevent infinite sliding
        b->velocity *= 0.995f;

        // clear forces
        b->forceAccumulator = glm::vec3(0.0f);
        b->onGround = false;
    }

    // --- Collision detection & response ---
    handleCollisions();
}

bool PhysicsWorld::checkAABBCollision(const std::shared_ptr<RigidBody>& a,
                                      const std::shared_ptr<RigidBody>& b) {
    glm::vec3 aMin = a->position - a->scale * 0.5f;
    glm::vec3 aMax = a->position + a->scale * 0.5f;
    glm::vec3 bMin = b->position - b->scale * 0.5f;
    glm::vec3 bMax = b->position + b->scale * 0.5f;

    return (aMin.x <= bMax.x && aMax.x >= bMin.x) &&
           (aMin.y <= bMax.y && aMax.y >= bMin.y) &&
           (aMin.z <= bMax.z && aMax.z >= bMin.z);
}

void PhysicsWorld::handleCollisions() {
    for (size_t i = 0; i < bodies.size(); ++i) {
        for (size_t j = i + 1; j < bodies.size(); ++j) {
            auto& A = bodies[i];
            auto& B = bodies[j];
            if (!A || !B || (A->isStatic && B->isStatic)) continue;
            if (!checkAABBCollision(A, B)) continue;
            resolveCollision(A, B);
        }
    }
}

void PhysicsWorld::resolveCollision(std::shared_ptr<RigidBody>& a,
                                    std::shared_ptr<RigidBody>& b) {
    // Compute overlap (penetration)
    glm::vec3 delta = b->position - a->position;
    glm::vec3 totalHalf = (a->scale + b->scale) * 0.5f;

    float dx = totalHalf.x - std::abs(delta.x);
    float dy = totalHalf.y - std::abs(delta.y);
    float dz = totalHalf.z - std::abs(delta.z);

    // smallest penetration axis
    if (dx < dy && dx < dz) {
        float sign = (delta.x > 0) ? 1.0f : -1.0f;
        glm::vec3 normal(sign, 0.0f, 0.0f);
        float penetration = dx;
        applyCollisionResponse(a, b, normal, penetration);
    } else if (dy < dz) {
        float sign = (delta.y > 0) ? 1.0f : -1.0f;
        glm::vec3 normal(0.0f, sign, 0.0f);
        float penetration = dy;
        applyCollisionResponse(a, b, normal, penetration);

        // ground detection
        if (sign < 0) a->onGround = true;
        if (sign > 0) b->onGround = true;
    } else {
        float sign = (delta.z > 0) ? 1.0f : -1.0f;
        glm::vec3 normal(0.0f, 0.0f, sign);
        float penetration = dz;
        applyCollisionResponse(a, b, normal, penetration);
    }
}

void PhysicsWorld::applyCollisionResponse(std::shared_ptr<RigidBody>& a,
                                          std::shared_ptr<RigidBody>& b,
                                          const glm::vec3& normal,
                                          float penetration) {
    float totalMass = (a->isStatic ? 0.0f : a->mass) + (b->isStatic ? 0.0f : b->mass);
    if (totalMass <= 0.0f) return;

    // positional correction (to separate bodies)
    glm::vec3 correction = normal * (penetration + 0.001f);
    if (!a->isStatic)
        a->position -= correction * (b->mass / totalMass);
    if (!b->isStatic)
        b->position += correction * (a->mass / totalMass);

    // relative velocity
    glm::vec3 relVel = b->velocity - a->velocity;
    float velAlongNormal = glm::dot(relVel, normal);

    if (velAlongNormal > 0.0f)
        return; // moving apart

    // restitution (bounciness)
    float e = std::min(a->restitution, b->restitution);

    // impulse scalar
    float j = -(1 + e) * velAlongNormal;
    j /= (a->isStatic ? 0.0f : 1 / a->mass) + (b->isStatic ? 0.0f : 1 / b->mass);

    glm::vec3 impulse = j * normal;
    if (!a->isStatic) a->velocity -= impulse / a->mass;
    if (!b->isStatic) b->velocity += impulse / b->mass;

    // simple friction
    glm::vec3 tangent = relVel - (velAlongNormal * normal);
    if (glm::length2(tangent) > 0.0001f) {
        tangent = glm::normalize(tangent);
        float frictionCoeff = 0.5f; // tweakable
        glm::vec3 frictionImpulse = -frictionCoeff * j * tangent;
        if (!a->isStatic) a->velocity -= frictionImpulse / a->mass;
        if (!b->isStatic) b->velocity += frictionImpulse / b->mass;
    }
}

