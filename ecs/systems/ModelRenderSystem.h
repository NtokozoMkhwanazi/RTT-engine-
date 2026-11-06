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
#include "../components/AnimatedCharacterComponent.h" // NPC shared-character component + TickAnimatedCharacter
#include "../../renderer/Renderer.h"
#include "../../modelSystem/Model.h"
#include "../../modelSystem/ModelManager.h"
#include "../../shaderSystem/Shader.h"
#include <glm/glm.hpp>
#include <vector>
#include <iostream>
#include "../../editor/world_manager.h"   // World::WorldManager singleton: terrain height for geo NPCs

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
     * Scene-layer culling (see RenderSystem): hide whole layers from the Layers
     * panel — applies to static models (render() below) and skinned NPCs
     * (renderAnimated()). Entities without a LayerComponent default to the
     * always-visible Default layer (0).
     */
    void setLayerVisibility(bool v[LayerComponent::kMaxLayers]) {
        for (int i = 0; i < LayerComponent::kMaxLayers; ++i)
            m_layerVisible[i] = v[i];
    }
    bool m_layerVisible[LayerComponent::kMaxLayers] =
        {true,true,true,true,true,true,true,true};

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
        // Tick AnimatedCharacterComponent NPCs that share the play character's
        // full FSM / motion-matching / foot-IK properties (pose-synced into the
        // ECS TransformComponent twin so the camera/focus/gizmo track them).
        updateAnimatedCharacters(deltaTime);
    }

    /**
     * Update animations for all animated models
     */
    void updateAnimations(float deltaTime) {
        if (!m_world || m_world->getEntityCount() == 0) return;

        // Advance bone matrices for animated models. Uses m_world->forEach (== render())
        // so this works even for static systems in the editor, which never have the
        // legacy m_entityManager/m_componentManager wired up.
        m_world->forEach<TransformComponent, ModelComponent>(
            [this, deltaTime](EntityID eid, TransformComponent& transform, ModelComponent& modelComp) {
                if (!m_world->hasComponent<ModelAnimatorComponent>(Entity{eid})) return;
                auto* animatorComp = m_world->getComponent<ModelAnimatorComponent>(Entity{eid});
                if (animatorComp && animatorComp->isValid() && animatorComp->isPlaying) {
                    updateAnimation(eid, transform, modelComp, *animatorComp, deltaTime);
                }
            });
    }

    /**
     * Update AnimatedCharacterComponent NPCs (full character FSM + motion
     * matching + foot IK) that share the play character's properties. Runs
     * every frame alongside the ModelAnimatorComponent tick above so NPC
     * characters advance their poses via the same per-frame path.
     */
    void updateAnimatedCharacters(float deltaTime) {
        if (!m_world || m_world->getEntityCount() == 0) return;

        m_world->forEach<TransformComponent, AnimatedCharacterComponent>(
            [deltaTime](EntityID eid, TransformComponent& t, AnimatedCharacterComponent& ac) {
                if (!ac.active) return;
                if (ac.externallyTicked) return;   // play char: FSM driven by PlayModeController, not NPC wander/geo
                if (!ac.character || !ac.character->ready()) return;
                glm::vec2 moveDir(0.0f);
                float moveMag = 0.0f;
                bool wantSprint = false;
                if (ac.geoDriven) {
                    // Live telemetry: direction + magnitude come from the feed
                    // (GeospatialSystem::update / injected sim drive). geoMoveSpeed
                    // is a normalized walk band [0..1]; >1.0 promotes to RUN
                    // (AnimatedCharacter only drives RUN under input.sprint).
                    moveDir = ac.geoMoveDir;
                    moveMag = ac.geoMoveSpeed;
                    wantSprint = moveMag > 1.0f;
                    moveMag = glm::min(moveMag, 1.0f);
                } else {
                    ac.idleTimer += deltaTime;
                    if (ac.wanderTimer > 0.0f) {
                        ac.wanderTimer -= deltaTime;
                        moveDir = ac.wanderDir;
                    } else if (ac.idleTimer > 6.0f) {   // idle a while, then wander
                        ac.idleTimer = 0.0f;
                        ac.wanderTimer = 2.0f;
                        ac.wanderDir = glm::vec2(0.4f, 0.0f);
                    }
                    moveMag = glm::length(moveDir);
                }
                CharacterInput input;
                input.moveDirection = moveDir;
                input.moveMagnitude = moveMag;
                input.sprint = wantSprint;
                input.grounded = true;
                input.motionContext = static_cast<int>(ac.motionContext);
                // Geo owns the NPC's absolute position (live fix or sim spawn),
                // so capture it before the tick — AnimatedCharacter::update
                // integrates input/root-motion velocity which would drift the
                // NPC off its tracked lat/lon fix. Restored below for geo NPCs.
                const glm::vec3 geoPos = ac.character->position;
                // Real RockyGround terrain (heightmap + boulder tops) pins the
                // NPC's feet to the surface; flat-0 fallback when no world is
                // initialized (headless sims / unit tests). Qualified with ::
                // to bypass ecs::World (the ECS World class) name shadowing.
                const auto terrainH = [](float x, float z) -> float {
                    ::World::WorldManager& wm = ::World::WorldManager::getInstance();
                    return wm.getTerrain() ? wm.getSurfaceHeightAt(x, z) : 0.0f;
                };
                TickAnimatedCharacter(*ac.character, t, deltaTime, input,
                                      terrainH);
                if (ac.geoDriven) {
                    // Pin to the geo position (x/z from the feed or sim spawn) and
                    // snap Y to the RockyGround surface so the NPC stands ON the
                    // terrain instead of buried/floating; the locomotion FSM still
                    // plays by feed speed. Restores geo ownership + terrain height.
                    ac.character->position = geoPos;
                    t.position = geoPos;
                    const float groundY = terrainH(geoPos.x, geoPos.z);
                    ac.character->position.y = groundY;
                    t.position.y = groundY;
                }
                // Geo-driven NPCs: emit each NPC's live FSM state every tick so a
                // stress run proves N simultaneous geo-tracked NPCs animate idle /
                // walk / run by feed velocity (and none are frozen T-pose).
                if (ac.geoDriven) {
                    const char* fsmName = "other";
                    switch (ac.character->state()) {
                        case AnimationState::IDLE: fsmName = "idle"; break;
                        case AnimationState::WALK: fsmName = "walk"; break;
                        case AnimationState::RUN:  fsmName = "run";  break;
                        default: break;
                    }
                    std::cout << "[GeoNPC] entity " << eid << " FSM=" << fsmName
                              << " geoSpeed=" << ac.geoMoveSpeed
                              << " curSpeed=" << ac.character->currentSpeed()
                              << "m/s pos=(" << t.position.x << "," << t.position.y
                              << "," << t.position.z << ")\n";
                }
                // Inertial crossfade: drain any active NPC blend layers (pushed
                // by AnimatedCharacterComponent::reload() on hot-swap) out of the
                // just-updated live bone palette before renderAnimated() reads it.
                // No-op in the common case (no layers); otherwise mirrors the
                // play character's PlayModeController::tickInertialBlender().
                if (!ac.activeBlendLayers.empty()) {
                    auto& bones = const_cast<std::vector<glm::mat4>&>(
                        ac.character->debugFinalBones());
                    if (!bones.empty())
                        ac.tickInertialBlender(deltaTime, bones);
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
        [this, &rendered](EntityID eid, TransformComponent& t, ModelComponent& m) {
            // Cull by scene layer (Layers panel toggles m_layerVisible[8]).
            if (m_world) {
                if (const auto* lc = m_world->getComponent<LayerComponent>(Entity{eid}); lc) {
                    if (lc->layer < 0 || lc->layer >= LayerComponent::kMaxLayers ||
                        !m_layerVisible[lc->layer])
                        return;
                }
            }
            // Animated (skinned) entities are drawn by renderAnimated() via
            // Model::Draw(shader, animator) — never as static batched triangles.
            if (m_world && m_world->hasComponent<ModelAnimatorComponent>(Entity{eid})) return;
            // Geo personnel (AnimatedCharacter) twins render skinned via
            // renderAnimated(); skip them here so the static ModelComponent mesh
            // (the GeoViz point-marker gate) is never drawn as a second T-pose body.
            if (m_world && m_world->hasComponent<AnimatedCharacterComponent>(Entity{eid})) return;
            if (!m.visible) return;
            if (!m.isValid()) return;
            
            Model* model = m.getModel();
            if (!model) return;
            if (model->GetMeshCount() == 0) return;
            
            glm::mat4 modelMatrix = t.getModelMatrix();
            
            if (m_skinShader) {
                // Forward-render this static model through the SAME skin shader
                // + direct draw path the animated character uses
                // (renderAnimated / renderPlayCharacter, both driven by
                // m_renderPipeline.getModelShader(), bound at setSkinShader).
                // The skin shader defaults skinning ON; the legacy batched
                // AddRenderable path never set uDisableSkinning /
                // uBoneBufferEnabled, so a stale bone palette warped these meshes
                // off-screen (they registered "healthy" but never appeared).
                // Model::DrawStatic flips uDisableSkinning=1 /
                // uBoneBufferEnabled=0 itself (model.cpp); we set the
                // camera+model uniforms so the draw is self-contained and uses
                // the viewport-configured forward renderer instead of stale
                // batch state. (Material overrides are not carried on the
                // skin-shader path — matching renderPlayCharacter.)
                m_skinShader->use();
                m_skinShader->setMat4("projection", m_projectionMatrix);
                m_skinShader->setMat4("view", m_viewMatrix);
                m_skinShader->setMat4("model", modelMatrix);
                m_skinShader->setVec3("lightPos", glm::vec3(10.0f, 15.0f, 10.0f));
                m_skinShader->setVec3("viewPos", m_cameraPos);
                m_skinShader->setInt("uDisableInstancing", 1);
                m_skinShader->setInt("uDisableSkinning", 1);
                m_skinShader->setInt("uPaletteSize", 0);
                m_skinShader->setInt("uShowDebug", 0);
                model->DrawStatic(*m_skinShader);
                ++rendered;
            } else {
                // Fallback: legacy batched path (headless tests that don't bind
                // the skin shader).
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
                    ++rendered;
                }
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
        if (!m_renderer || !m_skinShader || !m_world || m_world->getEntityCount() == 0) return;

        // Immediate skinning pass: each animated model sets its own bone palette
        // via Model::Draw(shader, animator) (the proven path used by
        // RenderPlayCharacter). Called AFTER the batched static pass so skinned
        // NPCs depth-test against the deferred G-buffer + static forward geometry.
        m_world->forEach<TransformComponent, ModelComponent>(
            [this](EntityID eid, TransformComponent& transform, ModelComponent& modelComp) {
                // Cull by scene layer (Layers panel toggles m_layerVisible[8]).
                if (m_world) {
                    if (const auto* lc = m_world->getComponent<LayerComponent>(Entity{eid}); lc) {
                        if (lc->layer < 0 || lc->layer >= LayerComponent::kMaxLayers ||
                            !m_layerVisible[lc->layer])
                            return;
                    }
                }
                if (!modelComp.visible || !modelComp.isValid()) return;
                if (!m_world->hasComponent<ModelAnimatorComponent>(Entity{eid})) return;
                auto* animatorComp = m_world->getComponent<ModelAnimatorComponent>(Entity{eid});
                if (animatorComp && animatorComp->isValid()) {
                    renderAnimatedModel(eid, transform, modelComp, *animatorComp);
                }
            });

        // AnimatedCharacterComponent NPCs: render via the skin shader, mirroring
        // RenderPlayCharacter (AnimatedCharacter::modelMatrix() carries
        // visualScale + the 180? facing flip; the character's own Animator
        // supplies the bone palette).
        m_world->forEach<TransformComponent, AnimatedCharacterComponent>(
            [this](EntityID eid, TransformComponent&, AnimatedCharacterComponent& ac) {
                if (!m_world) return;
                if (const auto* lc = m_world->getComponent<LayerComponent>(Entity{eid}); lc) {
                    if (lc->layer < 0 || lc->layer >= LayerComponent::kMaxLayers ||
                        !m_layerVisible[lc->layer])
                        return;
                }
                if (!ac.active || !ac.isValid()) return;
                Model* model = ac.getModel();
                AnimatedCharacter* ch = ac.get();
                Animator* animator = ch ? ch->animator() : nullptr;
                if (!model || !animator) return;
                m_skinShader->use();
                m_skinShader->setMat4("projection", m_projectionMatrix);
                m_skinShader->setMat4("view", m_viewMatrix);
                m_skinShader->setMat4("model", ch->modelMatrix());
                m_skinShader->setVec3("lightPos", glm::vec3(10.0f, 15.0f, 10.0f));
                m_skinShader->setVec3("viewPos", m_cameraPos);
                m_skinShader->setInt("uDisableInstancing", 1);
                m_skinShader->setInt("uDisableSkinning", 0);
                m_skinShader->setInt("uPaletteSize", 256);
                m_skinShader->setInt("uShowDebug", 0);
                model->Draw(*m_skinShader, *animator);
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
        if (!model) return;
        Animator* animator = animatorComp.getAnimator();
        if (!animator) {
            renderModel(entityID, transform, modelComp);
            return;
        }

        const glm::mat4 modelMatrix = transform.getModelMatrix();

        // GPU skinning — mirrors RenderPlayCharacter (test.cpp): set camera/light
        // + skinning uniforms, then Model::Draw uploads the bone SSBO and draws the
        // skinned meshes immediately (not via the batched Renderer).
        m_skinShader->use();
        m_skinShader->setMat4("projection", m_projectionMatrix);
        m_skinShader->setMat4("view", m_viewMatrix);
        m_skinShader->setMat4("model", modelMatrix);
        m_skinShader->setVec3("lightPos", glm::vec3(10.0f, 15.0f, 10.0f));
        m_skinShader->setVec3("viewPos", m_cameraPos);
        m_skinShader->setInt("uDisableInstancing", 1);
        m_skinShader->setInt("uDisableSkinning", 0);
        m_skinShader->setInt("uPaletteSize", 256);
        m_skinShader->setInt("uShowDebug", 0);
        model->Draw(*m_skinShader, *animator);
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
    void setSkinShader(Shader* shader) { m_skinShader = shader; }
    void setCameraPosition(const glm::vec3& pos) { m_cameraPos = pos; }

protected:
    World* m_world = nullptr;
    Renderer* m_renderer = nullptr;

    glm::mat4 m_viewMatrix{1.0f};
    glm::mat4 m_projectionMatrix{1.0f};
    glm::mat4 m_viewProjectionMatrix{1.0f};

    GLuint m_defaultShaderProgram = 0;
    GLuint m_skinnedShaderProgram = 0;

    Shader* m_skinShader = nullptr;
    glm::vec3 m_cameraPos{0.0f};

    size_t m_visibleCount = 0;
};

} // namespace ecs
