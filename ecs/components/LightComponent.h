#pragma once

#include "../ECS.h"
#include <glm/glm.hpp>
#include <string>
#include <cmath>

namespace ecs {

/**
 * Light Types
 */
enum class LightType {
    DIRECTIONAL,
    POINT,
    SPOT,
    AREA
};

/**
 * Light Component - Light source for rendering
 */
struct LightComponent : public Component {
    LightType type = LightType::POINT;
    
    // Color and intensity
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float temperature = 6500.0f;  // Kelvin
    
    // Range and attenuation
    float range = 10.0f;
    float constant = 1.0f;
    float linear = 0.09f;
    float quadratic = 0.032f;
    
    // Spot light properties
    float spotInnerAngle = glm::radians(12.5f);
    float spotOuterAngle = glm::radians(17.5f);
    
    // Shadow properties
    bool castShadows = true;
    int shadowMapSize = 1024;
    float shadowBias = 0.005f;
    float shadowNear = 0.1f;
    float shadowFar = 50.0f;
    
    // Light flags
    bool enabled = true;
    bool useTemperature = false;
    
    LightComponent() = default;
    LightComponent(LightType t, const glm::vec3& c, float i)
        : type(t), color(c), intensity(i) {}
    
    /**
     * Set as directional light
     */
    void setDirectional(const glm::vec3& col = glm::vec3(1.0f), float intens = 1.0f) {
        type = LightType::DIRECTIONAL;
        color = col;
        intensity = intens;
    }
    
    /**
     * Set as point light
     */
    void setPoint(const glm::vec3& col = glm::vec3(1.0f), float intens = 1.0f, float r = 10.0f) {
        type = LightType::POINT;
        color = col;
        intensity = intens;
        range = r;
    }
    
    /**
     * Set as spot light
     */
    void setSpot(const glm::vec3& col = glm::vec3(1.0f), float intens = 1.0f, 
                 float r = 10.0f, float innerAngle = 12.5f, float outerAngle = 17.5f) {
        type = LightType::SPOT;
        color = col;
        intensity = intens;
        range = r;
        spotInnerAngle = glm::radians(innerAngle);
        spotOuterAngle = glm::radians(outerAngle);
    }
    
    /**
     * Get color adjusted by temperature
     */
    glm::vec3 getAdjustedColor() const {
        if (!useTemperature) return color;
        // Simple temperature to color conversion (Planckian approximation)
        float t = temperature / 100.0f;
        glm::vec3 tempColor;
        tempColor.r = (t <= 66.0f) ? 1.0f : 1.294f * std::pow(t - 60.0f, -0.133f);
        tempColor.g = (t <= 66.0f) ? 0.948f * std::pow(t, 0.044f) : 1.129f * std::pow(t - 60.0f, -0.075f);
        tempColor.b = (t >= 66.0f) ? 1.0f : (t <= 19.0f) ? 0.0f : 0.543f * std::pow(t - 10.0f, 0.5f);
        return color * glm::clamp(tempColor, 0.0f, 1.0f);
    }
};

/**
 * Ambient Light Component - Global ambient lighting
 */
struct AmbientLightComponent : public Component {
    glm::vec3 color{0.1f};
    float intensity = 1.0f;
    glm::vec3 skyColor{0.5f};
    glm::vec3 groundColor{0.2f};
    
    AmbientLightComponent() = default;
    AmbientLightComponent(const glm::vec3& col, float intens)
        : color(col), intensity(intens) {}
};

} // namespace ecs
