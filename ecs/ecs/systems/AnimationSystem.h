#pragma once

/**
 * Integrated Animation System - Uses engine's Animator
 * 
 * This system syncs ECS AnimatorComponent with engine Animator
 * and uses engine's bone sampling, blending, and GPU skinning.
 */

#include "../ECS.h"
#include "../components/Components.h"
#include "../../animationSystem/Animator.h"
#include "../../animationSystem/Animation.h"
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>

namespace ecs {

/**
 * Animation System - Integrates ECS with engine Animator
 */
class AnimationSystem : public TypedSystem<AnimatorComponent, SkeletonComponent> {
public:
    AnimationSystem() = default;
    ~AnimationSystem() = default;

    void init() override {
        m_filter = SystemFilter::require<AnimatorComponent, SkeletonComponent>();
    }

    void update(float deltaTime) override {
        if (!m_entityManager || !m_componentManager) return;

        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID entityID, AnimatorComponent& animator, SkeletonComponent& skeleton) {
                updateAnimator(entityID, animator, skeleton, deltaTime);
            });
    }

    /**
     * Update a single animator
     */
    void updateAnimator(EntityID entityID, AnimatorComponent& animator, SkeletonComponent& skeleton, float deltaTime) {
        // Get or create engine Animator for this entity
        auto it = m_animators.find(entityID);
        if (it == m_animators.end()) {
            // Create new engine Animator
            createAnimator(entityID, animator, skeleton);
            it = m_animators.find(entityID);
        }

        if (it != m_animators.end() && it->second) {
            auto& engineAnimator = it->second;

            // Sync state from ECS component to engine animator
            syncToEngine(animator, *engineAnimator);

            // Update engine animator (samples bones, applies blending, foot IK, etc.)
            engineAnimator->Update(deltaTime);

            // Sync bone matrices back to ECS skeleton
            syncFromEngine(*engineAnimator, skeleton);
        }
    }

    /**
     * Create engine Animator for an entity
     */
    void createAnimator(EntityID entityID, AnimatorComponent& animator, SkeletonComponent& skeleton) {
        // Create engine skeleton from ECS skeleton
        auto engineSkeleton = createEngineSkeleton(skeleton);
        
        // Create engine animator
        auto engineAnimator = std::make_unique<Animator>(engineSkeleton.get());

        // Add animations to engine animator
        for (Animation* anim : animator.animations) {
            if (anim) {
                engineAnimator->Play(anim);
            }
        }

        m_animators[entityID] = std::move(engineAnimator);
        m_skeletons[entityID] = std::move(engineSkeleton);
    }

    /**
     * Create engine Skeleton from ECS SkeletonComponent
     */
    std::unique_ptr<Skeleton> createEngineSkeleton(SkeletonComponent& skeleton) {
        auto engineSkeleton = std::make_unique<Skeleton>();
        
        // Copy bone count
        engineSkeleton->bones.resize(skeleton.bones.size());
        engineSkeleton->rootBoneIndex = skeleton.rootBoneIndex;

        // Copy bone data
        for (size_t i = 0; i < skeleton.bones.size(); i++) {
            // bones[i] returns a BoneRef proxy by value (BoneSoA storage); bind it
            // via auto&& so the proxy (and its embedded references) live for the loop.
            auto&& ecsBone = skeleton.bones[i];
            auto& engineBone = engineSkeleton->bones[i];

            engineBone.id = static_cast<int>(i);
            engineBone.bindTransform = ecsBone.inverseBindMatrix;
            engineBone.offset = glm::inverse(ecsBone.inverseBindMatrix);
        }

        return engineSkeleton;
    }

    /**
     * Sync ECS AnimatorComponent to engine Animator
     */
    void syncToEngine(AnimatorComponent& animator, Animator& engineAnimator) {
        // Sync playback state
        if (engineAnimator.GetCurrentTime() != animator.currentTime) {
            engineAnimator.SetCurrentTime(animator.currentTime);
        }

        // Handle play/pause
        if (animator.isPlaying && engineAnimator.GetCurrentTime() == engineAnimator.GetCurrentTime()) {
            // Already playing
        }

        // Handle animation changes
        if (animator.currentAnimation >= 0 && 
            animator.currentAnimation < static_cast<int>(animator.animations.size())) {
            
            Animation* currentAnim = animator.animations[animator.currentAnimation];
            if (currentAnim) {
                // Would need to check if this is already playing
                // For now, assume ECS state is authoritative
            }
        }
    }

    /**
     * Sync bone matrices from engine Animator to ECS SkeletonComponent
     */
    void syncFromEngine(Animator& engineAnimator, SkeletonComponent& skeleton) {
        // Get final bone matrices from engine animator
        const auto& finalMatrices = engineAnimator.GetFinalBoneMatrices();
        
        // Copy to ECS skeleton
        size_t count = std::min(finalMatrices.size(), skeleton.bones.size());
        for (size_t i = 0; i < count; i++) {
            skeleton.bones[i].worldTransform = finalMatrices[i];
        }
    }

    /**
     * Play an animation on an entity
     */
    void playAnimation(Entity entity, int animationIndex, bool loop = true) {
        auto* animator = m_componentManager->getComponent<AnimatorComponent>(entity.id);
        if (animator) {
            animator->play(animationIndex, loop);
        }

        // Also update engine animator if it exists
        auto it = m_animators.find(entity.id);
        if (it != m_animators.end() && it->second) {
            if (animationIndex >= 0 && animationIndex < static_cast<int>(animator->animations.size())) {
                it->second->Play(animator->animations[animationIndex]);
            }
        }
    }

    /**
     * Stop animation on an entity
     */
    void stopAnimation(Entity entity) {
        auto* animator = m_componentManager->getComponent<AnimatorComponent>(entity.id);
        if (animator) {
            animator->stop();
        }
    }

    /**
     * Blend to a new animation
     */
    void blendToAnimation(Entity entity, int animationIndex, float blendDuration, bool loop = true) {
        auto* animator = m_componentManager->getComponent<AnimatorComponent>(entity.id);
        if (animator) {
            animator->previousAnimation = animator->currentAnimation;
            animator->currentAnimation = animationIndex;
            animator->isBlending = true;
            animator->blendTime = 0.0f;
            animator->blendDuration = blendDuration;
            animator->loop = loop;
        }

        // Also update engine animator
        auto it = m_animators.find(entity.id);
        if (it != m_animators.end() && it->second) {
            if (animationIndex >= 0 && animationIndex < static_cast<int>(animator->animations.size())) {
                it->second->BlendTo(animator->animations[animationIndex], blendDuration);
            }
        }
    }

    /**
     * Add animation to entity
     */
    void addAnimation(Entity entity, Animation* anim) {
        auto* animator = m_componentManager->getComponent<AnimatorComponent>(entity.id);
        if (animator && anim) {
            animator->addAnimation(anim);
        }

        // Also add to engine animator
        auto it = m_animators.find(entity.id);
        if (it != m_animators.end() && it->second) {
            it->second->Play(anim);
        }
    }

    /**
     * Set animation playback speed
     */
    void setPlaybackSpeed(Entity entity, float speed) {
        auto* animator = m_componentManager->getComponent<AnimatorComponent>(entity.id);
        if (animator) {
            animator->playbackSpeed = speed;
        }
    }

    /**
     * Get current animation time
     */
    float getAnimationTime(Entity entity) const {
        auto* animator = m_componentManager->getComponent<AnimatorComponent>(entity.id);
        return animator ? animator->currentTime : 0.0f;
    }

    /**
     * Check if animation is playing
     */
    bool isPlaying(Entity entity) const {
        auto* animator = m_componentManager->getComponent<AnimatorComponent>(entity.id);
        return animator ? animator->isPlaying : false;
    }

    const char* getName() const override { return "AnimationSystem (Integrated)"; }

private:
    // Map from ECS entity ID to engine Animator
    std::unordered_map<EntityID, std::unique_ptr<Animator>> m_animators;
    std::unordered_map<EntityID, std::unique_ptr<Skeleton>> m_skeletons;
    // NOTE(#L1075): per-bone pointer map was removed - it took the address of a
    // BoneRef proxy (a temporary) into a dead map and was never read. Bone sync
    // is value-based (by index) via syncFromEngine, so no indirection map is needed.
};

} // namespace ecs
