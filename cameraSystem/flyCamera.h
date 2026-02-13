#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>
#include <iostream>

class flyCamera {
public:
    // Camera attributes
    glm::vec3 Position;
    glm::vec3 Target;
    glm::vec3 Up;
    glm::vec3 Right;
    glm::vec3 WorldUp;

    // Euler angles (orbit)
    float Yaw;
    float Pitch;

    // Camera options
    float DistanceToTarget;
    float MouseSensitivity;
    float Zoom;

    // Mouse state
    float LastX, LastY;
    bool FirstMouse;

    // Smooth follow
    float followSpeed = 5.0f;

    // Constructor
    flyCamera(glm::vec3 startPos = glm::vec3(0.0f, 2.0f, 10.0f),
              glm::vec3 up = glm::vec3(0.0f,1.0f,0.0f),
              float yaw = -90.0f,
              float pitch = 20.0f,
              float distance = 8.0f)
        : WorldUp(up), Yaw(yaw), Pitch(pitch),
          MouseSensitivity(0.2f), Zoom(45.0f),
          DistanceToTarget(distance)
    {
        Position = startPos;
        Target = glm::vec3(0.0f);
        FirstMouse = true;
        LastX = 400; LastY = 300;
        updateCameraVectors();
    }

    // Get view matrix
    glm::mat4 GetViewMatrix() {
        return glm::lookAt(Position, Target, Up);
    }

    // Smooth third-person follow
    void FollowPlayerSmooth(const glm::vec3& playerPos, float deltaTime) {
        Target = playerPos;

        glm::vec3 offset;
        offset.x = DistanceToTarget * cos(glm::radians(Pitch)) * sin(glm::radians(Yaw));
        offset.y = DistanceToTarget * sin(glm::radians(Pitch));
        offset.z = DistanceToTarget * cos(glm::radians(Pitch)) * cos(glm::radians(Yaw));

        glm::vec3 desiredPos = Target - offset; // minus ensures proper behind position
        Position = glm::mix(Position, desiredPos, followSpeed * deltaTime);

        updateCameraVectors();
    }

    // Instant follow (optional)
    void FollowPlayer(const glm::vec3& playerPos) {
        Target = playerPos;
        glm::vec3 offset;
        offset.x = DistanceToTarget * cos(glm::radians(Pitch)) * sin(glm::radians(Yaw));
        offset.y = DistanceToTarget * sin(glm::radians(Pitch));
        offset.z = DistanceToTarget * cos(glm::radians(Pitch)) * cos(glm::radians(Yaw));

        Position = Target - offset;
        updateCameraVectors();
    }

    // Mouse rotation
    void ProcessMouseMovement(float xpos, float ypos) {
        if (FirstMouse) { LastX = xpos; LastY = ypos; FirstMouse = false; }

        float xoffset = xpos - LastX;
        float yoffset = LastY - ypos; // reversed
        LastX = xpos;
        LastY = ypos;

        xoffset *= MouseSensitivity;
        yoffset *= MouseSensitivity;

        Yaw += xoffset;
        Pitch += yoffset;

        // Clamp pitch
        if (Pitch > 80.0f) Pitch = 80.0f;
        if (Pitch < -30.0f) Pitch = -30.0f;

        updateCameraVectors();
    }

    // Scroll zoom
    void ProcessMouseScroll(float yoffset) {
        DistanceToTarget -= yoffset;
        if (DistanceToTarget < 0.00f) DistanceToTarget = 0.00f;
        if (DistanceToTarget > 50.0f) DistanceToTarget = 50.0f;
    }

private:
    void updateCameraVectors() {
        // Calculate front, right, up vectors based on orbit
        glm::vec3 front = glm::normalize(Target - Position);
        Right = glm::normalize(glm::cross(front, WorldUp));
        Up = glm::normalize(glm::cross(Right, front));
    }
};

