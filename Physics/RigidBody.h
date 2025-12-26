#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

enum class ColliderType { BOX, SPHERE };

struct RigidBody {
    glm::vec3 tempPosition;
    glm::vec3 position {0.0f};
    glm::vec3 prevPosition {0.0f}; 

    // Quaternion-based rotation
    glm::quat rotation {1.0f, 0.0f, 0.0f, 0.0f};       // current rotation
    glm::quat prevRotation {1.0f, 0.0f, 0.0f, 0.0f};   // previous rotation for interpolation

    glm::vec3 velocity {0.0f};
    glm::vec3 angularVelocity {0.0f};
    glm::vec3 scale {1.0f};

    // Inertia
    glm::mat3 inertiaTensor {0.0f};
    glm::mat3 inertiaLocalInv {1.0f};

    float mass {1.0f};
    bool isStatic {false};
    ColliderType colliderType {ColliderType::BOX};
    float restitution {0.0f};
    float friction {0.0f};
    bool onGround {false};
    bool isModel {false};
    bool isPlayer {false};

    glm::vec3 forceAccumulator {0.0f};
    glm::vec3 torqueAccumulator {0.0f};

    RigidBody() = default;

    RigidBody(const glm::vec3& pos, const glm::vec3& scl, float m, bool stat, ColliderType type = ColliderType::BOX)
        : position(pos), prevPosition(pos), scale(scl), mass(m), isStatic(stat), colliderType(type) 
    {
        computeBoxInertia();
    }

    // Save previous state for interpolation
    inline void savePrevState() {
        prevPosition = position;
        prevRotation = rotation;
    }

    void applyForce(const glm::vec3& f) {
        if (!isStatic) forceAccumulator += f;
    }

    void applyTorque(const glm::vec3& t) {
        if (!isStatic) torqueAccumulator += t;
    }

    void clearAccumulators() { 
        forceAccumulator = glm::vec3(0.0f); 
        torqueAccumulator = glm::vec3(0.0f);
        if (onGround && std::abs(velocity.y) < 0.02f) {
            velocity.y = 0.0f;
        }
    }

    float invMass() const { return (mass > 0.0f && !isStatic) ? 1.0f / mass : 0.0f; }

    void computeBoxInertia() {
        if (isStatic || mass <= 0.0f) {
            inertiaLocalInv = glm::mat3(0.0f);
            return;
        }

        float w = scale.x, h = scale.y, d = scale.z;
        float ix = (1.0f/12.0f) * mass * (h*h + d*d);
        float iy = (1.0f/12.0f) * mass * (w*w + d*d);
        float iz = (1.0f/12.0f) * mass * (w*w + h*h);

        inertiaLocalInv = glm::mat3(
            (ix > 0.0f ? 1.0f/ix : 0.0f), 0.0f, 0.0f,
            0.0f, (iy > 0.0f ? 1.0f/iy : 0.0f), 0.0f,
            0.0f, 0.0f, (iz > 0.0f ? 1.0f/iz : 0.0f)
        );
    }
};

