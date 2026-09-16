#pragma once

/**
 * Model Render System - Renders entities with ModelComponent
 * 
 * This system handles:
 * - Rendering static models (no animation)
 * - Rendering animated models with GPU skinning
 * - Animation updates and bone matrix uploads
 * - LOD selection and culling
 */

#include "../ECS.h"
#include "../components/Components.h"
#include "../components/ModelComponent.h"
#include "../../renderer/Renderer.h"
#include "../../modelSystem/Model.h"
#include "../../modelSystem/ModelManager.h"
#include <glm/glm.hpp>
#include <vector>
#include <iostream>

namespace ecs {

/**
 * ModelRenderSystem - Handles rendering of ModelComponent entities
 */
class ModelRenderSystem : public TypedSystem<TransformComponent, ModelComponent> {
public:
    ModelRenderSystem() = default;
    ~ModelRenderSystem() = default;

    /**
     * Set the world pointer
     */
    void setWorld(World* world) { m_world = world; }
    void setRenderer(Renderer* renderer) { m_renderer = renderer; }
    Renderer* getRenderer() const { return m_renderer; }

/**
       * Render a single static model entity (internal helper)
       */
    void renderModelInternal(TransformComponent& transform, ModelComponent& modelComp) {
        if (!modelComp.visible || !m_renderer) return;
        if (!modelComp.isValid()) return;
        
        Model* model = modelComp.getModel();
        if (!model) return;
        
        size_t meshCount = model->GetMeshCount();
        if (meshCount == 0) return;

        glm::mat4 modelMatrix = transform.getModelMatrix();

        // Render each mesh in the model
        for (size_t i = 0; i < meshCount; ++i) {
            Mesh& mesh = model->GetMesh(i);
            
            if (mesh.GetVAO() == 0) continue;

            std::vector<glm::mat4> transforms = {modelMatrix};                m_renderer->AddRenderable(
                    mesh.GetVAO(), 0, mesh.GetEBO(),
                    mesh.GetStatistics().indexCount,
                    GL_TRIANGLES, m_defaultShaderProgram,
                    transforms,
                    modelComp.useMaterialOverrides ? modelComp.albedoOverride : glm::vec3(1.0f),
                    modelComp.useMaterialOverrides ? modelComp.metallicOverride : 0.5f,
                    modelComp.useMaterialOverrides ? modelComp.roughnessOverride : 0.5f,
                    mesh.GetBoundingBox().min, mesh.GetBoundingBox().max
                );
        }
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

    void init() override {
        m_filter = SystemFilter::require<TransformComponent, ModelComponent>();
    }

    /**
     * Update system (required override)
     */
    void update(float deltaTime) override {
        // Update animations
        updateAnimations(deltaTime);
    }

    /**
     * Update animations for all animated models
     */
    void updateAnimations(float deltaTime) {
        if (!m_entityManager || !m_componentManager) return;

        // Iterate through entities with ModelComponent and ModelAnimatorComponent
        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID entityID, TransformComponent& transform, ModelComponent& modelComp) {
                // Check if entity has animator
                if (m_componentManager->hasComponent<ModelAnimatorComponent>(entityID)) {
                    auto* animatorComp = m_componentManager->getComponent<ModelAnimatorComponent>(entityID);
                    if (animatorComp && animatorComp->isValid() && animatorComp->isPlaying) {
                        updateAnimation(entityID, transform, modelComp, *animatorComp, deltaTime);
                    }
                }
            });
    }

/**
 * Render all entities with ModelComponent
 */
void render() override {
    if (!m_renderer) return;
    if (!m_world || m_world->getEntityCount() == 0) return;
    
    int rendered = 0;
    
    m_world->forEach<TransformComponent, ModelComponent>(
        [this, &rendered](EntityID, TransformComponent& t, ModelComponent& m) {
            if (!m.visible) return;
            if (!m.isValid()) return;
            
            Model* model = m.getModel();
            if (!model) return;
            if (model->GetMeshCount() == 0) return;
            
            glm::mat4 modelMatrix = t.getModelMatrix();
            
            for (size_t i = 0; i < model->GetMeshCount(); ++i) {
                Mesh& mesh = model->GetMesh(i);
                if (mesh.GetVAO() == 0) continue;
                
                std::vector<glm::mat4> transforms = {modelMatrix};
                m_renderer->AddRenderable(
                    mesh.GetVAO(), 0, mesh.GetEBO(),
                    mesh.GetStatistics().indexCount,
                    GL_TRIANGLES, m_defaultShaderProgram,
                    transforms,
                    m.useMaterialOverrides ? m.albedoOverride : glm::vec3(1.0f),
                    m.useMaterialOverrides ? m.metallicOverride : 0.5f,
                    m.useMaterialOverrides ? m.roughnessOverride : 0.5f,
                    mesh.GetBoundingBox().min, mesh.GetBoundingBox().max
                );
                rendered++;
            }
        });
    
    if (rendered > 0 && m_renderer) {
        m_renderer->SubmitBatches();
    }
    
    m_visibleCount = rendered;
}

/**
     * Render animated models with skinning
     */
    void renderAnimated() {
        if (!m_renderer) return;

        if (!m_entityManager || !m_componentManager) return;

        forEach(*m_entityManager, *m_componentManager,
            [this](EntityID entityID, TransformComponent& transform, ModelComponent& modelComp) {
                if (!modelComp.visible || !modelComp.isValid()) return;

                // Check if entity has animator
                if (m_componentManager->hasComponent<ModelAnimatorComponent>(entityID)) {
                    auto* animatorComp = m_componentManager->getComponent<ModelAnimatorComponent>(entityID);
                    if (animatorComp && animatorComp->isValid()) {
                        renderAnimatedModel(entityID, transform, modelComp, *animatorComp);
                        return;
                    }
                }

                // Fall back to static rendering
                renderModel(entityID, transform, modelComp);
            });
    }

/**
      * Render a single static model entity
      */
    void renderModel(EntityID entityID, TransformComponent& transform, ModelComponent& modelComp) {
        if (!modelComp.visible || !m_renderer) return;
        if (!modelComp.isValid()) return;
        
        Model* model = modelComp.getModel();
        if (!model) return;
        
        size_t meshCount = model->GetMeshCount();
        if (meshCount == 0) return;

        glm::mat4 modelMatrix = transform.getModelMatrix();

        // Render each mesh in the model
        for (size_t i = 0; i < meshCount; ++i) {
            Mesh& mesh = model->GetMesh(i);
            
            if (mesh.GetVAO() == 0) continue;

            std::vector<glm::mat4> transforms = {modelMatrix};

            glm::vec3 color = modelComp.useMaterialOverrides ? 
                            modelComp.albedoOverride : glm::vec3(1.0f);
            float metallic = modelComp.useMaterialOverrides ? 
                           modelComp.metallicOverride : 0.5f;
            float roughness = modelComp.useMaterialOverrides ? 
                            modelComp.roughnessOverride : 0.5f;

            m_renderer->AddRenderable(
                mesh.GetVAO(),
                0,
                mesh.GetEBO(),
                mesh.GetStatistics().indexCount,
                GL_TRIANGLES,
                m_defaultShaderProgram,
                transforms,
                color,
                metallic,
                roughness,
                mesh.GetBoundingBox().min,
                mesh.GetBoundingBox().max
            );
        }
    }

    /**
     * Render animated model with GPU skinning
     */
    void renderAnimatedModel(EntityID entityID, TransformComponent& transform, 
                            ModelComponent& modelComp, ModelAnimatorComponent& animatorComp) {
        if (!modelComp.visible || !m_renderer) return;
        if (!modelComp.isValid()) return;
        
        if (!animatorComp.isValid()) {
            renderModel(entityID, transform, modelComp);
            return;
        }

        Model* model = modelComp.getModel();
        Animator* animator = animatorComp.getAnimator();
        glm::mat4 modelMatrix = transform.getModelMatrix();

        if (animator->HasBoneBuffer()) {
            animator->UpdateBoneBuffer();
        }

        for (size_t i = 0; i < model->GetMeshCount(); ++i) {
            Mesh& mesh = model->GetMesh(i);
            
            if (mesh.GetVAO() == 0) continue;

            std::vector<glm::mat4> transforms = {modelMatrix};

            GLuint shaderProgram = m_skinnedShaderProgram;
            if (shaderProgram == 0) {
                shaderProgram = m_defaultShaderProgram;
            }

            m_renderer->AddRenderable(
                mesh.GetVAO(),
                0,
                mesh.GetEBO(),
                mesh.GetStatistics().indexCount,
                GL_TRIANGLES,
                shaderProgram,
                transforms,
                modelComp.useMaterialOverrides ? modelComp.albedoOverride : glm::vec3(1.0f),
                modelComp.useMaterialOverrides ? modelComp.metallicOverride : 0.5f,
                modelComp.useMaterialOverrides ? modelComp.roughnessOverride : 0.5f,
                mesh.GetBoundingBox().min,
                mesh.GetBoundingBox().max
            );
        }
    }

    /**
     * Update animation for a single entity
     */
    void updateAnimation(EntityID entityID, TransformComponent& transform,
                        ModelComponent& modelComp, ModelAnimatorComponent& animatorComp, float deltaTime) {
        if (!modelComp.isValid() || !animatorComp.isValid()) return;

        Animator* animator = animatorComp.getAnimator();
        Model* model = modelComp.getModel();
        if (!model) return;

        // Check if we need to start an animation
        if (animatorComp.activeAnimation >= 0 && 
            animatorComp.activeAnimation < static_cast<int>(model->GetAnimationCount())) {
            
            Animation* anim = model->GetAnimation(animatorComp.activeAnimation);
            if (anim) {
                // Check if animator is playing this animation
                if (animator->GetCurrentAnimation() != anim) {
                    animator->Play(anim);
                    animator->SetCurrentTime(animatorComp.currentTime);
                }

                // Update animation time
                if (animatorComp.isPlaying) {
                    animator->Update(deltaTime * animatorComp.playbackSpeed);
                    animatorComp.currentTime = animator->GetCurrentTime();
                }
            }
        }

        // Handle animation blending
        if (animatorComp.nextAnimation >= 0 && 
            animatorComp.nextAnimation < static_cast<int>(model->GetAnimationCount())) {
            
            Animation* nextAnim = model->GetAnimation(animatorComp.nextAnimation);
            if (nextAnim && animatorComp.blendProgress < 1.0f) {
                animatorComp.blendProgress += deltaTime / animatorComp.blendDuration;
                if (animatorComp.blendProgress >= 1.0f) {
                    animatorComp.blendProgress = 1.0f;
                    animatorComp.activeAnimation = animatorComp.nextAnimation;
                    animatorComp.nextAnimation = -1;
                }
            }
        }
    }

    size_t getVisibleCount() const { return m_visibleCount; }
    const char* getName() const override { return "ModelRenderSystem"; }

    /**
     * Set shader programs
     */
    void setDefaultShaderProgram(GLuint program) { m_defaultShaderProgram = program; }
    void setSkinnedShaderProgram(GLuint program) { m_skinnedShaderProgram = program; }

protected:
    World* m_world = nullptr;
    Renderer* m_renderer = nullptr;

    glm::mat4 m_viewMatrix{1.0f};
    glm::mat4 m_projectionMatrix{1.0f};
    glm::mat4 m_viewProjectionMatrix{1.0f};

    GLuint m_defaultShaderProgram = 0;
    GLuint m_skinnedShaderProgram = 0;

    size_t m_visibleCount = 0;
};

} // namespace ecs
