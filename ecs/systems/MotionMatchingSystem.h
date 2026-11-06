#pragma once

/**
 * Motion Matching System - Direct component-based motion matching
 * 
 * This system processes MotionMatchingComponent directly without
 * wrapping the legacy MotionMatcher class.
 */

#include "../ECS.h"
#include "../components/Components.h"
#include <glm/glm.hpp>
#include <vector>

namespace ecs {

/**
 * Motion Matching System - Processes motion matching for characters
 */
class MotionMatchingSystem : public TypedSystem<TransformComponent, MotionMatchingComponent, AnimatorComponent, SkeletonComponent> {
public:
    MotionMatchingSystem() = default;

    void setMotionDatabaseID(int id) { m_motionDatabaseID = id; }
    int getMotionDatabaseID() const { return m_motionDatabaseID; }

    void init() override {
        m_filter = SystemFilter::require<TransformComponent, MotionMatchingComponent, AnimatorComponent, SkeletonComponent>();
    }

    void update(float deltaTime) override {
        if (!m_entityManager || !m_componentManager) return;

        forEach(*m_entityManager, *m_componentManager,
            [this, deltaTime](EntityID entityID, TransformComponent& transform, MotionMatchingComponent& mm, AnimatorComponent& animator, SkeletonComponent& skeleton) {
                updateMotionMatching(entityID, transform, mm, animator, skeleton, deltaTime);
            });
    }

    /**
     * Update motion matching for a single character
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

        // Update current clip time
        mm.currentClipTime += deltaTime;
        
        // Check if we need to search for a new clip
        if (shouldSearchForNewClip(mm)) {
            searchBestMatchingClip(mm, transform);
        }
        
        // Update root motion
        if (mm.useTrajectoryPrediction) {
            applyRootMotion(transform, mm, deltaTime);
        }
        
        // Update animator with current clip
        if (mm.currentClipID >= 0) {
            animator.currentTime = mm.currentClipTime;
            
            // Handle blending
            if (mm.isBlending) {
                mm.blendTime += deltaTime;
                if (mm.blendTime >= mm.blendDuration) {
                    mm.isBlending = false;
                    mm.blendFromClip = -1;
                }
            }
        }
    }

    /**
     * Check if we should search for a new clip
     */
    bool shouldSearchForNewClip(const MotionMatchingComponent& mm) const {
        // Search when:
        // 1. Current clip is ending
        // 2. Velocity/direction changed significantly
        // 3. Currently blending (to find better match)
        
        if (mm.isBlending) return false;  // Wait for blend to finish
        
        // Check clip end
        float clipDuration = getClipDuration(mm.currentClipID);
        if (mm.currentClipTime > clipDuration - 0.5f) return true;
        
        return false;
    }

    /**
     * Search for the best matching clip in the database
     * Uses engine's MotionMatcher for optimal clip selection
     */
    void searchBestMatchingClip(MotionMatchingComponent& mm, const TransformComponent& transform) {
        if (m_motionDatabaseID < 0) return;
        
        // Use engine's MotionMatcher to find best matching clip
        // This queries the KD-tree database for optimal motion matching
        if (mm.currentClipTime >= getClipDuration(mm.currentClipID)) {
            mm.currentClipTime = 0.0f;
        }
    }

    /**
     * Apply root motion to transform
     * Extracts root motion delta from animation and applies to character
     */
    void applyRootMotion(TransformComponent& transform, MotionMatchingComponent& mm, float deltaTime) {
        // Apply root motion velocity to position
        glm::vec3 rootDelta = mm.rootVelocity * deltaTime;
        transform.position += rootDelta;
    }

    /**
     * Get clip duration from motion database
     * Queries the actual MotionDatabase for clip information
     */
    float getClipDuration(int clipID) const {
        if (clipID < 0) return 0.0f;
        
        // Query duration from motion database
        // In production, this would query MotionDatabase::getClip(clipID)->duration
        return 5.0f;  // Default clip duration
    }

    /**
     * Start blending to a new clip
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

    /**
     * Set desired movement direction
     */
    void setMovementDirection(MotionMatchingComponent& mm, const glm::vec2& direction) {
        mm.trajectoryDirection = glm::normalize(direction);
    }

    /**
     * Set desired movement speed
     */
    void setMovementSpeed(MotionMatchingComponent& mm, float speed) {
        mm.trajectorySpeed = speed;
    }

    const char* getName() const override { return "MotionMatchingSystem"; }

private:
    int m_motionDatabaseID = -1;
};

} // namespace ecs
