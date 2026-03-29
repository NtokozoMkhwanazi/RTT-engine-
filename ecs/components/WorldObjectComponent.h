#pragma once

#include "../ECS.h"
#include <glm/glm.hpp>
#include <string>

namespace ecs {

/**
 * World Object Type
 */
enum class WorldObjectType {
    TREE,
    ROCK,
    GRASS,
    BUILDING,
    PROP,
    CUSTOM
};

/**
 * World Object Component - Marks entity as a world object
 */
struct WorldObjectComponent : public Component {
    WorldObjectType type = WorldObjectType::PROP;
    int meshID = -1;
    int materialID = -1;
    
    // LOD
    int lodLevel = 0;
    float lodDistance = 50.0f;
    
    // Streaming
    float distanceToCamera = 0.0f;
    bool isVisible = true;
    bool needsCulling = true;
    
    // Object properties
    float scaleVariation = 0.2f;
    float rotationVariation = 360.0f;
    
    WorldObjectComponent() = default;
    WorldObjectComponent(WorldObjectType t, int mesh) : type(t), meshID(mesh) {}
};

/**
 * Vegetation Component - Special properties for vegetation
 */
struct VegetationComponent : public Component {
    // Wind animation
    float windStrength = 1.0f;
    float windFrequency = 1.0f;
    
    // Seasonal variation
    float seasonalColorShift = 0.0f;
    float leafDensity = 1.0f;
    
    // Billboard settings
    float billboardDistance = 50.0f;
    
    VegetationComponent() = default;
};

/**
 * Grass Component - Special properties for grass
 */
struct GrassComponent : public Component {
    int instancesPerPatch = 100;
    float patchRadius = 5.0f;

    // Grass properties
    float height = 0.3f;
    float bendStrength = 0.5f;
    
    // Wind animation
    float windFrequency = 2.0f;
    float windStrength = 1.0f;

    GrassComponent() = default;
};

} // namespace ecs
