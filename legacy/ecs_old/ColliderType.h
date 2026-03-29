#pragma once
#include <glm/glm.hpp>

enum class ColliderType {
    NONE,
    BOX,
    SPHERE,
    CAPSULE,
    MESH
};

struct Collider {
    ColliderType type = ColliderType::NONE;

    // Box dimensions
    glm::vec3 size = glm::vec3(1.0f);

    // Sphere radius
    float radius = 1.0f;

    // Capsule radius & height
    float capsuleHeight = 2.0f;

    // Local offset from entity origin
    glm::vec3 offset = glm::vec3(0.0f);

    // Constructors
    Collider() = default;

    static Collider Box(const glm::vec3& s, const glm::vec3& o = glm::vec3(0.0f)) {
        Collider c;
        c.type = ColliderType::BOX;
        c.size = s;
        c.offset = o;
        return c;
    }

    static Collider Sphere(float r, const glm::vec3& o = glm::vec3(0.0f)) {
        Collider c;
        c.type = ColliderType::SPHERE;
        c.radius = r;
        c.offset = o;
        return c;
    }

    static Collider Capsule(float r, float h, const glm::vec3& o = glm::vec3(0.0f)) {
        Collider c;
        c.type = ColliderType::CAPSULE;
        c.radius = r;
        c.capsuleHeight = h;
        c.offset = o;
        return c;
    }
};
