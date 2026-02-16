#pragma once
#include <glad/glad.h>

#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <map>
#include <functional>
#include <set>

#include "Animation.h"
#include "shaderSystem/Shader.h"
#include "boneSystem/Skeleton.h"

class Animator
{
public:
    Animator(const Skeleton *skeleton);

    void Play(Animation *animation);
    void BlendTo(Animation *animation, float duration);
    void BlendToWithWeight(Animation *animation, float targetWeight, float duration);
    void AddAnimationLayer(Animation *animation, float weight = 1.0f, float blendDuration = 0.3f);

    void Update(float dt);

    void UploadToTexture(GLuint texID);
    void Upload(Shader &shader);

    // -------- FOOT LOCKING / IK --------
    void AddIKOffset(int bone, const glm::vec3 &offset, float weight);
    bool IsFootPlanted(int bone) const;
    glm::vec3 GetBoneWorldPosition(int bone, const glm::mat4 &modelMat) const;

    glm::vec3 ConsumeRootMotion();

    const std::vector<glm::mat4> &GetFinalBoneMatrices() const;

    // Animation blending utilities
    void SetAnimationWeight(int layerIndex, float weight);
    void RemoveAnimationLayer(int layerIndex);
    int GetActiveAnimationCount(void) const { return (int)activeAnimations.size(); }

    // Animation Events
    struct AnimationEventTrigger {
        Animation* animation;
        float time;
        std::string eventName;
        std::function<void()> callback;  // Custom callback function
        bool triggered = false;          // Whether this event has been triggered in current cycle
        bool repeatable = true;          // Whether this event can trigger multiple times
        
        bool operator<(const AnimationEventTrigger& other) const {
            if (animation != other.animation) return animation < other.animation;
            if (time != other.time) return time < other.time;
            return eventName < other.eventName;
        }
    };
    
    using AnimationEventCallback = std::function<void(const std::string&)>;
    void RegisterAnimationEvent(Animation* animation, float time, const std::string& eventName);
    void RegisterAnimationEventWithCallback(Animation* animation, float time, const std::string& eventName, std::function<void()> callback);
    void SetAnimationEventCallback(const AnimationEventCallback& callback);
    void UnregisterAnimationEvent(Animation* animation, float time, const std::string& eventName);
    void ClearAnimationEvents();
    void SetEventRepeatable(Animation* animation, const std::string& eventName, bool repeatable);
    
    // Enhanced layering system
    void SetLayerPriority(int layerIndex, int priority);
    void SetLayerAdditive(int layerIndex, bool additive);
    void SetLayerBoneMask(int layerIndex, const std::vector<bool>& mask);
    void FadeInLayer(int layerIndex, float duration);
    void FadeOutLayer(int layerIndex, float duration);
    void EnableLayer(int layerIndex, bool enable);
    int GetLayerByAnimation(Animation* animation) const;
    
    // Advanced blending features
    enum class BlendType {
        LINEAR,
        ADDITIVE,
        DIRECTIONAL,
        MASKED
    };
    
    void SetLayerBlendType(int layerIndex, BlendType blendType);
    void SetDirectionalBlendParams(int layerIndex, const glm::vec2& direction, float angleThreshold = 45.0f);
    void SetUpperLowerBodySeparation(int layerIndex, bool upperBodyOnly = false, bool lowerBodyOnly = false);
    void CrossFadeBetweenLayers(int fromLayer, int toLayer, float duration);
    void SetLayerSyncPoint(int layerIndex, Animation* animation, float syncTime);
    
    // Animation retargeting
    Animation RetargetAnimationToSkeleton(const Animation& sourceAnim, const Skeleton& targetSkel, float scale = 1.0f);
    
    // Mixamo-specific retargeting with pose correction
    Animation RetargetMixamoAnimationToSkeleton(const Animation& sourceAnim, const Skeleton& targetSkel, float scale = 1.0f);
    
    // Normalize animation to standard pose (fix T-pose/A-pose issues)
    Animation NormalizeAnimation(const Animation& sourceAnim, const Skeleton& skeleton);
    
    // Animation Quality of Service
    enum class AnimationQualityLevel {
        HIGH,     // Full detail, all bones, full keyframe interpolation
        MEDIUM,   // Reduced keyframes, some bones may be simplified
        LOW,      // Minimal bones, heavily compressed
        VERY_LOW  // Only essential bones, maximum compression
    };
    
    void SetAnimationQuality(AnimationQualityLevel level);
    AnimationQualityLevel GetAnimationQuality() const { return qualityLevel; }
    void SetDistanceBasedLOD(float nearDist, float farDist);
    void SetBoneLODThreshold(int highDetailBones, int lowDetailBones);
    void UpdateAnimationLOD(const glm::vec3& viewerPosition, const glm::vec3& modelPosition);
    
private:
    // Animation Quality of Service
    AnimationQualityLevel qualityLevel = AnimationQualityLevel::HIGH;
    float lodNearDistance = 10.0f;
    float lodFarDistance = 30.0f;
    int highDetailBoneThreshold = 50;
    int lowDetailBoneThreshold = 20;
    bool lodEnabled = true;

private:
    struct AnimationLayer
    {
        Animation *animation;
        float weight;
        float targetWeight;
        float blendProgress;
        float blendDuration;
        float time;
        float normalizedWeight;
        
        // Advanced layering features
        int priority = 0;                    // Higher priority layers blend over lower ones
        bool additive = false;               // Whether this layer is additive
        std::vector<bool> boneMask;          // Which bones this layer affects
        float fadeInDuration = 0.0f;         // Time to fade this layer in
        float fadeOutDuration = 0.0f;        // Time to fade this layer out
        bool enabled = true;                 // Whether this layer is currently enabled
        
        // Advanced blending features
        BlendType blendType = BlendType::LINEAR;
        glm::vec2 direction = glm::vec2(0.0f, 1.0f);  // Movement direction for directional blending
        float angleThreshold = 45.0f;                  // Threshold for directional blending
        bool upperBodyOnly = false;                    // Only affect upper body bones
        bool lowerBodyOnly = false;                    // Only affect lower body bones
        float syncTime = 0.0f;                         // Sync point for animation synchronization

        AnimationLayer(Animation *anim, float w, float dur)
            : animation(anim), weight(w), targetWeight(w), blendProgress(0.0f), blendDuration(dur), time(0.0f), normalizedWeight(1.0f) {}
    };

    const Skeleton *skeleton = nullptr;
    Animation *current = nullptr;
    Animation *next = nullptr;
    std::vector<AnimationLayer> activeAnimations;
    std::vector<AnimationLayer> queuedAnimations; // For smooth transitions

    std::vector<glm::vec3> m_BoneWorldPositions;

    float animatorTime = 0.0f;
    float blendTime = 0.0f;
    float blendDuration = 0.0f;

    glm::vec3 rootMotionDelta{0.0f};
    glm::vec3 prevRootPos{0.0f};
    bool hasPrevRoot = false;

    std::vector<glm::mat4> finalBoneMatrices;

public: // temporarily for debug purposes
    std::vector<glm::mat4> globalBoneMatrices;
    // -------- FOOT STATE --------
    std::vector<glm::vec3> prevBoneWorldPos;
    std::vector<glm::vec3> currBoneWorldPos;
    std::vector<glm::vec3> ikOffsets;

private:                                  // temporarily
    bool debugForceIdentityScale = false; // Test if scale animation is corrupting legs

    // Animation Events
    std::set<AnimationEventTrigger> animationEvents;
    AnimationEventCallback eventCallback;

    // Animation blending improvements
    void UpdateAnimationBlending(float dt);
    
    // Performance optimizations
    void SetCachingEnabled(bool enabled) { useCaching = enabled; }
    void ClearCache();
    
    // Animation compression utilities
    void SetCompressionEnabled(bool enabled) { compressionEnabled = enabled; }
    bool IsCompressionEnabled() const { return compressionEnabled; }
    
private:
    // Animation compression
    bool compressionEnabled = false;
    // Performance optimizations
    bool useCaching = true;
    std::vector<float> cachedAnimationTimes;
    std::vector<std::vector<glm::mat4>> cachedBoneTransforms;
    bool cacheValid = false;

    void EvaluateNode(
        const AssimpNodeData &node,
        const glm::mat4 &parent,
        Animation *blendAnim,
        float blendFactor);

    // static std::string NormalizeName(const std::string& name);
};
