#pragma once
#include <memory>
#include <GLFW/glfw3.h>
#include "physicsSystem/RigidBody.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

class Player {
public:
    std::shared_ptr<RigidBody> body;

    float maxSpeed = 5.0f;
    float accel = 20.0f;
    float airControl = 0.3f;
    float frictionGround = 5.0f;
    float frictionAir = 2.0f;
    float jumpVel = 5.0f;

private:
    bool lastJump = false;

public:
    Player(std::shared_ptr<RigidBody> b);

    void processInput(GLFWwindow* window, float dt);

    void update(GLFWwindow* window, float dt);

    // Interpolated position for rendering
    glm::vec3 getInterpolatedPosition(float alpha) const {
        return glm::mix(body->prevPosition, body->position, alpha);
    }

    // Interpolated rotation for rendering
    glm::quat getInterpolatedRotation(float alpha) const {
        return glm::slerp(body->prevRotation, body->rotation, alpha);
    }
};

