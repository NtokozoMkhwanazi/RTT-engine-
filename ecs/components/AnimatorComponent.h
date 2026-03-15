#pragma once

#include "../ECS.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>

namespace ecs {

/**
 * Bone/Joint data for skeletal animation
 */
struct Bone {
    std::string name;
    int parentIndex = -1;
    glm::mat4 inverseBindMatrix{1.0f};
    glm::mat4 localTransform{1.0f};
    glm::mat4 worldTransform{1.0f};
};

/**
 * Skeleton Component - Bone hierarchy for animation
 */
struct SkeletonComponent : public Component {
    std::vector<Bone> bones;
    int rootBoneIndex = -1;
    
    // Bone texture info (for GPU skinning)
    int boneTextureWidth = 0;
    int boneTextureHeight = 0;
    
    SkeletonComponent() = default;
    
    /**
     * Find bone by name
     */
    int findBoneIndex(const std::string& name) const {
        for (size_t i = 0; i < bones.size(); ++i) {
            if (bones[i].name == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    
    /**
     * Get bone count
     */
    size_t getBoneCount() const { return bones.size(); }
    
    /**
     * Check if skeleton is valid
     */
    bool isValid() const { return !bones.empty() && rootBoneIndex >= 0; }
};

/**
 * Animator Component - Controls animation playback
 */
struct AnimatorComponent : public Component {
    // Animation state
    int currentAnimation = -1;
    int previousAnimation = -1;
    float currentTime = 0.0f;
    float playbackSpeed = 1.0f;
    bool isPlaying = true;
    bool loop = true;
    
    // Blending
    float blendWeight = 1.0f;
    float blendDuration = 0.2f;
    float blendTime = 0.0f;
    bool isBlending = false;
    
    // Animation layers
    int activeLayer = 0;
    std::vector<int> animationLayers;
    
    // Root motion
    bool useRootMotion = true;
    glm::vec3 rootMotionDelta{0.0f};
    float rootMotionRotation = 0.0f;
    
    AnimatorComponent() = default;
    
    /**
     * Start playing an animation
     */
    void play(int animIndex, bool loopAnim = true) {
        if (animIndex == currentAnimation) {
            isPlaying = true;
            return;
        }
        
        previousAnimation = currentAnimation;
        currentAnimation = animIndex;
        currentTime = 0.0f;
        loop = loopAnim;
        isPlaying = true;
        
        // Start blending from previous animation
        if (previousAnimation >= 0) {
            isBlending = true;
            blendTime = 0.0f;
        }
    }
    
    /**
     * Stop animation
     */
    void stop() {
        isPlaying = false;
    }
    
    /**
     * Pause animation
     */
    void pause() {
        isPlaying = false;
    }
    
    /**
     * Resume animation
     */
    void resume() {
        isPlaying = true;
    }
    
    /**
     * Set animation time
     */
    void setTime(float time) {
        currentTime = time;
    }
    
    /**
     * Get normalized time (0-1)
     */
    float getNormalizedTime(float animationDuration) const {
        if (animationDuration <= 0.0f) return 0.0f;
        return currentTime / animationDuration;
    }
};

/**
 * Animation State Machine Component
 */
struct AnimationStateComponent : public Component {
    int currentState = 0;
    int previousState = 0;
    float stateTime = 0.0f;
    
    // State flags
    bool isIdle = true;
    bool isWalking = false;
    bool isRunning = false;
    bool isJumping = false;
    bool isFalling = false;
    bool isAttacking = false;
    
    // Transition parameters
    float moveSpeed = 0.0f;
    float verticalVelocity = 0.0f;
    bool isGrounded = true;
    
    AnimationStateComponent() = default;
};

} // namespace ecs
