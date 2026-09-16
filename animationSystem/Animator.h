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

// (free) world-space rotation that orients a +Y-up foot to a ground normal (flat
// ground -> identity; slope -> hug-the-surface tilt). Declared here so the foot-IK
// terrain alignment (todo Part 3, Option A) and its unit tests can call it.
glm::quat ComputeFootTiltQuat(const glm::vec3& groundNormal,
                              const glm::vec3& footUp = glm::vec3(0.0f, 1.0f, 0.0f));

// Kalman filter state for adaptive ground-normal smoothing (Fix 1 from
// updated todo: eliminates sub-pixel terrain raycast flutter at idle).
struct NormalKalmanState {
    glm::vec3 x = glm::vec3(0.0f, 1.0f, 0.0f); // Filtered output normal
    glm::vec3 P = glm::vec3(0.1f);              // Error covariance
    float Q = 0.002f;                           // Process noise covariance
    float R = 0.08f;                            // Measurement noise covariance
};

class Animator
{
public:
    Animator(const Skeleton *skeleton);
    ~Animator();

    void Play(Animation *animation);
    void BlendTo(Animation *animation, float duration);
    void BlendToAt(Animation *animation, float time, float duration);
    void BlendToWithWeight(Animation *animation, float targetWeight, float duration);
    void BlendTwoAnimations(Animation *anim1, float weight1, Animation *anim2, float weight2, float dt);
    void AddAnimationLayer(Animation *animation, float weight = 1.0f, float blendDuration = 0.3f);
    
    // Time control
    void SetCurrentTime(float time);
    float GetCurrentTime() const { return animatorTime; }

    void Update(float dt);
    // Re-evaluate the skeleton hierarchy with the IK rotations/foot offsets
    // just computed by UpdateFootIK, so the visual pose matches the IK solve
    // in the SAME frame (no 1-frame lag).  Called by AnimatedCharacter
    // after matcher->ApplyFootIK().
    void RevalidateIK();

    // ---- PELVIS HEIGHT ADJUSTMENT (knee-bend fix) ----
    // When a planted foot target is below the animated ankle (e.g. walking
    // down a step), the analytical two-bone solver clamps the reach to max
    // leg length and shoves the residual into the ankle offset — this
    // visibly STRETCHES the leg and makes the knee bend look unnatural.
    // The pelvis adjuster measures how far each leg is from fully reaching
    // its target, then drops the Hips bone (propagates down the leg chain)
    // so the knee bend carries the foot down instead. Run AFTER bone eval,
    // BEFORE UpdateFootIK, using the previous frame's IK targets.
    void CalculatePelvisAdjustment(float dt, const glm::mat4& modelMatrix);
    float getPelvisDropY() const { return currentPelvisDropY; }
    float getPelvisDropVelocity() const { return pelvisDropVelocity; }
    float getLeftLegReach()  const { return dynamicLeftLegLength;  }
    float getRightLegReach() const { return dynamicRightLegLength; }
    float getIKWorldScale()  const { return ikWorldScale; }

    // ---- VELOCITY-ROBUST RELEASES ----
    // Feed the character's horizontal world velocity to the animator so the
    // IK / over-stride release logic can scale its release thresholds with
    // body speed (prevents false releases on high-speed turns where the hip
    // swings further past the planted foot naturally).
    void SetCharacterVelocity(const glm::vec3& velocity) {
        characterVelocity = velocity;
    }

    // ---- Structural Motion Graph: Transition clip playback ----
    // Previously, walk→crouch transitions used runtime patches:
    //   - SetInertialStatureOffset() — held pelvis up during database swap
    //   - ApplyStaturePoseBlend() — blended leg bone shapes from walk→crouch
    //   - ClearStaturePoseBlend() — released the blend on completion
    // These are REMOVED. Transitions are now pre-computed as smooth blend
    // clips (TransitionClipGenerator) that structurally handle all-joint
    // slerp + 2D alignment + height differences at load time.

    void UploadToTexture(GLuint texID);
    void Upload(Shader &shader);

    // -------- FOOT LOCKING / IK --------
    void AddIKOffset(int bone, const glm::vec3 &offset, float weight);
    void AddRootRotationOffset(const glm::quat& rot);  // quintic inertialization root rot
    bool IsFootPlanted(int bone) const;
    bool IsCharacterGrounded() const;  // Check if character has at least one foot planted
    glm::vec3 GetBoneWorldPosition(int bone, const glm::mat4 &modelMat) const;

    // -------- FOOT IK SYSTEM --------
    struct FootIKSettings {
        bool enabled = false;
        float floorHeight = 0.0f;
        float ikStrength = 1.0f;
        float footLockBlend = 15.0f;     // Foot-lock weight ramp rate (1/s). Must engage within a
                                          // single stance (~0.4s walk) so lockWeight saturates to 1.0
                                          // and the two-bone knee bend + ankle offset fire at full
                                          // strength — 0.8 (1.25s) never saturated mid-step, leaving
                                          // the knees ~30-50% bent and the legs sagging behind the body.
                                          // 15 locks in ~4 frames (knee bends in smoothly, no pop)
                                          // while staying well inside a stance.
        float ankleFKWeight = 0.5f;       // Blend between FK ankle and IK ankle
        float maxIKDistance = 0.2f;      // Tight ankle-offset budget (world m): the ankle translate must NOT visibly stretch the leg, so keep it small and let the knee (ikMinKneeClampDeg=20) + soft-extension damping do most of the planting; this only trims the residual.
        float ankleOffsetMaxSpeed = 3.0f;  // m/s cap on ankle-translate rate; clamps the one-frame ankleOffset spike on motion transitions (visible ankle stretch) WITHOUT lagging real plants (which move ~0.5-1 m/s). At 0.2m budget a spike settles in ~4 frames; raise to soften/smear more, lower to sharpen.
        float footLockReleaseSpeed = 2.0f;
        // Frames of SUSTAINED ground contact required before a foot is allowed
        // to lock. Kills the low-speed false-lock: a jog/crouch swing foot sits
        // within the nearFloor band and moves slowly (footSpeed < plant
        // threshold) just like a planting foot — without confirmation it locks
        // mid-swing, sticking the foot under a body that's already moving on
        // (legs "trail") and toggling on idle animation noise (jitter). 5
        // frames (~3 frames at 60fps) is far shorter than a real stance and
        // longer than a passing slow swing, so only a foot that is genuinely
        // settled on the ground locks.
        int footLockConfirmFrames = 5;
        int leftFootBone = -1;
        int rightFootBone = -1;
        int leftToeBone = -1;
        int rightToeBone = -1;
        // Two-bone knee bend: thigh (upLeg) + shin (leg) per leg, so the leg
        // bends at the knee toward the planted foot instead of stretching.
        int leftUpLegBone = -1;
        int rightUpLegBone = -1;
        int leftLegBone = -1;
        int rightLegBone = -1;
        float kneeBendWeight = 1.0f;  // 1 = full knee bend; 0 = ankle-translate only
        float ikKneeClampDeg = 175.0f; // don't hyperextend the knee past this (deg).
                                       // On flat ground an over-reach two-bone would
                                       // otherwise drive the knee to 180 (locked);
                                       // this keeps it in (0,pi) and ankleOffset finishes
                                       // the plant at floorY.
        // UE-style min knee angle (deg). The low floor (~20°) lets the leg bend
        // deeply to shorten and reach low / sloped targets via the knee instead
        // of stretching the ankleOffset. The iterative knee clamp in SolveLegIK
        // still honours the [ikMinKneeClampDeg, ikKneeClampDeg] band; on flat
        // ground the soft-extension damping keeps the knee near-straight, so
        // this floor is rarely hit while walking.
        float ikMinKneeClampDeg = 20.0f;
        // Unreal Engine-style leg profile (todo: "How UE Two-Bone IK eliminates
        // leg stretching"). bAllowStretching OFF by default -> non-elastic clamp
        // (bones never stretch past L1+L2; the foot->floor gap is absorbed by
        // the ankleOffset, capped at maxIKDistance). True scales bone lengths by
        // d/maxReach for a cartoony elastic stretch when the target is unreachable.
        bool bAllowStretching = false;
        // UE "Extension Damping" soft-limiting zone (5% of max leg length): as
        // the hip->target distance nears max reach it is eased toward maxReach
        // with an exponential curve, keeping a microscopic knee bend on flat
        // ground and killing the knee->180 snap/pop (worse at 70% scale).
        float softExtensionFactor = 0.05f;
        // Rest-state damping on the solver output rotations (10 Hz low-pass
        // envelope) to kill micro-vibration from ground-normal flutter when
        // the character is standing perfectly still. Bypassed during locomotion.
        bool restDampingEnabled = true;
    };

    void SetFootIKEnabled(bool enabled);
    void SetFootIKSettings(const FootIKSettings& settings);
    void SetFloorHeight(float height);
    // Per-foot terrain heightmap (world x,z -> y). Drives the ground-normal foot
    // tilt so heels/toes hug slopes (todo Part 3, Option A). Nil = flat foot.
    void SetTerrainHeightFn(const std::function<float(float,float)>& fn);
    // Public terrain-normal sampler (moved from free-fn to expose for the
    // MotionMatcher foot-tilt pass). Returns world-space surface normal.
    static glm::vec3 SampleTerrainNormal(const std::function<float(float,float)>& fn,
                                         const glm::vec3& worldPos, float eps = 0.5f);
    // Adaptive 1-D Kalman filter for ground normals (Fix 1). Filters raw
    // terrain raycast normals to kill idle-state sub-pixel flutter while
    // automatically scaling back filtering during fast movement.
    glm::vec3 FilterGroundNormal(const glm::vec3& rawMeasurement,
                                 bool isLeftLeg, float dt);
    void SetIKWorldScale(float scale);
    void SetFootBones(int leftFoot, int rightFoot, int leftToe = -1, int rightToe = -1,
                      int leftUpLeg = -1, int rightUpLeg = -1,
                      int leftLeg = -1, int rightLeg = -1);
    void UpdateFootIK(float dt, const glm::mat4& modelMatrix, bool isMoving = false);  // isMoving disables foot lock
    // Dynamic leg reach metrics used by the MotionMatcher pelvis-damping pass
    // (Fix 2 from updated todo). Returns L1+L2 in model units for each leg.
    float GetLeftLegMaxReach() const;
    float GetRightLegMaxReach() const;
    // Analytical two-bone knee bend: swings the thigh (about the hip) and the
    // shin (about the knee) so the ankle reaches targetWorld without stretching
    // the leg. Out params are LOCAL-frame matrices to apply via the ikRotation
    // path (rotation then translate) and a residual ankle translate for exact
    // floor contact. Returns false if the solve is degenerate (bones missing).
    bool SolveLegIK(int thighBone, int shinBone, int ankleBone,
                    const glm::mat4& modelMatrix, const glm::vec3& targetWorld,
                    glm::mat4& outThighRot, glm::mat4& outShinRot,
                    glm::vec3& outAnkleOffsetModel,
                    glm::vec3& outAnkleWorldEnd,     // ankle world pos after the knee rotation (pre-translate)
                    glm::vec3& outKneeWorldEnd)     // knee world pos after the knee rotation
                    const;

    // Debug wrapper that logs when the knee angle drops below 170°
    bool DebugSolveLegIK(int thighBone, int shinBone, int ankleBone,
                         const glm::mat4& modelMatrix, const glm::vec3& targetWorld,
                         glm::mat4& outThighRot, glm::mat4& outShinRot,
                         glm::vec3& outAnkleOffsetModel,
                         glm::vec3& outAnkleWorldEnd, glm::vec3& outKneeWorldEnd) const;

    void DebugDrawFootIK();  // Call after rendering to debug

    glm::vec3 ConsumeRootMotion();

    // Root motion control - lock root bone position to prevent sliding
    void SetRootMotionEnabled(bool enabled) { rootMotionEnabled = enabled; }
    bool IsRootMotionEnabled() const { return rootMotionEnabled; }
    void SetLockRootPosition(bool lock) { lockRootPosition = lock; }
    bool IsRootPositionLocked() const { return lockRootPosition; }

    const std::vector<glm::mat4> &GetFinalBoneMatrices() const;

    // Mutable accessor for layer blending / batched uploads that need to write
    // back blended bone matrices (kept in sync with globalBoneMatrices).
    std::vector<glm::mat4> &GetFinalBoneMatricesMutable() { return finalBoneMatrices; }

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
    struct AnimationLayer;
    const AnimationLayer* GetActiveLayer(int index) const;
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
    std::vector<glm::mat4> ikRotations;  // per-bone LOCAL-frame knee-bend rotations (two-bone IK)
    // Per-bone ground-normal tilt quat (foot bones only; identity elsewhere).
    // Set in UpdateFootIK per planted foot; applied to the ankle ikXform so feet
    // hug slopes (todo Part 3, Option A).
    // Root rotation offset quaternion (for quintic inertialization). Applied
    // after translation in EvaluateNode so the root bone rotates smoothly
    // during state transitions — eliminates the "snap" when the old pose and
    // new pose face different headings.
    glm::quat rootQuatOffset{1.0f, 0.0f, 0.0f, 0.0f};
    std::vector<glm::quat> ikFootTilt;

    // -------- FOOT IK STATE --------
    FootIKSettings footIKSettings;
    struct FootIKState {
        glm::vec3 lockedPosition{0.0f};
        glm::vec3 targetPosition{0.0f};
        glm::vec3 ankleOffset{0.0f};
        glm::vec3 ankleOffsetSmoothed{0.0f}; // rate-limited ankle translate actually emitted to the bones (gated by FootIKSettings::ankleOffsetMaxSpeed)
        float lockWeight = 0.0f;  // 0 = fully unlocked, 1 = fully locked
        bool isLocked = false;
        float timeSinceLock = 0.0f;
        // Contact-confirmation counter for lock hysteresis (see footLockConfirmFrames).
        int contactConfirm = 0;
        // Over-stride release arming flag: set on release, re-armed only when the
        // hip returns within the leg's reach. This makes the release fire ONCE
        // per genuine over-reach instead of re-firing every frame while the body
        // is still past the foot (which churned the lock/unlock on sharp turns ->
        // the jitter / "quick turn messes up feet" symptom).
        bool overStrideArmed = true;
    };
    FootIKState leftFootIK;
    FootIKState rightFootIK;

    // Kalman filter states for adaptive ground-normal smoothing (Fix 1)
    NormalKalmanState leftAnkleKalman;
    NormalKalmanState rightAnkleKalman;

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
public:
    // Public so retargeting/scaling can invalidate Animator IK state (see
    // SkeletonRetargeter::initialize). Resets animation caches AND the cached
    // L1+L2 leg-reach lengths so the IK solver re-derives limb reach.
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
    bool lockRootPosition = false;
    float ikWorldScale = 1.0f;  // When true, root bone position is locked to bind pose
    // Terrain heightfield (world x,z -> y); sampled per foot for the slope normal.
    // Empty on the engine/headless path -> feet stay flat (no tilt).
    std::function<float(float,float)> m_terrainFn;

    // ---- PELVIS HEIGHT ADJUSTMENT (knee-bend fix) ----
    // Smoothed world-space pelvis drop (negative = down). Applied as a local-Y
    // translation on the "Hips" bone in EvaluateNode / EvaluateNodeTRS / the
    // TRS blend compose lambda, so it propagates down both leg chains.
    float currentPelvisDropY = 0.0f;
    // Velocity of the pelvis drop itself — tracked so the non-linear spring
    // envelope in CalculatePelvisAdjustment has a proper critically-damped
    // state with velocity, not just a positional lerp.
    float pelvisDropVelocity = 0.0f;
    // Character's horizontal world velocity, fed in by MotionMatcher before
    // the IK / pelvis passes so the over-stride release can be made
    // velocity-robust.
    glm::vec3 characterVelocity{0.0f};
    // Pelvis drop sampled during the frame's FIRST skeleton pass (the value of
    // currentPelvisDropY when evaluateBoneMatrices ran inside Update). Paired
    // with currentPelvisDropY (the final revalidation pass) by SolveLegIK to
    // compute the mid-frame pelvis delta and stop 1-frame leg stretch/jitter.
    float prevPelvisDropY = 0.0f;
    // Cached per-leg max reach (thigh length + shin length). Lazily computed
    // from the animated bone positions so it adapts to scaling / rig changes.
    float dynamicLeftLegLength = -1.0f;
    float dynamicRightLegLength = -1.0f;
    // Tracked so we can invalidate the animation cache when the pelvis drop
    // changes (cached matrices would otherwise bake in a stale drop).
    float cachedPelvisDropY = -999.0f;
    // Last frame delta-time (sampled in Update). Exposed to SolveLegIK for
    // rest-state damping envelopes that are frame-rate independent.
    float lastDeltaTime = (1.0f / 120.0f);
    // Compute L1+L2 for each leg from the current animated bone positions.
    void CacheLegLengths();

    // Old leg ikRotations from the previous frame, saved in UpdateFootIK before
    // the reset so SolveLegIK can compute true no-IK bone matrices.
    glm::mat4 savedOldLeftThighIk  = glm::mat4(1.0f);
    glm::mat4 savedOldRightThighIk = glm::mat4(1.0f);
    glm::mat4 savedOldLeftShinIk   = glm::mat4(1.0f);
    glm::mat4 savedOldRightShinIk  = glm::mat4(1.0f);

    void EvaluateNode(
        const AssimpNodeData &node,
        const glm::mat4 &parent,
        Animation *blendAnim,
        float blendFactor);

    // Evaluate the bone hierarchy from the current animation state, applying
    // ikRotations / ikOffsets / currentPelvisDropY.  Shared by Update() and
    // RevalidateIK() to eliminate the 1-frame IK lag.
    void evaluateBoneMatrices();

    // TRS-based evaluation for fast multi-layer blending
    void EvaluateNodeTRS(
        const AssimpNodeData &node,
        const glm::mat4& parentGlobal,
        Animation* anim,
        float time,
        std::vector<BoneTRS>& outTRS);

    // static std::string NormalizeName(const std::string& name);
};
