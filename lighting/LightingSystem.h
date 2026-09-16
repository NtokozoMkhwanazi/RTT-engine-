#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <memory>

#include "lighting/LightData.h"  // GPULightData, kMaxLights, makeGpuLight/makeDirectionalLight
#include "lighting/LightingEnvironment.h"  // #3: fallback sun sourced from an explicit env, not Instance()

enum class LightType {
    DIRECTIONAL,
    POINT,
    SPOT
};

struct Light {
    LightType type;
    glm::vec3 position;
    glm::vec3 direction;
    glm::vec3 color;
    float intensity;
    
    // Point/Spot light properties
    float constant;
    float linear;
    float quadratic;
    
    // Spot light properties
    float cutOff;
    float outerCutOff;
    
    Light(LightType t = LightType::POINT, 
          const glm::vec3& pos = glm::vec3(0.0f),
          const glm::vec3& dir = glm::vec3(0.0f, -1.0f, 0.0f),
          const glm::vec3& col = glm::vec3(1.0f),
          float intens = 1.0f)
        : type(t), position(pos), direction(dir), color(col), intensity(intens),
          constant(1.0f), linear(0.09f), quadratic(0.032f),
          cutOff(cos(glm::radians(12.5f))), outerCutOff(cos(glm::radians(15.0f))) {}
};

class LightingSystem {
public:
    LightingSystem();
    ~LightingSystem() = default;

    // Add a light to the system
    void addLight(const Light& light);

    // Remove a light by index
    void removeLight(size_t index);

    // Update a light by index
    void updateLight(size_t index, const Light& light);

    // Get all lights
    const std::vector<Light>& getLights() const { return lights; }

    // Set ambient light
    void setAmbientLight(const glm::vec3& color, float intensity);

    // Get ambient light
    glm::vec3 getAmbientLight() const { return ambientColor * ambientIntensity; }

    // Update lighting calculations
    void update(float deltaTime);

    // Get directional light (there should typically be only one)
    const Light* getDirectionalLight() const;

    // Get point lights
    const std::vector<Light>& getPointLights() const;

    // Get spot lights
    const std::vector<Light>& getSpotLights() const;

    // Calculate light attenuation
    float calculateAttenuation(float distance, float constant, 
                             float linear, float quadratic) const;

    // --- GPU light buffer (suggestions.txt #1) --------------------------------
    // Packs the registered lights into a flat, std430-aligned GPULightData
    // array capped at `cap` (<= kMaxLights). Empty -> a single directional
    // light sourced from the passed-in `fallbackEnv`'s sun (the editor's key
    // light), so callers always get at least one light to bind.
    // #3: `fallbackEnv` is REQUIRED -- LightingSystem never reads the global
    // LightingEnvironment::Instance() itself.
    std::vector<GPULightData> buildGpuLightData(size_t cap,
                                                const LightingEnvironment& fallbackEnv) const;

    // Set global lighting parameters
    void setGlobalParameters(float ambientIntensity, const glm::vec3& ambientColor);

private:
    std::vector<Light> lights;
    glm::vec3 ambientColor;
    float ambientIntensity;
    
    std::vector<Light> directionalLights;
    std::vector<Light> pointLights;
    std::vector<Light> spotLights;

    void categorizeLights();
};