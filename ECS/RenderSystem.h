#pragma once
#include <vector>
#include <memory>
#include "Entity.h"
#include "TransformComponent.h"
#include "ModelComponent.h"
#include "link/Rendering/Shader.h"
#include "link/Rendering/flyCamera.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp> // for mat4_cast

class RenderSystem {
public:
    RenderSystem() = default;

    void render(Shader& shader, flyCamera& cam, const std::vector<std::shared_ptr<Entity>>& entities);
};
