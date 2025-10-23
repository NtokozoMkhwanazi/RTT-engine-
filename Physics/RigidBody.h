#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "ColliderType.h"

// Simple rigidbody used by our custom physics
struct RigidBody {
    glm::vec3 position {0.0f};
    glm::vec3 velocity {0.0f};
    glm::quat rotation {1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale {1.0f};
    float mass {1.0f};
    bool isStatic {false};
    ColliderType colliderType {ColliderType::BOX};
    float restitution {0.0f}; // bounciness
    bool onGround {false};

    // Accumulators
    glm::vec3 forceAccumulator {0.0f};
    glm::vec3 torqueAccumulator {0.0f};
    glm::vec3 angularVelocity {0.0f};

    // Helpers
    RigidBody() = default;
    RigidBody(const glm::vec3& pos, const glm::vec3& scl, float m, bool stat, ColliderType type = ColliderType::BOX)
        : position(pos), scale(scl), mass(m), isStatic(stat), colliderType(type) {}

    void applyForce(const glm::vec3& f) {
        if (!isStatic) forceAccumulator += f;
    }

    void clearAccumulators() {
        forceAccumulator = glm::vec3(0.0f);
        torqueAccumulator = glm::vec3(0.0f);
    }

    float invMass() const { return (mass > 0.0f && !isStatic) ? 1.0f / mass : 0.0f; }
};

