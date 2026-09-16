#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>

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

    // Camera options (order matches constructor initialization list)
    float FieldOfView;
    float Zoom;
    float MouseSensitivity;
    float DistanceToTarget;
    float MinDistance;
    float MaxDistance;

    // Mouse state
    float LastX, LastY;
    bool FirstMouse;

    // Smooth follow
    float followSpeed = 5.0f;
    float rotationSpeed = 2.0f;

    // Camera shake
    struct ShakeEffect {
        float intensity = 0.0f;
        float duration = 0.0f;
        float decayRate = 1.0f;
        glm::vec3 direction = glm::vec3(1.0f);
    };
    ShakeEffect currentShake;

    // Constructor
    flyCamera(glm::vec3 startPos = glm::vec3(0.0f, 2.0f, 10.0f),
              glm::vec3 up = glm::vec3(0.0f,1.0f,0.0f),
              float yaw = -90.0f,
              float pitch = 20.0f,
              float distance = 8.0f)
        : WorldUp(up), Yaw(yaw), Pitch(pitch),
          FieldOfView(60.0f), Zoom(60.0f),
          MouseSensitivity(0.2f),
          DistanceToTarget(distance), MinDistance(1.0f), MaxDistance(50.0f)
    {
        Position = startPos;
        Target = glm::vec3(0.0f);
        FirstMouse = true;
        LastX = 400; LastY = 300;
        
        // Calculate orbit position from Yaw, Pitch, and DistanceToTarget
        // This positions the camera correctly for orbit camera behavior
        updateCameraVectors();
    }

    // Get view matrix
    glm::mat4 GetViewMatrix() {
        return glm::lookAt(Position, Target, Up);
    }

    // Get projection matrix
    glm::mat4 GetProjectionMatrix(float aspectRatio) {
        return glm::perspective(glm::radians(FieldOfView), aspectRatio, 0.1f, 1000.0f);
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

        Yaw += xoffset * rotationSpeed;
        Pitch += yoffset * rotationSpeed;

        // Clamp pitch
        if (Pitch > 89.0f) Pitch = 89.0f;
        if (Pitch < -89.0f) Pitch = -89.0f;

        updateCameraVectors();
    }

    // Scroll zoom (applies immediately by repositioning around the target)
    void ProcessMouseScroll(float yoffset) {
        DistanceToTarget -= yoffset;
        if (DistanceToTarget < MinDistance) DistanceToTarget = MinDistance;
        if (DistanceToTarget > MaxDistance) DistanceToTarget = MaxDistance;
        updateCameraVectors();
    }

    // Camera shake effect
    void AddShake(float intensity, float duration, const glm::vec3& direction = glm::vec3(1.0f)) {
        currentShake.intensity = intensity;
        currentShake.duration = duration;
        currentShake.decayRate = intensity / duration;
        currentShake.direction = direction;
    }

    // Update camera shake
    void UpdateShake(float deltaTime) {
        if (currentShake.duration > 0.0f) {
            currentShake.duration -= deltaTime;
            if (currentShake.duration <= 0.0f) {
                currentShake.intensity = 0.0f;
            } else {
                currentShake.intensity -= currentShake.decayRate * deltaTime;
            }
            
            // Apply shake offset
            if (currentShake.intensity > 0.0f) {
                // Generate a pseudo-random offset based on time
                float time = glfwGetTime();
                glm::vec3 shakeOffset;
                shakeOffset.x = (sin(time * 100.0f) * 0.5f - 0.25f) * currentShake.intensity * currentShake.direction.x;
                shakeOffset.y = (cos(time * 123.0f) * 0.5f - 0.25f) * currentShake.intensity * currentShake.direction.y;
                shakeOffset.z = (sin(time * 147.0f) * 0.5f - 0.25f) * currentShake.intensity * currentShake.direction.z;
                
                Position += shakeOffset;
                Target += shakeOffset;
            }
        }
    }

    // Set field of view
    void SetFieldOfView(float fov) {
        FieldOfView = glm::clamp(fov, 1.0f, 120.0f);
    }

    // Cinematic camera modes
    enum class CameraMode {
        FREE_LOOK,
        THIRD_PERSON,
        FIRST_PERSON,
        ORBITAL
    };

    void SetCameraMode(CameraMode mode) {
        cameraMode = mode;
    }

    // Collision detection for camera
    void SetCollisionEnabled(bool enabled) {
        collisionEnabled = enabled;
    }

    void SetCollisionDistance(float distance) {
        collisionDistance = distance;
    }

    // Update camera with collision detection
    void UpdateWithCollision(const std::vector<glm::vec3>& collisionPoints) {
        if (!collisionEnabled) return;

        // Check for collisions with nearby objects
        for (const auto& point : collisionPoints) {
            float dist = glm::distance(Position, point);
            if (dist < collisionDistance) {
                // Move camera away from collision point
                glm::vec3 direction = glm::normalize(Position - point);
                Position = point + direction * collisionDistance;
            }
        }

        updateCameraVectors();
    }

    // Reposition the orbit camera around its current target (Orbit/ThirdPerson
    // modes). Keeps yaw/pitch/distance and heals a degenerate WorldUp.
    void RepositionOrbit() {
        if (glm::length(WorldUp) < 1e-6f) WorldUp = glm::vec3(0.0f, 1.0f, 0.0f);
        updateCameraVectors();
    }

    // Mouse rotation - takes absolute positions from GLFW (PUBLIC)
    void ProcessMouseMovementAbsolute(float xpos, float ypos) {
        if (FirstMouse) { LastX = xpos; LastY = ypos; FirstMouse = false; }

        float xoffset = xpos - LastX;
        float yoffset = LastY - ypos; // reversed
        LastX = xpos;
        LastY = ypos;

        xoffset *= MouseSensitivity;
        yoffset *= MouseSensitivity;

        Yaw += xoffset * rotationSpeed;
        Pitch += yoffset * rotationSpeed;

        // Clamp pitch
        if (Pitch > 89.0f) Pitch = 89.0f;
        if (Pitch < -89.0f) Pitch = -89.0f;

        updateCameraVectors();
    }

private:
    CameraMode cameraMode = CameraMode::FIRST_PERSON;
    bool collisionEnabled = false;
    float collisionDistance = 0.5f;

    void updateCameraVectors() {
        // Heal a degenerate WorldUp (the NaN-poisoning bug): fall back to +Y.
        if (glm::length(WorldUp) < 1e-6f) WorldUp = glm::vec3(0.0f, 1.0f, 0.0f);

        // Calculate new position based on Yaw, Pitch, and DistanceToTarget
        // Yaw=-90 means looking down +Z, Pitch=0 means level
        float cosPitch = cos(glm::radians(Pitch));
        float sinPitch = sin(glm::radians(Pitch));
        float cosYaw = cos(glm::radians(Yaw));
        float sinYaw = sin(glm::radians(Yaw));

        glm::vec3 offset;
        offset.x = DistanceToTarget * cosYaw * cosPitch;
        offset.y = DistanceToTarget * sinPitch;
        offset.z = DistanceToTarget * sinYaw * cosPitch;

        Position = Target - offset;

        // Calculate right and up vectors (guard against NaN from parallel cross product).
        glm::vec3 front = Target - Position;
        if (glm::length(front) < 1e-6f) front = glm::vec3(0.0f, 0.0f, -1.0f);
        front = glm::normalize(front);
        if (glm::abs(glm::dot(front, WorldUp)) > 0.999f)
            Right = glm::normalize(glm::cross(front, glm::vec3(1.0f, 0.0f, 0.0f)));
        else
            Right = glm::normalize(glm::cross(front, WorldUp));
        Up = glm::normalize(glm::cross(Right, front));
    }
};

