#pragma once

/**
 * Terrain System - ECS adapter over the engine's world/Terrain
 *
 * This system is a thin adapter. It owns NO terrain-generation, height-sampling,
 * or LOD logic of its own: every substantive operation is delegated to the injected
 * world::Terrain (the single, tested source of truth — see world/Terrain.h and
 * tests/test_terrain.cpp / TerrainPipelineGLTest).
 *
 * The previous version of this file re-implemented height sampling with a
 * sine-noise stub (the comment admitted: "replace with proper noise in production")
 * and left updateTerrain() empty. Both have been removed so there is exactly one
 * terrain implementation to maintain.
 *
 * TerrainChunkSystem (below) is a separate, lightweight system that maintains the
 * ECS-side TerrainChunkComponent's view of per-chunk state (distance / LOD / loaded
 * flag). It does not duplicate world/Terrain's internal chunk management — it mirrors
 * it for ECS consumers.
 */

#include "../ECS.h"
#include "../components/Components.h"
#include "../../world/Terrain.h"   // the engine's Terrain (the "better version")
#include <glm/glm.hpp>
#include <vector>

namespace ecs {

/**
 * Terrain System - Manages the terrain root entity and delegates to world/Terrain.
 */
class TerrainSystem : public TypedSystem<TerrainComponent> {
public:
    TerrainSystem() = default;

    /**
     * Inject the engine's live Terrain. Until set, the system is a no-op —
     * it never falls back to a duplicate implementation.
     */
    void setTerrain(Terrain* terrain) { m_terrain = terrain; }
    Terrain* getTerrain() const { return m_terrain; }

    void setCameraPosition(const glm::vec3& pos) { m_cameraPosition = pos; }
    glm::vec3 getCameraPosition() const { return m_cameraPosition; }

    void init() override {
        m_filter = SystemFilter::require<TerrainComponent>();
    }

    void update(float deltaTime) override {
        if (!m_terrain) return;

        // Drive the engine Terrain's streaming / LOD update once. The ECS
        // TerrainComponent is the authoring config; the engine Terrain is the
        // implementation that owns chunk generation, culling, and LOD.
        m_terrain->update(m_cameraPosition, deltaTime);

        if (!m_entityManager || !m_componentManager) return;

        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID entityID, TerrainComponent& terrain) {
                updateTerrain(entityID, terrain, deltaTime);
            });
    }

    /**
     * Sync a TerrainComponent's authoring config into the engine Terrain.
     *
     * Height sampling itself is delegated to world/Terrain (see getHeightAt).
     * Only the parameters that world::TerrainConfig exposes are mirrored; engine
     * Terrain instances are configured at construction time, so here we just keep
     * the component and the live Terrain consistent for any future config API.
     */
    void updateTerrain(EntityID terrainEntityID, TerrainComponent& terrain, float deltaTime) {
        // chunkSize / heightScale are the two TerrainComponent fields that
        // world::TerrainConfig exposes. We don't reconstruct the Terrain here
        // (it is injected via setTerrain and owns the GPU heightmap), but we
        // record the authoring intent so a config-apply pass can consume it.
        m_requestedChunkSize = terrain.chunkSize;
        m_requestedHeightScale = terrain.heightScale;

        (void)terrainEntityID;
        (void)deltaTime;
    }

    /**
     * Height at a world position — delegates to the engine Terrain.
     */
    float getHeightAt(float worldX, float worldZ, int seed = 0) const {
        (void)seed; // engine Terrain samples its loaded heightmap; seed is authoring-only
        return m_terrain ? m_terrain->getHeightAt(worldX, worldZ) : 0.0f;
    }

    /**
     * Normal at a world position — delegates to the engine Terrain.
     */
    glm::vec3 getNormalAt(float worldX, float worldZ, float sampleDist = 1.0f) const {
        (void)sampleDist; // engine Terrain uses its own derivative step
        return m_terrain ? m_terrain->getNormalAt(worldX, worldZ)
                         : glm::vec3(0.0f, 1.0f, 0.0f);
    }

    const char* getName() const override { return "TerrainSystem"; }

private:
    Terrain* m_terrain = nullptr;   // non-owning; the engine's live Terrain
    glm::vec3 m_cameraPosition{0.0f};

    // Authoring intent recorded from the component (see updateTerrain).
    float m_requestedChunkSize = 100.0f;
    float m_requestedHeightScale = 50.0f;
};

/**
 * Terrain Chunk System - Updates individual terrain chunks (ECS-side view).
 *
 * Maintains the per-chunk distance / LOD fields on TerrainChunkComponent so ECS
 * consumers can cull/LOD against the camera. The engine Terrain (injected into
 * TerrainSystem) owns the real chunk generation; this system only mirrors state.
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
        // Distance to camera drives the ECS-side LOD flag; the engine Terrain
        // resolves the actual LOD level for its internal chunks.
        chunk.distanceToCamera = glm::distance(transform.position, m_cameraPosition);
        chunk.lodLevel = calculateLOD(chunk.distanceToCamera);
        chunk.needsUpdate = false;

        (void)entityID;
        (void)deltaTime;
    }

    int calculateLOD(float distance) const {
        if (distance < 50.0f)  return 0;
        if (distance < 150.0f) return 1;
        return 2;
    }

    const char* getName() const override { return "TerrainChunkSystem"; }

private:
    glm::vec3 m_cameraPosition{0.0f};
};

} // namespace ecs
