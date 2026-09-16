#pragma once

/**
 * World Object System - ECS adapter over the engine's world/WorldObjectManager
 *
 * This system is a thin ECS adapter. It creates/manages ECS entities that carry
 * WorldObjectComponent, but it does NOT re-implement placement, culling, or LOD:
 * all of that is delegated to the injected world::WorldObjectManager (the tested
 * source of truth — see world/WorldObjectManager.h and test_world_object_physics /
 * test_world_object_culling).
 *
 * The previous version of this file re-implemented placeObject/LOD/culling inline
 * and never included world/WorldObjectManager.h. That duplicated logic has been
 * removed so there is exactly one object-management implementation to maintain.
 */

#include "../ECS.h"
#include "../components/Components.h"
#include "../../world/WorldObjectManager.h"   // the engine's object manager (the "better version")
#include <glm/glm.hpp>
#include <vector>
#include <random>

namespace ecs {

/**
 * World Object System - Manages world object entities (trees, rocks, props).
 *
 * Authoring (creating entities) is ECS-side; the runtime culling/LOD/rendering
 * pass is delegated to the injected WorldObjectManager.
 */
class WorldObjectSystem : public TypedSystem<TransformComponent, WorldObjectComponent> {
public:
    WorldObjectSystem() = default;

    /** Inject the engine's live WorldObjectManager. Until set, update() is a no-op. */
    void setWorldObjectManager(WorldObjectManager* mgr) { m_mgr = mgr; }
    WorldObjectManager* getWorldObjectManager() const { return m_mgr; }

    void setCameraPosition(const glm::vec3& pos) { m_cameraPosition = pos; }
    glm::vec3 getCameraPosition() const { return m_cameraPosition; }

    void setCullDistance(float distance) { m_cullDistance = distance; }
    float getCullDistance() const { return m_cullDistance; }

    void init() override {
        m_filter = SystemFilter::require<TransformComponent, WorldObjectComponent>();
    }

    void update(float deltaTime) override {
        if (!m_entityManager || !m_componentManager) return;

        // Let the engine WorldObjectManager run its authoritative culling/LOD
        // pass over the objects it owns. (The ECS entities authored below are a
        // parallel representation for ECS consumers; the manager is the runtime.)
        if (m_mgr) {
            m_mgr->update(m_cameraPosition, deltaTime);
        }

        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID entityID, TransformComponent& transform, WorldObjectComponent& obj) {
                updateObject(entityID, transform, obj, deltaTime);
            });
    }

    /**
     * Update a single world object's ECS-side bookkeeping (distance / visibility).
     * The authoritative culling/LOD is performed by WorldObjectManager::update above;
     * here we only populate the component fields ECS consumers read.
     */
    void updateObject(EntityID entityID, TransformComponent& transform, WorldObjectComponent& obj, float deltaTime) {
        obj.distanceToCamera = glm::distance(transform.position, m_cameraPosition);

        // Visibility uses the system cull distance (the manager's cull distance
        // is configured separately on the WorldObjectManager itself).
        obj.isVisible = (obj.distanceToCamera < m_cullDistance);
        obj.needsCulling = false;

        (void)entityID;
        (void)deltaTime;
    }

    /**
     * Place a world object at a position (ECS entity authoring).
     *
     * Note: this creates an ECS entity + components only. Registering the object
     * in the engine WorldObjectManager is a separate step (the manager's placeObject
     * takes a world::WorldObjectType + modelPath, which is a different authoring
     * model; map between them at the call site when wiring this system up).
     */
    Entity placeObject(World& world, WorldObjectType type, int meshID, const glm::vec3& position, float rotation = 0.0f, float scale = 1.0f) {
        Entity entity = world.createEntity();

        auto& transform = world.addComponent<TransformComponent>(entity);
        transform.position = position;
        transform.rotation = glm::angleAxis(glm::radians(rotation), glm::vec3(0.0f, 1.0f, 0.0f));
        transform.scale = glm::vec3(scale);

        auto& obj = world.addComponent<WorldObjectComponent>(entity);
        obj.type = type;
        obj.meshID = meshID;

        return entity;
    }

    /**
     * Place multiple objects with randomization (ECS entity authoring).
     */
    std::vector<Entity> placeObjectsRandom(
        World& world,
        WorldObjectType type,
        int meshID,
        const std::vector<glm::vec3>& positions,
        float minScale = 0.8f,
        float maxScale = 1.2f)
    {
        std::vector<Entity> entities;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<float> scaleDist(minScale, maxScale);
        std::uniform_real_distribution<float> rotDist(0.0f, 360.0f);

        for (const auto& pos : positions) {
            Entity entity = placeObject(
                world, type, meshID, pos,
                rotDist(gen), scaleDist(gen)
            );
            entities.push_back(entity);
        }

        return entities;
    }

    const char* getName() const override { return "WorldObjectSystem"; }

private:
    WorldObjectManager* m_mgr = nullptr;   // non-owning; the engine's live object manager
    glm::vec3 m_cameraPosition{0.0f};
    float m_cullDistance = 500.0f;
};

/**
 * Vegetation System - Manages vegetation-specific behavior.
 */
class VegetationSystem : public TypedSystem<TransformComponent, VegetationComponent> {
public:
    VegetationSystem() = default;

    void setWindParameters(float strength, float frequency) {
        m_windStrength = strength;
        m_windFrequency = frequency;
    }

    void init() override {
        m_filter = SystemFilter::require<TransformComponent, VegetationComponent>();
    }

    void update(float deltaTime) override {
        if (!m_entityManager || !m_componentManager) return;

        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID, TransformComponent&, VegetationComponent& veg) {
                // Update vegetation wind animation state
                // Wind animation is typically done in vertex shader
                // This updates state that shaders can use
                veg.windStrength = m_windStrength;
                veg.windFrequency = m_windFrequency;
                (void)deltaTime;
            });
    }

    const char* getName() const override { return "VegetationSystem"; }

private:
    float m_windStrength = 1.0f;
    float m_windFrequency = 1.0f;
};

/**
 * Grass System - Manages grass patches.
 */
class GrassSystem : public TypedSystem<TransformComponent, GrassComponent> {
public:
    GrassSystem() = default;

    void init() override {
        m_filter = SystemFilter::require<TransformComponent, GrassComponent>();
    }

    void update(float deltaTime) override {
        if (!m_entityManager || !m_componentManager) return;

        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID, TransformComponent&, GrassComponent& grass) {
                // Update grass wind animation state
                // Grass wind is typically animated in vertex/geometry shader
                grass.windFrequency = m_windFrequency;
                (void)deltaTime;
            });
    }

    const char* getName() const override { return "GrassSystem"; }

private:
    float m_windFrequency = 2.0f;
};

} // namespace ecs
