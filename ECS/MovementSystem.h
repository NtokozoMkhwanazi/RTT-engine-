#pragma once
#include <GLFW/glfw3.h>
#include "Entity.h"
#include "PhysicsComponent.h"
#include "TransformComponent.h"
#include <glm/glm.hpp>

class MovementSystem {
public:
    MovementSystem(float moveSpeed = 6.0f, float jumpImpulse = 9.0f)
        : moveSpeed(moveSpeed), jumpImpulse(jumpImpulse) {}

    void updateEntity(Entity& entity, GLFWwindow* window, float dt) {
        if (!entity.has<PhysicsComponent>() || !entity.has<TransformComponent>()) return;
        auto physComp = entity.getComponent<PhysicsComponent>();
        if (!physComp || !physComp->body) return;
        auto body = physComp->body;

        glm::vec3 inputDir(0.0f);
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) inputDir.z -= 1.0f;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) inputDir.z += 1.0f;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) inputDir.x -= 1.0f;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) inputDir.x += 1.0f;

        if (glm::length(inputDir) > 0.0001f) inputDir = glm::normalize(inputDir);

        float accel = 20.0f;
        body->velocity.x += inputDir.x * accel * dt;
        body->velocity.z += inputDir.z * accel * dt;

        static bool jumpPressedLast = false;
        bool jumpPressedNow = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (jumpPressedNow && !jumpPressedLast && body->onGround) {
            body->velocity.y = jumpImpulse;
            body->onGround = false;
        }
        jumpPressedLast = jumpPressedNow;
    }

private:
    float moveSpeed;
    float jumpImpulse;
};
