#pragma once

#include "../ECS.h"
#include "../components/Components.h"
#include <glm/glm.hpp>
#include <vector>

// Forward declarations from animation system
class Animator;
class AnimationStateMachine;
class HybridMMFSM;

namespace ecs {

/**
 * Animation System - Updates animator components
 */
class AnimationSystem : public TypedSystem<AnimatorComponent, SkeletonComponent> {
public:
    AnimationSystem() = default;
    
    /**
     * Set external animator manager (optional)
     */
    void setAnimator(Animator* animator) {
        m_animator = animator;
    }
    
    /**
     * Set animation state machine
     */
    void setFSM(AnimationStateMachine* fsm) {
        m_fsm = fsm;
    }
    
    /**
     * Set hybrid MM+FSM system
     */
    void setHybrid(HybridMMFSM* hybrid) {
        m_hybrid = hybrid;
    }
    
    void init() override {
        m_filter = SystemFilter::require<AnimatorComponent, SkeletonComponent>();
    }
    
    void update(float deltaTime) override {
        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID entityID, AnimatorComponent& animator, 
                              SkeletonComponent& skeleton) {
                updateAnimator(animator, skeleton, deltaTime);
            });
    }
    
    /**
     * Update a single animator
     */
    void updateAnimator(AnimatorComponent& animator, SkeletonComponent& skeleton, 
                        float deltaTime) {
        if (!animator.isPlaying) return;
        
        // Update animation time
        animator.currentTime += deltaTime * animator.playbackSpeed;
        
        // Handle looping
        // Note: Animation duration would come from animation data
        // For now, we just keep incrementing
        
        // Update blending
        if (animator.isBlending) {
            animator.blendTime += deltaTime;
            if (animator.blendTime >= animator.blendDuration) {
                animator.isBlending = false;
                animator.blendWeight = 1.0f;
                animator.previousAnimation = -1;
            } else {
                animator.blendWeight = animator.blendTime / animator.blendDuration;
            }
        }
        
        // Update bone transforms (simplified)
        // In production, this would sample animation curves
        updateBoneTransforms(animator, skeleton);
    }
    
    /**
     * Update bone transforms from animation
     */
    void updateBoneTransforms(const AnimatorComponent& animator, 
                              SkeletonComponent& skeleton) {
        // This is a placeholder - actual implementation would:
        // 1. Sample animation curves at currentTime
        // 2. Blend between animations if blending
        // 3. Apply root motion if enabled
        // 4. Calculate final bone matrices
        
        // For now, just ensure bone matrices are identity
        for (auto& bone : skeleton.bones) {
            bone.localTransform = glm::mat4(1.0f);
            bone.worldTransform = glm::mat4(1.0f);
        }
    }
    
    /**
     * Play an animation on an entity
     */
    void playAnimation(Entity entity, int animationIndex, bool loop = true) {
        auto* animator = getComponent<AnimatorComponent>(entity.id);
        if (animator) {
            animator->play(animationIndex, loop);
        }
    }
    
    /**
     * Crossfade to a new animation
     */
    void crossfade(Entity entity, int animationIndex, float duration = 0.2f) {
        auto* animator = getComponent<AnimatorComponent>(entity.id);
        if (animator) {
            animator->blendDuration = duration;
            animator->play(animationIndex);
        }
    }
    
    /**
     * Stop animation
     */
    void stopAnimation(Entity entity) {
        auto* animator = getComponent<AnimatorComponent>(entity.id);
        if (animator) {
            animator->stop();
        }
    }
    
    /**
     * Set animation playback speed
     */
    void setPlaybackSpeed(Entity entity, float speed) {
        auto* animator = getComponent<AnimatorComponent>(entity.id);
        if (animator) {
            animator->playbackSpeed = speed;
        }
    }
    
    /**
     * Enable/disable root motion
     */
    void setRootMotionEnabled(Entity entity, bool enabled) {
        auto* animator = getComponent<AnimatorComponent>(entity.id);
        if (animator) {
            animator->useRootMotion = enabled;
        }
    }
    
    /**
     * Get root motion delta
     */
    glm::vec3 getRootMotionDelta(Entity entity) {
        auto* animator = getComponent<AnimatorComponent>(entity.id);
        if (animator) {
            return animator->rootMotionDelta;
        }
        return glm::vec3(0.0f);
    }
    
    const char* getName() const override { return "AnimationSystem"; }

private:
    Animator* m_animator = nullptr;
    AnimationStateMachine* m_fsm = nullptr;
    HybridMMFSM* m_hybrid = nullptr;
};

/**
 * Animation State System - Manages animation state machine
 */
class AnimationStateSystem : public TypedSystem<AnimatorComponent, AnimationStateComponent> {
public:
    AnimationStateSystem() = default;
    
    void init() override {
        m_filter = SystemFilter::require<AnimatorComponent, AnimationStateComponent>();
    }
    
    void update(float deltaTime) override {
        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID entityID, AnimatorComponent& animator, 
                              AnimationStateComponent& state) {
                updateState(animator, state, deltaTime);
            });
    }
    
    /**
     * Update animation state based on parameters
     */
    void updateState(AnimatorComponent& animator, AnimationStateComponent& state,
                     float deltaTime) {
        state.stateTime += deltaTime;
        
        // Update grounded state
        state.isIdle = (state.moveSpeed < 0.1f);
        state.isWalking = (state.moveSpeed >= 0.1f && state.moveSpeed < 0.5f);
        state.isRunning = (state.moveSpeed >= 0.5f);
        state.isJumping = (!state.isGrounded && state.verticalVelocity > 0.1f);
        state.isFalling = (!state.isGrounded && state.verticalVelocity < -0.1f);
        
        // State transitions would happen here based on FSM rules
        // For now, just update animator parameters
    }
    
    /**
     * Set movement speed parameter
     */
    void setMoveSpeed(Entity entity, float speed) {
        auto* state = getComponent<AnimationStateComponent>(entity.id);
        if (state) {
            state->moveSpeed = speed;
        }
    }
    
    /**
     * Set vertical velocity parameter
     */
    void setVerticalVelocity(Entity entity, float velocity) {
        auto* state = getComponent<AnimationStateComponent>(entity.id);
        if (state) {
            state->verticalVelocity = velocity;
        }
    }
    
    /**
     * Set grounded state
     */
    void setGrounded(Entity entity, bool grounded) {
        auto* state = getComponent<AnimationStateComponent>(entity.id);
        if (state) {
            state->isGrounded = grounded;
        }
    }
    
    /**
     * Trigger attack animation
     */
    void triggerAttack(Entity entity) {
        auto* state = getComponent<AnimationStateComponent>(entity.id);
        if (state) {
            state->isAttacking = true;
        }
    }
    
    const char* getName() const override { return "AnimationStateSystem"; }
};

/**
 * Motion Matching System - Advanced animation selection
 */
class MotionMatchingSystem : public TypedSystem<AnimatorComponent, AnimationStateComponent, TransformComponent> {
public:
    MotionMatchingSystem() = default;
    
    void setHybrid(HybridMMFSM* hybrid) {
        m_hybrid = hybrid;
    }
    
    void init() override {
        m_filter = SystemFilter::require<AnimatorComponent, AnimationStateComponent, TransformComponent>();
    }
    
    void update(float deltaTime) override {
        if (!m_hybrid) return;
        
        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID entityID, AnimatorComponent& animator, 
                              AnimationStateComponent& state, TransformComponent& transform) {
                updateMotionMatching(entityID, animator, state, transform, deltaTime);
            });
    }
    
    void updateMotionMatching(EntityID entityID, AnimatorComponent& animator,
                              AnimationStateComponent& state, TransformComponent& transform,
                              float deltaTime) {
        // Motion matching would:
        // 1. Extract current pose features
        // 2. Search database for best matching clip
        // 3. Sample and blend animation
        // 4. Extract and apply root motion
        
        // This is integrated with the existing HybridMMFSM system
    }
    
    const char* getName() const override { return "MotionMatchingSystem"; }

private:
    HybridMMFSM* m_hybrid = nullptr;
};

} // namespace ecs
