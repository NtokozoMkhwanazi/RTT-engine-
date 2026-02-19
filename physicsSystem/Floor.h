#pragma once
#include <glm/glm.hpp>

// ========================================
// Floor / Ground Plane
// ========================================
struct Floor {
    glm::vec3 position{0.0f, 0.0f, 0.0f};  // Center position
    glm::vec3 normal{0.0f, 1.0f, 0.0f};    // Up direction (default Y-up)
    glm::vec2 size{100.0f, 100.0f};        // Half extents in X and Z
    bool enabled{true};
    
    // Visual properties
    glm::vec3 color{0.3f, 0.3f, 0.35f};    // Dark gray-blue
    float gridSpacing{1.0f};               // Grid line spacing
    bool showGrid{true};
    
    // Physics properties
    float friction{0.8f};                  // High friction for ground
    float restitution{0.1f};               // Low bounce
    
    Floor() = default;
    
    Floor(const glm::vec3& pos, const glm::vec2& halfSize = glm::vec2(50.0f, 50.0f))
        : position(pos), size(halfSize) {}
    
    // Check if a point is above the floor (within bounds)
    bool isPointAboveFloor(const glm::vec3& point, float threshold = 0.0f) const {
        if (!enabled) return false;
        
        // Check height
        float heightAboveFloor = glm::dot(point - position, normal);
        if (heightAboveFloor < threshold) return false;
        
        // Check bounds (project point onto floor plane)
        glm::vec3 projected = point - normal * heightAboveFloor;
        glm::vec3 localPos = projected - position;
        
        // Simple AABB check in floor plane
        glm::vec3 right = glm::normalize(glm::cross(normal, glm::vec3(0.0f, 0.0f, 1.0f)));
        glm::vec3 forward = glm::normalize(glm::cross(right, normal));
        
        float x = glm::dot(localPos, right);
        float z = glm::dot(localPos, forward);
        
        return glm::abs(x) <= size.x && glm::abs(z) <= size.y;
    }
    
    // Get height at a given XZ position
    float getHeightAt(const glm::vec3& xzPos) const {
        if (!enabled) return -1000.0f;
        return glm::dot(xzPos - position, normal) + glm::dot(position, normal);
    }
    
    // Raycast against floor
    bool raycast(const glm::vec3& origin, const glm::vec3& direction,
                 glm::vec3& outHitPoint, float maxDist = 1000.0f) const {
        if (!enabled) return false;
        
        float denom = glm::dot(direction, normal);
        if (glm::abs(denom) < 0.0001f) return false;  // Parallel to floor
        
        float t = glm::dot(position - origin, normal) / denom;
        if (t < 0.0f || t > maxDist) return false;
        
        outHitPoint = origin + direction * t;
        
        // Check bounds
        glm::vec3 localPos = outHitPoint - position;
        glm::vec3 right = glm::normalize(glm::cross(normal, glm::vec3(0.0f, 0.0f, 1.0f)));
        glm::vec3 forward = glm::normalize(glm::cross(right, normal));
        
        float x = glm::dot(localPos, right);
        float z = glm::dot(localPos, forward);
        
        return glm::abs(x) <= size.x && glm::abs(z) <= size.y;
    }
};
