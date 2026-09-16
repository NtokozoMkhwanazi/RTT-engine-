#pragma once

#include "../ECS.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ecs {

/**
 * Transform Component - Position, rotation, and scale in 3D space
 */
struct TransformComponent : public Component {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    
    // Cached model matrix (dirty flag for lazy evaluation)
    mutable glm::mat4 cachedModelMatrix{1.0f};
    mutable bool isDirty = true;
    
    TransformComponent() = default;
    TransformComponent(const glm::vec3& pos) : position(pos) {}
    TransformComponent(const glm::vec3& pos, const glm::quat& rot) 
        : position(pos), rotation(rot) {}
    TransformComponent(const glm::vec3& pos, const glm::quat& rot, const glm::vec3& s)
        : position(pos), rotation(rot), scale(s) {}
    
    /**
     * Get the model matrix (cached)
     */
    glm::mat4 getModelMatrix() const {
        if (isDirty) {
            cachedModelMatrix = calculateModelMatrix();
            isDirty = false;
        }
        return cachedModelMatrix;
    }
    
    /**
     * Calculate the model matrix
     */
    glm::mat4 calculateModelMatrix() const {
        glm::mat4 matrix = glm::mat4(1.0f);
        matrix = glm::translate(matrix, position);
        matrix *= glm::mat4_cast(rotation);
        matrix = glm::scale(matrix, scale);
        return matrix;
    }
    
    /**
     * Mark the transform as dirty (needs recalculation)
     */
    void markDirty() const {
        isDirty = true;
    }
    
    /**
     * Translate the position
     */
    void translate(const glm::vec3& delta) {
        position += delta;
        markDirty();
    }
    
    /**
     * Rotate around an axis
     */
    void rotate(float angle, const glm::vec3& axis) {
        rotation = glm::angleAxis(angle, axis) * rotation;
        markDirty();
    }
    
    /**
     * Set rotation from Euler angles (in radians)
     */
    void setEulerAngles(const glm::vec3& euler) {
        rotation = glm::quat(euler);
        markDirty();
    }
    
    /**
     * Get forward direction
     */
    glm::vec3 getForward() const {
        return rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    }
    
    /**
     * Get right direction
     */
    glm::vec3 getRight() const {
        return rotation * glm::vec3(1.0f, 0.0f, 0.0f);
    }
    
    /**
     * Get up direction
     */
    glm::vec3 getUp() const {
        return rotation * glm::vec3(0.0f, 1.0f, 0.0f);
    }
    
    /**
     * Look at a target position
     */
    void lookAt(const glm::vec3& target, const glm::vec3& up = glm::vec3(0.0f, 1.0f, 0.0f)) {
        glm::vec3 direction = glm::normalize(target - position);
        glm::mat4 viewMatrix = glm::lookAt(position, position + direction, up);
        glm::mat4 rotatedView = glm::mat4_cast(rotation) * viewMatrix;
        rotation = glm::quat_cast(glm::inverse(rotatedView));
        markDirty();
    }
};

} // namespace ecs
