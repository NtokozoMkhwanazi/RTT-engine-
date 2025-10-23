#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>
#include <iostream>

class flyCamera {
public:
    // Camera attributes
    glm::vec3 Position;
    glm::vec3 Front;
    glm::vec3 Up;
    glm::vec3 Right;
    glm::vec3 WorldUp;

    // Euler angles
    float Yaw;
    float Pitch;

    // Options
    float MovementSpeed;
    float MouseSensitivity;
    float Zoom;

    // Mouse state
    float LastX, LastY;
    bool FirstMouse;

    // 3rd person target
    glm::vec3 Target;
    float DistanceToTarget;

    // Smooth follow
    float followSpeed = 5.0f;
    glm::vec3 desiredPositionOffset = glm::vec3(0.0f, 3.0f, 8.0f);

    // Constructor
    flyCamera(glm::vec3 startPos = glm::vec3(0.0f, 2.0f, 5.0f),
              glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f),
              float yaw = -90.0f,
              float pitch = 20.0f,
              float distance = 8.0f)
        : Front(glm::vec3(0.0f,0.0f,-1.0f)),
          MovementSpeed(5.0f),
          MouseSensitivity(1.0f),
          Zoom(45.0f),
          DistanceToTarget(distance)
    {
        Position = startPos;
        WorldUp = up;
        Yaw = yaw;
        Pitch = pitch;
        FirstMouse = true;
        LastX = 400;
        LastY = 300;
        Target = glm::vec3(0.0f);
        updateCameraVectors();
    }

    // Returns view matrix
    glm::mat4 GetViewMatrix() {
        return glm::lookAt(Position, Target, Up);
    }

// In flyCamera
void FollowPlayerSmooth(const glm::vec3& playerPos, float deltaTime) {
    Target = playerPos; // always look at the player
    // Calculate position around player using Yaw/Pitch
    float x = DistanceToTarget * cos(glm::radians(Pitch)) * cos(glm::radians(Yaw));
    float y = DistanceToTarget * sin(glm::radians(Pitch));
    float z = DistanceToTarget * cos(glm::radians(Pitch)) * sin(glm::radians(Yaw));
    glm::vec3 desiredPos = Target + glm::vec3(x, y, z);

    Position = glm::mix(Position, desiredPos, followSpeed * deltaTime);

    updateCameraVectors();
}

void updateCameraVectorsPublic(){

    updateCameraVectors();

}



    // Direct follow using spherical coordinates
    void FollowPlayer(const glm::vec3& targetPos) {
        Target = targetPos;

        float x = DistanceToTarget * cos(glm::radians(Pitch)) * cos(glm::radians(Yaw));
        float y = DistanceToTarget * sin(glm::radians(Pitch));
        float z = DistanceToTarget * cos(glm::radians(Pitch)) * sin(glm::radians(Yaw));

        Position = Target + glm::vec3(x, y, z);
        updateCameraVectors();
    }
void ProcessMouseMovement(float xpos, float ypos) {
    if(FirstMouse){
        LastX = xpos;
        LastY = ypos;
        FirstMouse = false;
    }

    float xoffset = xpos - LastX;
    float yoffset = LastY - ypos; // reversed

    LastX = xpos;
    LastY = ypos;

    xoffset *= MouseSensitivity;
    yoffset *= MouseSensitivity;

    Yaw   += xoffset;
    Pitch += yoffset;

    if(Pitch > 89.0f) Pitch = 89.0f;
    if(Pitch < -30.0f) Pitch = -30.0f; // limit to prevent going underground

    // No need to update camera vectors here, FollowPlayerSmooth recalculates
}

    // Mouse zoom
    void ProcessMouseScroll(float yoffset) {
        DistanceToTarget -= yoffset;
        if(DistanceToTarget < 2.0f) DistanceToTarget = 2.0f;
        if(DistanceToTarget > 20.0f) DistanceToTarget = 20.0f;

        updateCameraVectors();
    }

    glm::vec3 getPosition() const { return Position; }

private:
    void updateCameraVectors() {
        Front = glm::normalize(Target - Position);
        Right = glm::normalize(glm::cross(Front, WorldUp));
        Up    = glm::normalize(glm::cross(Right, Front));
    }
};
