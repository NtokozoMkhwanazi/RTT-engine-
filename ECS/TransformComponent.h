#pragma once
#include "Component.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// TransformComponent.h
struct TransformComponent : public Component {
    glm::vec3 position;
    glm::quat rotation;
    glm::vec3 scale;

    TransformComponent(
        const glm::vec3& pos = glm::vec3(0.0f),
        const glm::vec3& rotEuler = glm::vec3(0.0f),
        const glm::vec3& scl = glm::vec3(1.0f))
        : position(pos), rotation(glm::quat(rotEuler)), scale(scl) {}
};
