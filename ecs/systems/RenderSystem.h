#pragma once

#include "../ECS.h"
#include "../components/Components.h"
#include <glm/glm.hpp>

// Forward declarations - include actual headers in .cpp if needed
class ModelManager;
class Terrain;
class WorldObjectManager;

namespace ecs {

/**
 * Render System - Renders all entities with MeshComponent and TransformComponent
 */
class RenderSystem : public TypedSystem<TransformComponent, MeshComponent> {
public:
    RenderSystem() = default;
    
    /**
     * Initialize with model manager
     */
    void setModelManager(ModelManager* modelManager) {
        m_modelManager = modelManager;
    }
    
    /**
     * Set terrain for height-based rendering
     */
    void setTerrain(Terrain* terrain) {
        m_terrain = terrain;
    }
    
    /**
     * Set world object manager
     */
    void setWorldObjectManager(WorldObjectManager* wom) {
        m_worldObjectManager = wom;
    }
    
    void init() override {
        // Set filter: requires Transform + Mesh, excludes inactive
        m_filter = SystemFilter::require<TransformComponent, MeshComponent>();
    }
    
    void update(float deltaTime) override {
        // Update render state (culling, LOD, etc.)
        m_visibleCount = 0;
    }
    
    void render() override {
        // Note: Actual rendering requires ModelManager which is forward-declared here
        // To use rendering, include "modelSystem/Model.h" before this header
        // and call setModelManager() with a valid pointer
        if (!m_modelManager) return;
        
        m_visibleCount = 0;
        // Rendering implementation requires full ModelManager definition
        // This is a placeholder - actual rendering done by engine
    }
    
    /**
     * Set view and projection matrices for culling
     */
    void setViewProjection(const glm::mat4& view, const glm::mat4& projection) {
        m_viewMatrix = view;
        m_projectionMatrix = projection;
        m_viewProjectionMatrix = projection * view;
    }
    
    /**
     * Get visible entity count (after culling)
     */
    size_t getVisibleCount() const { return m_visibleCount; }
    
    const char* getName() const override { return "RenderSystem"; }

private:
    ModelManager* m_modelManager = nullptr;
    Terrain* m_terrain = nullptr;
    WorldObjectManager* m_worldObjectManager = nullptr;
    
    glm::mat4 m_viewMatrix{1.0f};
    glm::mat4 m_projectionMatrix{1.0f};
    glm::mat4 m_viewProjectionMatrix{1.0f};
    
    size_t m_visibleCount = 0;
};

/**
 * Skinned Mesh Render System - Renders animated characters
 */
class SkinnedMeshRenderSystem : public TypedSystem<TransformComponent, SkinnedMeshComponent, SkeletonComponent> {
public:
    SkinnedMeshRenderSystem() = default;
    
    void setModelManager(ModelManager* modelManager) {
        m_modelManager = modelManager;
    }
    
    void init() override {
        m_filter = SystemFilter::require<TransformComponent, SkinnedMeshComponent, SkeletonComponent>();
    }
    
    void update(float deltaTime) override {
        // Update bone matrices for GPU skinning
    }
    
    void render() override {
        if (!m_modelManager) return;
        
        forEach(*m_entityManager, *m_componentManager,
            [this](EntityID entityID, TransformComponent& transform, 
                   SkinnedMeshComponent& skinnedMesh, SkeletonComponent& skeleton) {
                if (!skinnedMesh.visible || skinnedMesh.meshID < 0) return;
                
                glm::mat4 modelMatrix = transform.getModelMatrix();
                
                // Draw skinned mesh with bone matrices
                // Note: This requires ModelManager to have drawSkinnedMesh method
                // For now, skip if method doesn't exist
                (void)skeleton;  // Suppress unused warning
            });
    }
    
    const char* getName() const override { return "SkinnedMeshRenderSystem"; }

private:
    ModelManager* m_modelManager = nullptr;
};

/**
 * Camera System - Manages active camera
 */
class CameraSystem : public TypedSystem<TransformComponent, CameraComponent> {
public:
    CameraSystem() = default;
    
    void init() override {
        m_filter = SystemFilter::require<TransformComponent, CameraComponent>();
    }
    
    void update(float deltaTime) override {
        m_activeCamera = nullptr;
        m_activeCameraEntity = INVALID_ENTITY_ID;
        
        // Find active camera
        forEach(*m_entityManager, *m_componentManager,
            [this](EntityID entityID, TransformComponent& transform, CameraComponent& camera) {
                if (camera.isActive && m_activeCamera == nullptr) {
                    m_activeCamera = &camera;
                    m_activeCameraEntity = entityID;
                    m_activeCameraTransform = &transform;
                }
            });
    }
    
    /**
     * Get the active camera view matrix
     */
    glm::mat4 getViewMatrix() const {
        if (!m_activeCameraTransform) {
            return glm::mat4(1.0f);
        }
        
        const TransformComponent& transform = *m_activeCameraTransform;
        glm::vec3 forward = transform.getForward();
        glm::vec3 up = transform.getUp();
        
        return glm::lookAt(
            transform.position,
            transform.position + forward,
            up
        );
    }
    
    /**
     * Get the active camera projection matrix
     */
    glm::mat4 getProjectionMatrix() const {
        if (!m_activeCamera) {
            return glm::perspective(glm::radians(45.0f), 16.0f/9.0f, 0.1f, 1000.0f);
        }
        return m_activeCamera->getProjectionMatrix();
    }
    
    /**
     * Get active camera position
     */
    glm::vec3 getCameraPosition() const {
        if (!m_activeCameraTransform) {
            return glm::vec3(0.0f);
        }
        return m_activeCameraTransform->position;
    }
    
    /**
     * Get active camera component
     */
    const CameraComponent* getActiveCamera() const { return m_activeCamera; }
    
    /**
     * Get active camera entity
     */
    EntityID getActiveCameraEntity() const { return m_activeCameraEntity; }
    
    /**
     * Set a specific entity as the active camera
     */
    void setActiveCamera(Entity entity) {
        m_activeCameraEntity = entity.id;
    }
    
    const char* getName() const override { return "CameraSystem"; }

private:
    CameraComponent* m_activeCamera = nullptr;
    TransformComponent* m_activeCameraTransform = nullptr;
    EntityID m_activeCameraEntity = INVALID_ENTITY_ID;
};

/**
 * Light System - Manages lights for rendering
 */
class LightSystem : public TypedSystem<TransformComponent, LightComponent> {
public:
    LightSystem() = default;
    
    void init() override {
        m_filter = SystemFilter::require<TransformComponent, LightComponent>();
    }
    
    void update(float deltaTime) override {
        m_directionalLights.clear();
        m_pointLights.clear();
        m_spotLights.clear();
        
        forEach(*m_entityManager, *m_componentManager,
            [this](EntityID entityID, TransformComponent& transform, LightComponent& light) {
                if (!light.enabled) return;
                
                switch (light.type) {
                    case LightType::DIRECTIONAL:
                        m_directionalLights.push_back({entityID, &transform, &light});
                        break;
                    case LightType::POINT:
                        m_pointLights.push_back({entityID, &transform, &light});
                        break;
                    case LightType::SPOT:
                        m_spotLights.push_back({entityID, &transform, &light});
                        break;
                    default:
                        break;
                }
            });
    }
    
    struct LightEntry {
        EntityID entity;
        TransformComponent* transform;
        LightComponent* light;
    };
    
    const std::vector<LightEntry>& getDirectionalLights() const { return m_directionalLights; }
    const std::vector<LightEntry>& getPointLights() const { return m_pointLights; }
    const std::vector<LightEntry>& getSpotLights() const { return m_spotLights; }
    
    const char* getName() const override { return "LightSystem"; }

private:
    std::vector<LightEntry> m_directionalLights;
    std::vector<LightEntry> m_pointLights;
    std::vector<LightEntry> m_spotLights;
};

} // namespace ecs
