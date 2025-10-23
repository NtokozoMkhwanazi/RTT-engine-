#pragma once
#include "Entity.h"
#include "PhysicsComponent.h"
#include "TransformComponent.h"
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtx/norm.hpp> // for glm::length2
#include "link/Rendering/flyCamera.h"

class flyCamera;

class PlayerController {
public:
    PlayerController(std::shared_ptr<Entity> player)
        : playerEntity(player) {}

    void update(GLFWwindow* window, float dt, flyCamera* camera) {
        if (!playerEntity) return;

        // Get PhysicsComponent
        auto physicsComp = playerEntity->getComponent<PhysicsComponent>();
        if (!physicsComp || !physicsComp->body) return;

        // Get TransformComponent (optional)
        auto transformComp = playerEntity->getComponent<TransformComponent>();

        auto rb = physicsComp->body;

        glm::vec3 inputDir(0.0f);
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) inputDir.z -= 1.0f;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) inputDir.z += 1.0f;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) inputDir.x -= 1.0f;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) inputDir.x += 1.0f;

        if (glm::length2(inputDir) > 0.0f)
            inputDir = glm::normalize(inputDir);

        glm::vec3 forward = camera->Front;
        forward.y = 0.0f;
        forward = glm::normalize(forward);
        glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0,1,0)));

        float moveSpeed = 8.0f;
        float acceleration = 10.0f;
        float airControl = 0.3f;
        float gravity = 9.8f;
        float jumpImpulse = 6.0f;
        float drag = 0.8f;

        glm::vec3 targetVelocity = forward * inputDir.z * moveSpeed
                                 + right * inputDir.x * moveSpeed;

        float control = rb->onGround ? 1.0f : airControl;
        rb->velocity.x += (targetVelocity.x - rb->velocity.x) * acceleration * dt * control;
        rb->velocity.z += (targetVelocity.z - rb->velocity.z) * acceleration * dt * control;

        rb->velocity.y -= gravity * dt;

        bool jumpPressed = (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS);
        if (jumpPressed && rb->onGround) {
            rb->velocity.y = jumpImpulse;
            rb->onGround = false;
        }

        if (rb->onGround) {
            rb->velocity.x *= drag;
            rb->velocity.z *= drag;
        }

        rb->position += rb->velocity * dt;

        // Sync back to TransformComponent
        if (transformComp) {
            transformComp->position = rb->position;
        }
    }

private:
    std::shared_ptr<Entity> playerEntity;
};
