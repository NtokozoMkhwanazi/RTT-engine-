#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>

class OrbitCamera {
public:
    glm::vec3 target;   // point we orbit around (cube)
    float radius;       // distance from target
    float yaw;          // horizontal rotation
    float pitch;        // vertical rotation
    float sensitivity;
    float zoomSpeed;

    float lastX, lastY;
    bool firstMouse = true;

    OrbitCamera(glm::vec3 target = glm::vec3(0.0f), float radius = 5.0f)
        : target(target), radius(radius), yaw(-90.0f), pitch(0.0f),
          sensitivity(0.3f), zoomSpeed(1.0f), lastX(400), lastY(300) {}

    glm::mat4 getViewMatrix() {
        glm::vec3 position = getPosition();
        return glm::lookAt(position, target, glm::vec3(0.0f, 1.0f, 0.0f));
    }

    glm::vec3 getPosition() {
        float x = target.x + radius * cos(glm::radians(pitch)) * cos(glm::radians(yaw));
        float y = target.y + radius * sin(glm::radians(pitch));
        float z = target.z + radius * cos(glm::radians(pitch)) * sin(glm::radians(yaw));
        return glm::vec3(x, y, z);
    }
    
    void processMouseMovement(double xpos, double ypos) {
        if (firstMouse) {
            lastX = xpos;
            lastY = ypos;
            firstMouse = false;
        }

        float xoffset = xpos - lastX;
        float yoffset = lastY - ypos; // reversed Y
        lastX = xpos;
        lastY = ypos;

        yaw   += xoffset * sensitivity;
        pitch += yoffset * sensitivity;

        // clamp pitch so we don't flip over top
        if (pitch > 89.0f) pitch = 89.0f;
        if (pitch < -89.0f) pitch = -89.0f;
    }

    void processMouseScroll(double yoffset) {
        radius -= yoffset * zoomSpeed;
        if (radius < 1.0f) radius = 0.000000000000001f;
        if (radius > 30.0f) radius = 30.0f;
    }
};

