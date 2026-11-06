#pragma once

/**
 * Terrain System - Direct component-based terrain management
 * 
 * This system processes TerrainComponent and TerrainChunkComponent directly
 * without wrapping legacy terrain classes.
 */

#include "../ECS.h"
#include "../components/Components.h"
#include <glm/glm.hpp>
#include <vector>
#include <cmath>

namespace ecs {

/**
 * Terrain System - Manages terrain chunks and streaming
 */
class TerrainSystem : public TypedSystem<TerrainComponent> {
public:
    TerrainSystem() = default;

    void setCameraPosition(const glm::vec3& pos) { m_cameraPosition = pos; }
    glm::vec3 getCameraPosition() const { return m_cameraPosition; }

    void init() override {
        m_filter = SystemFilter::require<TerrainComponent>();
    }

    void update(float deltaTime) override {
        if (!m_entityManager || !m_componentManager) return;

        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID entityID, TerrainComponent& terrain) {
                updateTerrain(entityID, terrain, deltaTime);
            });
    }

    /**
     * Update terrain streaming and LOD
     */
    void updateTerrain(EntityID terrainEntityID, TerrainComponent& terrain, float deltaTime) {
        // Find all chunk entities for this terrain
        // In a real implementation, we'd track chunk entities in the terrain component
        (void)terrainEntityID;  // Suppress unused warning
        (void)deltaTime;
    }

    /**
     * Generate height at a world position
     */
    float getHeightAt(float worldX, float worldZ, int seed = 0) const {
        // Simple noise-based height generation
        float x = worldX * 0.01f + seed;
        float z = worldZ * 0.01f + seed;
        
        // Combine multiple octaves of noise
        float height = 0.0f;
        float amplitude = 1.0f;
        float frequency = 1.0f;
        
        for (int i = 0; i < 4; i++) {
            height += sineNoise(x * frequency, z * frequency) * amplitude;
            amplitude *= 0.5f;
            frequency *= 2.0f;
        }
        
        return height * 50.0f;  // Scale to world units
    }

    /**
     * Get normal at a world position
     */
    glm::vec3 getNormalAt(float worldX, float worldZ, float sampleDist = 1.0f) const {
        float hL = getHeightAt(worldX - sampleDist, worldZ);
        float hR = getHeightAt(worldX + sampleDist, worldZ);
        float hD = getHeightAt(worldX, worldZ - sampleDist);
        float hU = getHeightAt(worldX, worldZ + sampleDist);
        
        glm::vec3 normal;
        normal.x = hL - hR;
        normal.y = 2.0f * sampleDist;
        normal.z = hD - hU;
        
        return glm::normalize(normal);
    }

    /**
     * Calculate LOD level based on distance
     */
    int calculateLOD(float distance, float maxLODDistance, float transitionDistance) const {
        if (distance < transitionDistance) return 0;
        if (distance < maxLODDistance) return 1;
        return 2;
    }

    const char* getName() const override { return "TerrainSystem"; }

private:
    glm::vec3 m_cameraPosition{0.0f};

    /**
     * Simple sine-based noise function (replace with proper noise in production)
     */
    float sineNoise(float x, float z) const {
        return std::sin(x) * std::cos(z);
    }
};

/**
 * Terrain Chunk System - Updates individual terrain chunks
 */
class TerrainChunkSystem : public TypedSystem<TransformComponent, TerrainChunkComponent> {
public:
    TerrainChunkSystem() = default;

    void setCameraPosition(const glm::vec3& pos) { m_cameraPosition = pos; }

    void init() override {
        m_filter = SystemFilter::require<TransformComponent, TerrainChunkComponent>();
    }

    void update(float deltaTime) override {
        if (!m_entityManager || !m_componentManager) return;

        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID entityID, TransformComponent& transform, TerrainChunkComponent& chunk) {
                updateChunk(entityID, transform, chunk, deltaTime);
            });
    }

    void updateChunk(EntityID entityID, TransformComponent& transform, TerrainChunkComponent& chunk, float deltaTime) {
        // Calculate distance to camera
        chunk.distanceToCamera = glm::distance(transform.position, m_cameraPosition);
        
        // Update LOD based on distance
        int newLOD = calculateLOD(chunk.distanceToCamera);
        
        // Mark for update if LOD changed
        chunk.needsUpdate = (chunk.lodLevel != newLOD);
        chunk.lodLevel = newLOD;
        
        (void)entityID;
        (void)deltaTime;
    }

    const char* getName() const override { return "TerrainChunkSystem"; }

private:
    glm::vec3 m_cameraPosition{0.0f};

    int calculateLOD(float distance) const {
        if (distance < 50.0f) return 0;
        if (distance < 150.0f) return 1;
        return 2;
    }
};

} // namespace ecs
