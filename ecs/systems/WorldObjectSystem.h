#pragma once

/**
 * World Object System - Direct component-based world object management
 * 
 * This system processes WorldObjectComponent, VegetationComponent, and GrassComponent
 * directly for efficient object placement, culling, and rendering.
 */

#include "../ECS.h"
#include "../components/Components.h"
#include <glm/glm.hpp>
#include <vector>
#include <random>

namespace ecs {

/**
 * World Object System - Manages world objects (trees, rocks, props)
 */
class WorldObjectSystem : public TypedSystem<TransformComponent, WorldObjectComponent> {
public:
    WorldObjectSystem() = default;

    void setCameraPosition(const glm::vec3& pos) { m_cameraPosition = pos; }
    glm::vec3 getCameraPosition() const { return m_cameraPosition; }

    void setCullDistance(float distance) { m_cullDistance = distance; }
    float getCullDistance() const { return m_cullDistance; }

    void init() override {
        m_filter = SystemFilter::require<TransformComponent, WorldObjectComponent>();
    }

    void update(float deltaTime) override {
        if (!m_entityManager || !m_componentManager) return;

        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID entityID, TransformComponent& transform, WorldObjectComponent& obj) {
                updateObject(entityID, transform, obj, deltaTime);
            });
    }

    /**
     * Update a single world object
     */
    void updateObject(EntityID entityID, TransformComponent& transform, WorldObjectComponent& obj, float deltaTime) {
        // Calculate distance to camera
        obj.distanceToCamera = glm::distance(transform.position, m_cameraPosition);
        
        // Culling
        obj.isVisible = (obj.distanceToCamera < m_cullDistance);
        obj.needsCulling = false;
        
        // LOD calculation
        obj.lodLevel = calculateLOD(obj.distanceToCamera, obj.lodDistance);
        
        (void)deltaTime;  // Suppress unused warning
    }

    /**
     * Place a world object at a position
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
     * Place multiple objects with randomization
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
    glm::vec3 m_cameraPosition{0.0f};
    float m_cullDistance = 500.0f;

    int calculateLOD(float distance, float lodTransitionDistance) const {
        if (distance < lodTransitionDistance * 0.5f) return 0;
        if (distance < lodTransitionDistance) return 1;
        return 2;
    }
};

/**
 * Vegetation System - Manages vegetation-specific behavior
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
 * Grass System - Manages grass patches
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
