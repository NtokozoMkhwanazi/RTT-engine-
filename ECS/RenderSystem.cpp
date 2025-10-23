#include "RenderSystem.h"
#include "Entity.h"
#include "TransformComponent.h"
#include "ModelComponent.h"
#include "Shader.h"
#include "flyCamera.h"
#include <glm/gtc/matrix_transform.hpp>

void RenderSystem::render(Shader& shader, flyCamera& cam, const std::vector<std::shared_ptr<Entity>>& entities) {
    for (auto& e : entities) {
        auto transform = e->getComponent<TransformComponent>();
        auto modelComp = e->getComponent<ModelComponent>();

        if (!transform || !modelComp || !modelComp->model) continue;

        glm::mat4 modelMat = glm::translate(glm::mat4(1.0f), transform->position);
        modelMat = modelMat * glm::mat4_cast(transform->rotation);
        modelMat = glm::scale(modelMat, transform->scale);

        shader.setMat4("model", modelMat);
        modelComp->model->Draw(shader);
    }
}
