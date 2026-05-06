#pragma once

#include "../ECS.h"
#include "../DynamicTypes.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <string>
#include <map>
#include <cstring>

#include "../../animationSystem/Animation.h"

namespace ecs {

constexpr size_t MAX_BONE_NAME_LENGTH = 32;

struct Bone {
    char name[MAX_BONE_NAME_LENGTH];
    int parentIndex = -1;
    glm::mat4 inverseBindMatrix{1.0f};
    glm::mat4 localTransform{1.0f};
    glm::mat4 worldTransform{1.0f};
    
    Bone() { name[0] = '\0'; }
    
    Bone(const std::string& n, int parent) : parentIndex(parent) {
        setName(n);
    }
    
    void setName(const std::string& n) {
        std::strncpy(name, n.c_str(), MAX_BONE_NAME_LENGTH - 1);
        name[MAX_BONE_NAME_LENGTH - 1] = '\0';
    }
    
    const char* getName() const { return name; }
    
    bool hasName() const { return name[0] != '\0'; }
};

struct SkeletonComponent : public Component {
    DynamicVector<Bone> bones;
    int rootBoneIndex = -1;

    int boneTextureWidth = 0;
    int boneTextureHeight = 0;

    SkeletonComponent() = default;

    int findBoneIndex(const std::string& name) const {
        for (size_t i = 0; i < bones.size(); ++i) {
            if (std::strcmp(bones[i].name, name.c_str()) == 0) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    size_t getBoneCount() const { return bones.size(); }

    bool isValid() const { return !bones.empty() && rootBoneIndex >= 0; }
    
    void setPool(DynamicPool* pool) { bones.setPool(pool); }
};

struct AnimatorComponent : public Component {
    DynamicVector<Animation*> animations;
    int currentAnimation = -1;
    int previousAnimation = -1;
    
    float currentTime = 0.0f;
    float playbackSpeed = 1.0f;
    bool isPlaying = true;
    bool loop = true;

    float blendWeight = 1.0f;
    float blendDuration = 0.2f;
    float blendTime = 0.0f;
    bool isBlending = false;

    int activeLayer = 0;
    DynamicVector<int> animationLayers;

    bool useRootMotion = true;
    glm::vec3 rootMotionDelta{0.0f};
    float rootMotionRotation = 0.0f;

    AnimatorComponent() = default;
    
    void setPool(DynamicPool* pool) { 
        animations.setPool(pool); 
        animationLayers.setPool(pool);
    }

    void addAnimation(Animation* anim) {
        if (anim) {
            animations.push_back(anim);
        }
    }

    Animation* getAnimation(int index) {
        if (index >= 0 && index < static_cast<int>(animations.size())) {
            return animations[index];
        }
        return nullptr;
    }

    Animation* getCurrentAnimation() {
        return getAnimation(currentAnimation);
    }

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

        if (previousAnimation >= 0) {
            isBlending = true;
            blendTime = 0.0f;
        }
    }

    void stop() { isPlaying = false; }
    void pause() { isPlaying = false; }
    void resume() { isPlaying = true; }
    void setTime(float time) { currentTime = time; }

    float getAnimationDuration() const {
        if (currentAnimation >= 0 && currentAnimation < static_cast<int>(animations.size())) {
            if (animations[currentAnimation]) {
                return animations[currentAnimation]->GetDuration();
            }
        }
        return 0.0f;
    }

    float getNormalizedTime() const {
        float duration = getAnimationDuration();
        if (duration <= 0.0f) return 0.0f;
        return currentTime / duration;
    }
};

struct AnimationStateComponent : public Component {
    int currentState = 0;
    int previousState = 0;
    float stateTime = 0.0f;

    bool isIdle = true;
    bool isWalking = false;
    bool isRunning = false;
    bool isJumping = false;
    bool isFalling = false;
    bool isAttacking = false;

    float moveSpeed = 0.0f;
    float verticalVelocity = 0.0f;
    bool isGrounded = true;

    AnimationStateComponent() = default;
};

} // namespace ecs
