#pragma once
#include <glm/glm.hpp>
#include <memory>

struct TOIResult {
    float t = 0.0f;
    bool hit = false;
    glm::vec3 normal = glm::vec3(0);
};

