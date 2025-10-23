#include "PlayerControllerSystem.h"
#include "Entity.h"
#include "TransformComponent.h"
#include "PhysicsComponent.h"
#include "flyCamera.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

void PlayerControllerSystem::update(float dt, GLFWwindow* window, std::vector<std::shared_ptr<Entity>>& entities) {
    if (!window) return;
    std::shared_ptr<Entity> player = nullptr;
    for (auto& e : entities) if (e->name == "player") { player = e; break; }
    if (!player) return;

    auto physComp = player->getComponent<PhysicsComponent>();
    auto tf = player->getComponent<TransformComponent>();
    if (!physComp || !physComp->body || !tf) return;
    auto body = physComp->body;

    glm::vec3 inputDir(0.0f);
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) inputDir.z -= 1.0f;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) inputDir.z += 1.0f;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) inputDir.x -= 1.0f;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) inputDir.x += 1.0f;
    if (glm::length(inputDir) > 0.001f) inputDir = glm::normalize(inputDir);

    glm::vec3 camForward(0,0,-1), camRight(1,0,0);
    if (camera) {
        camForward = camera->Front;
        camForward.y = 0.0f;
        if (glm::length(camForward) > 0.001f) camForward = glm::normalize(camForward);
        camRight = glm::normalize(glm::cross(camForward, glm::vec3(0,1,0)));
    }

    float moveSpeed = 8.0f;
    glm::vec3 desiredVel = camForward * (inputDir.z * moveSpeed) + camRight * (inputDir.x * moveSpeed);
    float accel = 20.0f;
    float airControl = body->onGround ? 1.0f : 0.25f;
    body->velocity.x += (desiredVel.x - body->velocity.x) * std::min(1.0f, accel * dt) * airControl;
    body->velocity.z += (desiredVel.z - body->velocity.z) * std::min(1.0f, accel * dt) * airControl;

    float gravity = 9.81f;
    body->velocity.y -= gravity * dt;

    bool jumpPressed = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
    if (jumpPressed && !jumpPressedLast && body->onGround) {
        body->velocity.y = 6.0f;
        body->onGround = false;
    }
    jumpPressedLast = jumpPressed;
}
