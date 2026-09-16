#include "LightingSystem.h"
#include "lighting/LightData.h"
#include "lighting/LightingEnvironment.h" // for the sun fallback
#include <algorithm>

LightingSystem::LightingSystem() 
    : ambientColor(0.1f, 0.1f, 0.1f), ambientIntensity(1.0f) {
}

void LightingSystem::addLight(const Light& light) {
    lights.push_back(light);
    categorizeLights();
}

void LightingSystem::removeLight(size_t index) {
    if (index < lights.size()) {
        lights.erase(lights.begin() + index);
        categorizeLights();
    }
}

void LightingSystem::updateLight(size_t index, const Light& light) {
    if (index < lights.size()) {
        lights[index] = light;
        categorizeLights();
    }
}

void LightingSystem::setAmbientLight(const glm::vec3& color, float intensity) {
    ambientColor = color;
    ambientIntensity = intensity;
}

void LightingSystem::update(float deltaTime) {
    // Update any animated lights here
    // For now, just maintain the light positions and properties
    (void)deltaTime; // Unused parameter
}

const Light* LightingSystem::getDirectionalLight() const {
    for (const auto& light : directionalLights) {
        return &light;
    }
    return nullptr;
}

const std::vector<Light>& LightingSystem::getPointLights() const {
    return pointLights;
}

const std::vector<Light>& LightingSystem::getSpotLights() const {
    return spotLights;
}

float LightingSystem::calculateAttenuation(float distance, float constant, 
                                        float linear, float quadratic) const {
    return 1.0f / (constant + linear * distance + quadratic * (distance * distance));
}

void LightingSystem::setGlobalParameters(float ambientIntensity, const glm::vec3& ambientColor) {
    this->ambientIntensity = ambientIntensity;
    this->ambientColor = ambientColor;
}

void LightingSystem::categorizeLights() {
    directionalLights.clear();
    pointLights.clear();
    spotLights.clear();

    for (const auto& light : lights) {
        switch (light.type) {
            case LightType::DIRECTIONAL:
                directionalLights.push_back(light);
                break;
            case LightType::POINT:
                pointLights.push_back(light);
                break;
            case LightType::SPOT:
                spotLights.push_back(light);
                break;
        }
    }
}

std::vector<GPULightData> LightingSystem::buildGpuLightData(
        size_t cap, const LightingEnvironment& fallbackEnv) const {
    const size_t limit = (cap == 0) ? kMaxGpuLights : std::min(cap, (size_t)kMaxGpuLights);

    std::vector<GPULightData> out;
    out.reserve(limit);

    if (lights.empty()) {
        // Editor key-light fallback: a single directional sun from the env
        // passed IN by the caller (#3: no LightingEnvironment::Instance() here).
        out.push_back(makeDirectionalLight(fallbackEnv.sunDirection,
                                           fallbackEnv.sunColor,
                                           fallbackEnv.sunIntensity));
        return out;
    }

    for (const Light& l : lights) {
        if (out.size() >= limit) break;
        uint32_t t = static_cast<uint32_t>(l.type);
        out.push_back(makeGpuLight(t, l.position, l.direction, l.color, l.intensity,
                                   l.constant, l.linear, l.quadratic,
                                   l.cutOff, l.outerCutOff));
    }
    return out;
}