#pragma once

#include "../ECS.h"
#include <glm/glm.hpp>

namespace ecs {

/**
 * Camera Component - Marks entity as a camera
 */
struct CameraComponent : public Component {
    // Projection settings
    float fov = 45.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    float aspectRatio = 16.0f / 9.0f;
    
    // Camera type
    bool isOrthographic = false;
    float orthoSize = 10.0f;
    
    // View settings
    bool isActive = true;
    int viewportX = 0;
    int viewportY = 0;
    int viewportWidth = 800;
    int viewportHeight = 600;
    
    // Culling
    float cullingDistance = 1000.0f;
    
    CameraComponent() = default;
    
    /**
     * Get the projection matrix
     */
    glm::mat4 getProjectionMatrix() const {
        if (isOrthographic) {
            float halfWidth = orthoSize * aspectRatio * 0.5f;
            float halfHeight = orthoSize * 0.5f;
            return glm::ortho(
                -halfWidth, halfWidth,
                -halfHeight, halfHeight,
                nearPlane, farPlane
            );
        } else {
            return glm::perspective(
                glm::radians(fov),
                aspectRatio,
                nearPlane,
                farPlane
            );
        }
    }
    
    /**
     * Set aspect ratio
     */
    void setAspectRatio(float aspect) {
        aspectRatio = aspect;
    }
    
    /**
     * Set viewport
     */
    void setViewport(int x, int y, int width, int height) {
        viewportX = x;
        viewportY = y;
        viewportWidth = width;
        viewportHeight = height;
    }
};

/**
 * Camera Controller Component - Marks entity as controllable camera
 */
struct CameraControllerComponent : public Component {
    // Movement settings
    float moveSpeed = 5.0f;
    float lookSpeed = 0.1f;
    float zoomSpeed = 2.0f;
    
    // Orbit settings (for third-person)
    bool isOrbitCamera = false;
    float orbitDistance = 10.0f;
    float orbitHeight = 3.0f;
    float orbitYaw = 0.0f;
    float orbitPitch = 20.0f;
    
    // Follow settings
    bool followTarget = false;
    Entity target{INVALID_ENTITY_ID};
    glm::vec3 followOffset{0.0f, 5.0f, -10.0f};
    float followSmoothness = 5.0f;
    
    CameraControllerComponent() = default;
};

} // namespace ecs
