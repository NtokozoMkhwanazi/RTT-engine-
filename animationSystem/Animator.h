#pragma once
#include <glad/glad.h>

#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <map>
#include <functional>
#include <set>
#include <memory>

#include "Animation.h"
#include "../shaderSystem/Shader.h"
#include "../boneSystem/Skeleton.h"
#include "BoneMatrixBuffer.h"

class Animator
{
public:
    Animator(const Skeleton *skeleton);
    ~Animator();

    void Play(Animation *animation);
    void BlendTo(Animation *animation, float duration);
    void BlendToWithWeight(Animation *animation, float targetWeight, float duration);
    void BlendTwoAnimations(Animation *anim1, float weight1, Animation *anim2, float weight2, float dt);
    void AddAnimationLayer(Animation *animation, float weight = 1.0f, float blendDuration = 0.3f);
    
    // Time control
    void SetCurrentTime(float time);
    float GetCurrentTime() const { return animatorTime; }

    void Update(float dt);

    void UploadToTexture(GLuint texID);
    void Upload(Shader &shader);

    // -------- FOOT LOCKING / IK --------
    void AddIKOffset(int bone, const glm::vec3 &offset, float weight);
    bool IsFootPlanted(int bone) const;
    bool IsCharacterGrounded() const;  // Check if character has at least one foot planted
    glm::vec3 GetBoneWorldPosition(int bone, const glm::mat4 &modelMat) const;

    // -------- FOOT IK SYSTEM --------
    struct FootIKSettings {
        bool enabled = false;
        float floorHeight = 0.0f;
        float ikStrength = 1.0f;
        float footLockBlend = 0.8f;      // How much to lock foot when planted
        float ankleFKWeight = 0.5f;       // Blend between FK ankle and IK ankle
        float maxIKDistance = 0.15f;      // Max distance foot can reach
        float footLockReleaseSpeed = 2.0f;
        int leftFootBone = -1;
        int rightFootBone = -1;
        int leftToeBone = -1;
        int rightToeBone = -1;
    };
    
    void SetFootIKEnabled(bool enabled);
    void SetFootIKSettings(const FootIKSettings& settings);
    void SetFloorHeight(float height);
    void SetFootBones(int leftFoot, int rightFoot, int leftToe = -1, int rightToe = -1);
    void UpdateFootIK(float dt, const glm::mat4& modelMatrix, bool isMoving = false);  // isMoving disables foot lock
    void DebugDrawFootIK();  // Call after rendering to debug

    glm::vec3 ConsumeRootMotion();

    // Root motion control - lock root bone position to prevent sliding
    void SetRootMotionEnabled(bool enabled) { rootMotionEnabled = enabled; }
    bool IsRootMotionEnabled() const { return rootMotionEnabled; }
    void SetLockRootPosition(bool lock) { lockRootPosition = lock; }
    bool IsRootPositionLocked() const { return lockRootPosition; }

    const std::vector<glm::mat4> &GetFinalBoneMatrices() const;

    // =========================================================================
    // BONE MATRIX BUFFER (UBO/SSBO) - PREFERRED METHOD
    // =========================================================================
    
    /**
     * Initialize bone buffer for fast matrix uploads
     * Auto-selects UBO (<=120 bones) or SSBO (>120 bones)
     * 
     * @param maxBones Maximum bone capacity (default 120)
     * @param config Configuration options (preferSSBO for crowds)
     * @return true if successful
     */
    bool InitializeBoneBuffer(size_t maxBones = 120, const BoneBufferConfig& config = BoneBufferConfig());
    
    /**
     * Update bone buffer with current matrices
     * 
     * @return true if successful
     */
    bool UpdateBoneBuffer();
    
    /**
     * Bind bone buffer to shader
     * 
     * @param bindingPoint Binding point (default 0)
     */
    void BindBoneBuffer(GLuint bindingPoint = 0) const;
    
    /**
     * Check if bone buffer is available
     */
    bool HasBoneBuffer() const { return boneBuffer != nullptr && boneBuffer->IsInitialized(); }
    
    /**
     * Get bone buffer statistics
     */
    void PrintBoneBufferStats() const;
    
    // =========================================================================
    // DEPRECATED: Old bone upload methods (use BoneBuffer instead)
    // =========================================================================
    
    [[deprecated("Use InitializeBoneBuffer() instead")]]
    bool InitializeBoneUBO(size_t maxBones = 120) { return InitializeBoneBuffer(maxBones); }
    
    [[deprecated("Use UpdateBoneBuffer() instead")]]
    bool UpdateBoneUBO() { return UpdateBoneBuffer(); }
    
    [[deprecated("Use BindBoneBuffer() instead")]]
    void BindBoneUBO(GLuint bindingPoint = 0) const { BindBoneBuffer(bindingPoint); }
    
    [[deprecated("Use HasBoneBuffer() instead")]]
    bool HasBoneUBO() const { return HasBoneBuffer(); }
    
    [[deprecated("Use PrintBoneBufferStats() instead")]]
    void PrintBoneUBOStats() const { PrintBoneBufferStats(); }

    // Animation blending utilities
    void SetAnimationWeight(int layerIndex, float weight);
    void RemoveAnimationLayer(int layerIndex);
    int GetActiveAnimationCount(void) const { return (int)activeAnimations.size(); }

    // Animation state queries (for testing and debugging)
    Animation* GetCurrentAnimation() const;
    int GetActiveAnimationLayerCount() const { return (int)activeAnimations.size(); }
    float GetActiveAnimationTime(int layerIndex = 0) const;

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

    // Animation layer structure (public for testing)
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

    // TRS structure for fast multi-layer blending (avoids matrix decompose/recompose)
    struct BoneTRS {
        glm::vec3 translation{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};
    };

private:
    // Animation Quality of Service
    AnimationQualityLevel qualityLevel = AnimationQualityLevel::HIGH;
    float lodNearDistance = 10.0f;
    float lodFarDistance = 30.0f;
    int highDetailBoneThreshold = 50;
    int lowDetailBoneThreshold = 20;
    bool lodEnabled = true;

private:
    const Skeleton *skeleton = nullptr;
    Animation *current = nullptr;
    Animation *next = nullptr;
    std::vector<AnimationLayer> activeAnimations;
    std::vector<AnimationLayer> queuedAnimations; // For smooth transitions

    std::vector<glm::vec3> m_BoneWorldPositions;

    float animatorTime = 0.0f;
    float blendTime = 0.0f;
    float blendDuration = 0.0f;

    glm::vec3 prevRootPos{0.0f};
    glm::vec3 rootMotionDelta{0.0f};
    bool hasPrevRoot = false;

    std::vector<glm::mat4> finalBoneMatrices;

public: // temporarily for debug purposes
    std::vector<glm::mat4> globalBoneMatrices;
    // -------- FOOT STATE --------
    std::vector<glm::vec3> prevBoneWorldPos;
    std::vector<glm::vec3> currBoneWorldPos;
    std::vector<glm::vec3> ikOffsets;

    // -------- FOOT IK STATE --------
    FootIKSettings footIKSettings;
    struct FootIKState {
        glm::vec3 lockedPosition{0.0f};
        glm::vec3 targetPosition{0.0f};
        glm::vec3 ankleOffset{0.0f};
        float lockWeight = 0.0f;  // 0 = fully unlocked, 1 = fully locked
        bool isLocked = false;
        float timeSinceLock = 0.0f;
    };
    FootIKState leftFootIK;
    FootIKState rightFootIK;

private:                                  // temporarily
    bool debugForceIdentityScale = false; // Test if scale animation is corrupting legs

    // Bone Matrix Buffer (UBO/SSBO)
    std::unique_ptr<BoneMatrixBuffer> boneBuffer;

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

    // Root motion control
    bool rootMotionEnabled = true;
    bool lockRootPosition = false;  // When true, root bone position is locked to bind pose

    void EvaluateNode(
        const AssimpNodeData &node,
        const glm::mat4 &parent,
        Animation *blendAnim,
        float blendFactor);

    // TRS-based evaluation for fast multi-layer blending
    void EvaluateNodeTRS(
        const AssimpNodeData &node,
        const glm::mat4& parentGlobal,
        Animation* anim,
        float time,
        std::vector<BoneTRS>& outTRS);

    // static std::string NormalizeName(const std::string& name);
};
