#pragma once

/**
 * Integrated Render System - Uses engine's Renderer
 *
 * This system syncs ECS MeshComponent with engine Renderer
 * and uses engine's batching and GPU submission.
 */

#include "../ECS.h"
#include "../components/Components.h"
#include "../../renderer/Renderer.h"
#include "../../modelSystem/Model.h"
#include <glm/glm.hpp>
#include <vector>
#include <iostream>

// Forward declarations
class Terrain;
class WorldObjectManager;

namespace ecs {

/**
 * Render System - Integrates ECS with engine Renderer
 */
class RenderSystem : public TypedSystem<TransformComponent, MeshComponent> {
public:
    RenderSystem() = default;
    ~RenderSystem() = default;

    /**
     * Set the world pointer for iteration
     */
    void setWorld(World* world) { m_world = world; }
    
    World* getWorld() const { return m_world; }

    /**
     * Set the engine's renderer
     */
    void setRenderer(Renderer* renderer) {
        m_renderer = renderer;
    }

    Renderer* getRenderer() const { return m_renderer; }

    /**
     * Set the model pointer for rendering
     */
    void setModel(Model* model) {
        m_model = model;
    }

    Model* getModel() const { return m_model; }

    /**
     * Set terrain for world rendering
     */
    void setTerrain(Terrain* terrain) { m_terrain = terrain; }

    /**
     * Set world object manager
     */
    void setWorldObjectManager(WorldObjectManager* wom) { m_worldObjectManager = wom; }

    void init() override {
        m_filter = SystemFilter::require<TransformComponent, MeshComponent>();
    }

    void update(float deltaTime) override {
        m_visibleCount = 0;
        
        if (m_entityManager && m_componentManager) {
            forEach(*m_entityManager, *m_componentManager,
                [this](EntityID, TransformComponent&, MeshComponent& mesh) {
                    if (mesh.visible) m_visibleCount++;
                });
        }
        (void)deltaTime;
    }

    /**
     * Render all visible entities using archetype iteration
     */
    void render() override {
        if (!m_model) {
            return;
        }

        m_visibleCount = 0;

        // Use world's forEach which properly uses archetypes
        if (m_world) {
            m_world->forEach<TransformComponent, MeshComponent>(
                [this](EntityID entityID, TransformComponent& transform, MeshComponent& mesh) {
                    renderEntity(entityID, transform, mesh);
                }
            );
        }
        
        // Submit batches to GPU via engine Renderer
        if (m_renderer) {
            m_renderer->Render();
        }
    }

    /**
     * Render a single entity
     */
    void renderEntity(EntityID entityID, TransformComponent& transform, MeshComponent& mesh) {
        if (!mesh.visible || !m_renderer || !m_model) return;

        m_visibleCount++;

        // Calculate model matrix
        glm::mat4 model = transform.getModelMatrix();

        // Check if model has debug VAO (programmatic mesh)
        if (m_model->getDebugVAO() != 0 && m_model->getDebugIndexCount() > 0) {
            // Use debug VAO directly
            std::vector<glm::mat4> transforms = {model};
            m_renderer->AddRenderable(
                m_model->getDebugVAO(),
                0,  // VBO not needed (in VAO)
                0,  // EBO not needed (in VAO)
                m_model->getDebugIndexCount(),
                GL_TRIANGLES,
                m_defaultShaderProgram,
                transforms
            );
        }
        // Otherwise use mesh from model
        else if (mesh.meshID >= 0 && mesh.meshID < static_cast<int>(m_model->GetMeshCount())) {
            Mesh& meshData = m_model->GetMesh(mesh.meshID);

            // Add to renderer batch
            std::vector<glm::mat4> transforms = {model};

            m_renderer->AddRenderable(
                meshData.VAO,
                0,  // VBO not needed (in VAO)
                0,  // EBO not needed (in VAO)
                meshData.GetStatistics().indexCount,
                GL_TRIANGLES,
                m_defaultShaderProgram,
                transforms
            );
        }
    }

    /**
     * Render skinned mesh with GPU skinning
     */
    void renderSkinnedMesh(EntityID entityID, TransformComponent& transform, 
                           SkinnedMeshComponent& skinnedMesh, SkeletonComponent& skeleton) {
        if (!m_renderer || !m_model) return;
        if (!skinnedMesh.visible) return;

        // Get mesh from model
        if (skinnedMesh.meshID < 0 || skinnedMesh.meshID >= static_cast<int>(m_model->GetMeshCount())) return;
        Mesh& meshData = m_model->GetMesh(skinnedMesh.meshID);

        // Calculate model matrix
        glm::mat4 model = transform.getModelMatrix();

        // Add to renderer batch
        std::vector<glm::mat4> transforms = {model};
        
        GLuint shaderProgram = 0;  // Would get skinned shader

        m_renderer->AddRenderable(
            meshData.VAO,
            0,  // VBO not needed (in VAO)
            0,  // EBO not needed (in VAO)
            meshData.GetStatistics().indexCount,
            GL_TRIANGLES,
            shaderProgram,
            transforms
        );
    }

    /**
     * Set view and projection matrices
     */
    void setViewProjection(const glm::mat4& view, const glm::mat4& projection) {
        m_viewMatrix = view;
        m_projectionMatrix = projection;
        m_viewProjectionMatrix = projection * view;
        
        if (m_renderer) {
            m_renderer->SetCameraMatrices(view, projection);
        }
    }

    /**
     * Set default shader programs
     */
    void setDefaultShaderProgram(GLuint program) { m_defaultShaderProgram = program; }
    void setSkinnedShaderProgram(GLuint program) { m_skinnedShaderProgram = program; }

    size_t getVisibleCount() const { return m_visibleCount; }
    const char* getName() const override { return "RenderSystem (Integrated)"; }

protected:
    World* m_world = nullptr;
    Renderer* m_renderer = nullptr;
    Model* m_model = nullptr;
    Terrain* m_terrain = nullptr;
    WorldObjectManager* m_worldObjectManager = nullptr;
    
    glm::mat4 m_viewMatrix{1.0f};
    glm::mat4 m_projectionMatrix{1.0f};
    glm::mat4 m_viewProjectionMatrix{1.0f};
    
    GLuint m_defaultShaderProgram = 0;
    GLuint m_skinnedShaderProgram = 0;
    
    size_t m_visibleCount = 0;
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
        m_activeCameraTransform = nullptr;

        if (!m_entityManager || !m_componentManager) return;

        forEach(*m_entityManager, *m_componentManager,
            [this](EntityID entityID, TransformComponent& transform, CameraComponent& camera) {
                if (camera.isActive && m_activeCamera == nullptr) {
                    m_activeCamera = &camera;
                    m_activeCameraEntity = entityID;
                    m_activeCameraTransform = &transform;
                }
            });
        (void)deltaTime;
    }

    glm::mat4 getViewMatrix() const {
        if (!m_activeCameraTransform) {
            return glm::mat4(1.0f);
        }
        const TransformComponent& transform = *m_activeCameraTransform;
        return glm::lookAt(
            transform.position, 
            transform.position + transform.getForward(), 
            transform.getUp()
        );
    }

    glm::mat4 getProjectionMatrix() const {
        if (!m_activeCamera) {
            return glm::perspective(glm::radians(45.0f), 16.0f/9.0f, 0.1f, 1000.0f);
        }
        const CameraComponent& camera = *m_activeCamera;
        float aspect = static_cast<float>(m_viewportWidth) / static_cast<float>(m_viewportHeight);
        
        if (camera.isOrthographic) {
            return glm::ortho(
                -camera.orthoSize * aspect,
                camera.orthoSize * aspect,
                -camera.orthoSize,
                camera.orthoSize,
                camera.nearPlane,
                camera.farPlane
            );
        } else {
            return glm::perspective(
                glm::radians(camera.fov),
                aspect,
                camera.nearPlane,
                camera.farPlane
            );
        }
    }

    void setViewport(int width, int height) {
        m_viewportWidth = width;
        m_viewportHeight = height;
    }

    CameraComponent* getActiveCamera() { return m_activeCamera; }
    const CameraComponent* getActiveCamera() const { return m_activeCamera; }
    EntityID getActiveCameraEntity() const { return m_activeCameraEntity; }

    const char* getName() const override { return "CameraSystem"; }

private:
    CameraComponent* m_activeCamera = nullptr;
    TransformComponent* m_activeCameraTransform = nullptr;
    EntityID m_activeCameraEntity = INVALID_ENTITY_ID;
    
    int m_viewportWidth = 1280;
    int m_viewportHeight = 720;
};

/**
 * Light System - Manages lighting
 */
class LightSystem : public TypedSystem<TransformComponent, LightComponent> {
public:
    struct LightData {
        EntityID entityID;
        LightType type;
        glm::vec3 color;
        float intensity;
        glm::vec3 direction;
        glm::vec3 position;
        float range;
        float spotInnerAngle;
        float spotOuterAngle;
        bool castShadows;
    };

    LightSystem() = default;

    void init() override {
        m_filter = SystemFilter::require<TransformComponent, LightComponent>();
    }

    void update(float deltaTime) override {
        m_lights.clear();
        
        if (!m_entityManager || !m_componentManager) return;

        forEach(*m_entityManager, *m_componentManager,
            [this](EntityID entityID, TransformComponent& transform, LightComponent& light) {
                LightData lightData;
                lightData.entityID = entityID;
                lightData.type = light.type;
                lightData.color = light.color;
                lightData.intensity = light.intensity;
                lightData.direction = transform.getForward();
                lightData.position = transform.position;
                lightData.range = light.range;
                lightData.spotInnerAngle = light.spotInnerAngle;
                lightData.spotOuterAngle = light.spotOuterAngle;
                lightData.castShadows = light.castShadows;
                m_lights.push_back(lightData);
            });
        (void)deltaTime;
    }

    const std::vector<LightData>& getLights() const { return m_lights; }

    const char* getName() const override { return "LightSystem"; }

private:
    std::vector<LightData> m_lights;
};

} // namespace ecs
