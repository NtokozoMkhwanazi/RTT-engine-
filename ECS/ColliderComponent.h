#pragma once
#include "Component.h"
#include "ColliderType.h"
#include <string>

struct ColliderComponent : public Component {
    ColliderType type = ColliderType::BOX;
    // optional per-collider params (radius, half-extents, etc)
    float radius = 0.5f;
    glm::vec3 halfExtents {0.5f, 0.5f, 0.5f};

    ColliderComponent() = default;
    ColliderComponent(const std::string& s) {
        if (s == "sphere") type = ColliderType::SPHERE;
        else if (s == "capsule") type = ColliderType::CAPSULE;
        else if (s == "mesh") type = ColliderType::MESH;
        else type = ColliderType::BOX;
    }
};
