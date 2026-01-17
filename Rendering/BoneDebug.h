#pragma once
#include <glm/glm.hpp>
#include <vector>

class BoneDebug {
public:
    void Draw(const std::vector<glm::mat4>& bones,
              const glm::mat4& model,
              const glm::mat4& view,
              const glm::mat4& proj);
};

