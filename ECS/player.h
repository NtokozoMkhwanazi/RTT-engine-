#pragma once
#include "link/Physics/Physics.h"
#include <glm/glm.hpp>
#include <GLFW/glfw3.h>

class Player {
public:
    std::shared_ptr<RigidBody> body;

    Player(std::shared_ptr<RigidBody> b) : body(b) {}

    void processInput(GLFWwindow* window) {
        glm::vec3 inputForce(0.0f);

        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) inputForce.z -= 1.0f;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) inputForce.z += 1.0f;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) inputForce.x -= 1.0f;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) inputForce.x += 1.0f;

        // Jump
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
            float floorY = 0.0f; // adjust as needed
            if (body->position.y - body->scale.y / 2 <= floorY + 0.01f)
                body->velocity.y = 10.0f;
        }

        if (glm::length(inputForce) > 0.0f) {
            inputForce = glm::normalize(inputForce) * 50.0f;
            body->applyForce(inputForce);
        }
    }
};
