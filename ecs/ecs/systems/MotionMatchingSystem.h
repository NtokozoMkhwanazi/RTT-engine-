#pragma once

/**
 * Motion Matching System - ECS adapter over the engine's motionMatching/MotionMatcher
 *
 * This system is a thin adapter. All pose search, trajectory prediction, KD-tree
 * matching, blending, and foot IK are delegated to a per-entity
 * motionMatching::MotionMatcher (the tested source of truth — see
 * motionMatching/MotionMatcher.h and tests/test_motion_matching.cpp).
 *
 * The previous version of this file re-implemented clip selection with stubs:
 *   - searchBestMatchingClip() was a no-op, despite a comment claiming it
 *     "uses the engine's MotionMatcher for optimal clip selection".
 *   - getClipDuration() returned a hardcoded 5.0f, with the comment
 *     "In production, this would query MotionDatabase::getClip(clipID)->duration".
 *   - shouldSearchForNewClip() depended on that fake duration.
 * Those stubs have been removed; clip selection now flows through
 * MotionMatcher::Update (which queries the KD-tree database and blends poses).
 */

#include "../ECS.h"
#include "../components/Components.h"          // SkeletonComponent, AnimatorComponent, ...
#include "../../motionMatching/MotionMatcher.h" // MotionMatcher + CharacterState/CharacterInput
#include "../../motionMatching/MotionDatabase.h"
#include "../../animationSystem/Animator.h"      // engine ::Animator (root-motion integration)
#include "../../boneSystem/Skeleton.h"           // engine ::Skeleton type
#include "ECSAnimationBridge.h"                  // makeEngineSkeleton()
#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>

namespace ecs {

/**
 * Motion Matching System - Integrates ECS with the engine MotionMatcher.
 *
 * Lazily creates one MotionMatcher (+ engine Skeleton + engine Animator) per
 * matching entity (mirroring how AnimationSystem creates per-entity Animators),
 * then drives pose selection through MotionMatcher::Update.
 */
class MotionMatchingSystem : public TypedSystem<TransformComponent, MotionMatchingComponent, AnimatorComponent, SkeletonComponent> {
public:
    MotionMatchingSystem() = default;

    /** Inject a motion database; the matcher queries this for pose search. */
    void setMotionDatabase(MotionDatabase* db) { m_database = db; }
    MotionDatabase* getMotionDatabase() const { return m_database; }

    void setMotionDatabaseID(int id) { m_motionDatabaseID = id; }
    int getMotionDatabaseID() const { return m_motionDatabaseID; }

    void init() override {
        m_filter = SystemFilter::require<TransformComponent, MotionMatchingComponent,
                                          AnimatorComponent, SkeletonComponent>();
    }

    void update(float deltaTime) override {
        if (!m_entityManager || !m_componentManager) return;

        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID entityID, TransformComponent& transform,
                              MotionMatchingComponent& mm, AnimatorComponent& animator,
                              SkeletonComponent& skeleton) {
                updateMotionMatching(entityID, transform, mm, animator, skeleton, deltaTime);
            });
    }

    /**
     * Update motion matching for a single character.
     *
     * All clip selection / blending is delegated to the engine MotionMatcher.
     * The previous stubs (searchBestMatchingClip / getClipDuration) are gone —
     * the matcher's Update() performs the real KD-tree search.
     */
    void updateMotionMatching(
        EntityID entityID,
        TransformComponent& transform,
        MotionMatchingComponent& mm,
        AnimatorComponent& animator,
        SkeletonComponent& skeleton,
        float deltaTime)
    {
        if (!mm.enabled) return;

        MotionMatcher* matcher = getOrCreateMatcher(entityID, animator, skeleton);
        if (!matcher || !matcher->IsInitialized()) {
            // No matcher/database yet. Keep ECS time ticking, but do NOT fall
            // back to a fake clip-duration search — the matcher owns selection.
            mm.currentClipTime += deltaTime;
            return;
        }

        // Build world-space character state from ECS components; the matcher
        // rotates it into the clip/root frame internally.
        CharacterState state;
        state.position      = transform.position;
        state.velocity      = mm.rootVelocity;
        state.worldVelocity = mm.rootVelocity;
        state.moveDirection = mm.trajectoryDirection;
        state.grounded      = true;

        matcher->Update(deltaTime, state);

        // Sync selection results back into the ECS component.
        mm.currentClipID   = matcher->GetCurrentPoseIndex();
        mm.currentClipTime = matcher->GetCurrentAnimationTime();

        // Apply trajectory / root motion to the transform (ECS-side integration
        // of the velocity the matcher produced this frame).
        if (mm.useTrajectoryPrediction) {
            applyRootMotion(transform, mm, deltaTime);
        }
    }

    /**
     * Set desired movement direction.
     */
    void setMovementDirection(MotionMatchingComponent& mm, const glm::vec2& direction) {
        mm.trajectoryDirection = glm::normalize(direction);
    }

    /**
     * Set desired movement speed.
     */
    void setMovementSpeed(MotionMatchingComponent& mm, float speed) {
        mm.trajectorySpeed = speed;
    }

    /**
     * Start blending to a new clip.
     */
    void blendToClip(MotionMatchingComponent& mm, int newClipID, float blendDuration = 0.3f) {
        mm.isBlending = true;
        mm.blendTime = 0.0f;
        mm.blendDuration = blendDuration;
        mm.blendFromClip = mm.currentClipID;
        mm.blendToClip = newClipID;
        mm.currentClipID = newClipID;
        mm.currentClipTime = 0.0f;
    }

    const char* getName() const override { return "MotionMatchingSystem"; }

private:
    /**
     * Lazily create (and cache) the engine MotionMatcher for an entity.
     *
     * This mirrors AnimationSystem's per-entity Animator creation, reusing the
     * shared makeEngineSkeleton() bridge. The engine Skeleton is kept alive
     * (m_skeletons) so the matcher's and animator's borrowed pointers stay valid.
     */
    MotionMatcher* getOrCreateMatcher(EntityID entityID,
                                      AnimatorComponent& animator,
                                      SkeletonComponent& skeleton) {
        auto it = m_matchers.find(entityID);
        if (it != m_matchers.end()) {
            return it->second.get();
        }

        if (!skeleton.isValid()) {
            return nullptr;
        }

        // Build the engine skeleton + animator the matcher requires.
        auto engineSkeleton = makeEngineSkeleton(skeleton);
        auto engineAnimator = std::make_unique<Animator>(engineSkeleton.get());
        for (size_t i = 0; i < animator.animations.size(); ++i) {
            Animation* anim = animator.animations[i];
            if (anim) {
                engineAnimator->Play(anim);
            }
        }

        auto matcher = std::make_unique<MotionMatcher>();
        matcher->Initialize(engineSkeleton.get(), engineAnimator.get());
        if (m_database) {
            matcher->SetCurrentDatabase(*m_database);
            matcher->BuildSearchIndex();
        }

        MotionMatcher* raw = matcher.get();
        m_matchers[entityID]  = std::move(matcher);
        m_animators[entityID] = std::move(engineAnimator);
        m_skeletons[entityID] = std::move(engineSkeleton);
        return raw;
    }

    /** Apply root-motion velocity to the transform (ECS-side integration). */
    void applyRootMotion(TransformComponent& transform, MotionMatchingComponent& mm, float deltaTime) {
        glm::vec3 rootDelta = mm.rootVelocity * deltaTime;
        transform.position += rootDelta;
    }

    std::unordered_map<EntityID, std::unique_ptr<MotionMatcher>> m_matchers;
    std::unordered_map<EntityID, std::unique_ptr<Animator>>     m_animators;
    std::unordered_map<EntityID, std::unique_ptr<Skeleton>>     m_skeletons;

    MotionDatabase* m_database = nullptr;   // non-owning; the engine's motion database
    int m_motionDatabaseID = -1;
};

} // namespace ecs
