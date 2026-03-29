#pragma once

#include "../ECS.h"
#include <glm/glm.hpp>

namespace ecs {

/**
 * Terrain Chunk Component - Marks entity as a terrain chunk
 */
struct TerrainChunkComponent : public Component {
    int chunkX = 0;
    int chunkZ = 0;
    int lodLevel = 0;
    
    // Mesh references
    int meshID = -1;
    
    // Terrain data
    float minHeight = 0.0f;
    float maxHeight = 0.0f;
    
    // Streaming state
    bool isLoaded = false;
    bool needsUpdate = true;
    float distanceToCamera = 0.0f;
    
    TerrainChunkComponent() = default;
    TerrainChunkComponent(int x, int z) : chunkX(x), chunkZ(z) {}
};

/**
 * Terrain Component - Marks entity as terrain root
 */
struct TerrainComponent : public Component {
    int chunksPerSide = 16;
    float chunkSize = 100.0f;
    float maxLODDistance = 100.0f;
    float lodTransitionDistance = 50.0f;
    
    // Terrain generation parameters
    int seed = 0;
    float heightScale = 50.0f;
    float noiseScale = 0.01f;
    
    TerrainComponent() = default;
};

} // namespace ecs
