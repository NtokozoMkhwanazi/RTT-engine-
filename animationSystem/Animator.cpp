#include "Animator.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/string_cast.hpp>
#include "boneSystem/BoneName.h"
#include "AnimationRetargeting.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>

// No local normalization - use canonical NormalizeBoneName from BoneName.h

// ------------------------------------------------------------
// Animator
// ------------------------------------------------------------
Animator::Animator(const Skeleton *skel)
    : skeleton(skel),
      current(nullptr),
      next(nullptr),
      animatorTime(0.0f),
      blendTime(0.0f),
      blendDuration(0.0f),
      prevRootPos(0.0f),
      rootMotionDelta(0.0f)
{
    size_t boneCount = (skeleton && !skeleton->bones.empty()) ? skeleton->bones.size() : MAX_BONES;
    finalBoneMatrices.resize(boneCount, glm::mat4(1.0f));
    globalBoneMatrices.resize(boneCount, glm::mat4(1.0f));
    prevBoneWorldPos.resize(boneCount, glm::vec3(0.0f));
    currBoneWorldPos.resize(boneCount, glm::vec3(0.0f));
    ikOffsets.resize(boneCount, glm::vec3(0.0f));

    // Initialize caching system
    cachedAnimationTimes.reserve(10);
    cachedBoneTransforms.reserve(10);
}

void Animator::Play(Animation *anim)
{
    // For backward compatibility, set the old-style animation
    current = anim;
    next = nullptr;
    animatorTime = 0.0f;

    // Also clear any active animations and start fresh with the new one for the new system
    activeAnimations.clear();
    if (anim) {
        activeAnimations.emplace_back(anim, 1.0f, 0.0f);  // Single animation with full weight
    }

    // IMPORTANT: reset root motion state when a new animation starts
    prevRootPos = glm::vec3(0.0f);
    rootMotionDelta = glm::vec3(0.0f);
    hasPrevRoot = false;
}

void Animator::SetCurrentTime(float time)
{
    if (!current) return;

    // Clamp to animation duration
    animatorTime = fmod(time, current->GetDuration());
    if (animatorTime < 0.0f) {
        animatorTime += current->GetDuration();
    }
    
    // ALSO update activeAnimations time (for new system)
    if (!activeAnimations.empty()) {
        activeAnimations[0].time = animatorTime;
    }
}

void Animator::BlendTo(Animation *anim, float duration)
{
    if (!anim) {
        std::cout << "[BlendTo] NULL animation!\n";
        return;
    }

    std::cout << "[BlendTo] ptr=" << anim << " dur=" << anim->duration 
              << " bones=" << anim->boneAnimations.size()
              << " activeLayers=" << activeAnimations.size() << "\n";

    // Check if this animation is already the main active one at full weight
    if (!activeAnimations.empty() && activeAnimations[0].animation == anim && activeAnimations[0].targetWeight >= 0.99f) {
        std::cout << "[BlendTo] Already playing THIS animation (ptr match)\n";
        return;
    }

    // Check if this animation is already in the active list (by pointer)
    for (size_t i = 0; i < activeAnimations.size(); i++) {
        if (activeAnimations[i].animation == anim) {
            std::cout << "[BlendTo] Found in layer " << i << ", boosting weight\n";
            // Already active - boost it to full weight
            activeAnimations[i].targetWeight = 1.0f;
            activeAnimations[i].blendDuration = duration;
            activeAnimations[i].blendProgress = 0.0f;
            // Reduce other animations
            for (auto& other : activeAnimations) {
                if (other.animation != anim) {
                    other.targetWeight = 0.0f;
                    other.blendDuration = duration;
                }
            }
            return;
        }
    }

    // Animation not in list - add it with target weight 1.0
    std::cout << "[BlendTo] Adding NEW animation layer\n";
    next = anim;
    blendTime = 0.0f;
    blendDuration = glm::max(duration, 0.001f);

    if (activeAnimations.empty()) {
        activeAnimations.emplace_back(anim, 1.0f, 0.0f);
    } else {
        activeAnimations.emplace_back(anim, 0.0f, duration);
        activeAnimations.back().targetWeight = 1.0f;

        // Reduce weight of existing animations
        for (auto& layer : activeAnimations) {
            if (layer.animation != anim) {
                layer.targetWeight = 0.0f;
                layer.blendDuration = duration;
            }
        }
    }
}

void Animator::Update(float dt)
{
    if (!skeleton)
    {
        std::cerr << "[Animator::Update] ERROR: No skeleton!\n";
        return;
    }

    static bool DEBUG_PAUSE_ANIM = false;
    debugForceIdentityScale = false;

    // Save previous positions BEFORE overwriting
    prevBoneWorldPos = currBoneWorldPos;

    // Update animation times and blend weights
    if (!DEBUG_PAUSE_ANIM)
    {
        // Reset triggered status for repeatable events at the start of each frame
        for (auto& event : animationEvents) {
            if (event.repeatable) {
                const_cast<AnimationEventTrigger&>(event).triggered = false;
            }
        }

        UpdateAnimationBlending(dt);
    }

    // Initialize matrices
    finalBoneMatrices.assign(skeleton->bones.size(), glm::mat4(1.0f));
    globalBoneMatrices.assign(skeleton->bones.size(), glm::mat4(1.0f));

    // Process active animations with proper blending
    if (!activeAnimations.empty())
    {
        // Check if we can use cached transforms
        bool canUseCache = useCaching && cacheValid;
        if (canUseCache && cachedAnimationTimes.size() == activeAnimations.size())
        {
            for (size_t i = 0; i < activeAnimations.size(); ++i)
            {
                if (std::abs(cachedAnimationTimes[i] - activeAnimations[i].time) > 0.001f)
                {
                    canUseCache = false;
                    break;
                }
            }
        }
        else
        {
            canUseCache = false;
        }

        if (canUseCache)
        {
            // Use cached transforms
            for (size_t boneIdx = 0; boneIdx < skeleton->bones.size(); ++boneIdx)
            {
                if (boneIdx < finalBoneMatrices.size() && boneIdx < cachedBoneTransforms[0].size())
                {
                    finalBoneMatrices[boneIdx] = cachedBoneTransforms[0][boneIdx];
                }
            }
        }
        else
        {
            // Calculate total weight for normalization
            float totalWeight = 0.0f;
            int dominantLayerIdx = -1;
            float maxWeight = -1.0f;
            
            for (const auto& layer : activeAnimations)
            {
                if (layer.animation && layer.weight > 0.0f && layer.enabled)
                {
                    totalWeight += std::abs(layer.weight);
                }
            }

            // If total weight is too small, just compute bind pose
            if (totalWeight < 0.0001f)
            {
                EvaluateNode(skeleton->rootNode, glm::mat4(1.0f), nullptr, 0.0f);
            }
            else
            {
                // For proper blending, we need to evaluate each animation separately
                // and then blend the resulting transforms. This requires a more complex approach
                // than what's currently implemented in EvaluateNode.

                // For now, we'll use a weighted average approach for the first animation
                // with a fallback to the legacy system if there are multiple animations
                if (activeAnimations.size() == 1 && activeAnimations[0].animation && activeAnimations[0].enabled)
                {
                    // Single animation case - straightforward
                    EvaluateNode(skeleton->rootNode, glm::mat4(1.0f), activeAnimations[0].animation, activeAnimations[0].time);
                    dominantLayerIdx = 0;
                }
                else
                {
                    // Multiple animations - we need to blend them
                    // This is a simplified approach that prioritizes the animation with highest weight
                    const AnimationLayer* dominantLayer = nullptr;

                    for (size_t i = 0; i < activeAnimations.size(); i++)
                    {
                        const auto& layer = activeAnimations[i];
                        if (layer.animation && layer.weight > maxWeight && layer.enabled)
                        {
                            maxWeight = layer.weight;
                            dominantLayer = &layer;
                            dominantLayerIdx = i;
                        }
                    }

                    if (dominantLayer && dominantLayer->animation)
                    {
                        // Use the animation with the highest weight as the base
                        EvaluateNode(skeleton->rootNode, glm::mat4(1.0f), dominantLayer->animation, dominantLayer->time);
                    }
                    else
                    {
                        // Fallback to bind pose
                        EvaluateNode(skeleton->rootNode, glm::mat4(1.0f), nullptr, 0.0f);
                    }
                }
            }

            // Debug: print dominant animation layer every 30 frames
            static int debugFrame = 0;
            debugFrame++;
            if (debugFrame % 30 == 0 && dominantLayerIdx >= 0) {
                auto& layer = activeAnimations[dominantLayerIdx];
                std::cout << "[Animator] Layer#" << dominantLayerIdx
                          << " ptr=" << layer.animation
                          << " name=" << layer.animation->name
                          << " dur=" << layer.animation->duration
                          << " time=" << layer.time
                          << " weight=" << layer.weight << "\n";
                
                // Print a sample bone transform to verify animation is changing
                if (!finalBoneMatrices.empty()) {
                    glm::mat4& boneMat = finalBoneMatrices[55];  // rightupleg
                    std::cout << "  [Bone55] pos=(" << boneMat[3].x << "," << boneMat[3].y << "," << boneMat[3].z << ")\n";
                }
            }

            // Update cache if caching is enabled
            if (useCaching)
            {
                cachedAnimationTimes.resize(activeAnimations.size());
                for (size_t i = 0; i < activeAnimations.size(); ++i)
                {
                    cachedAnimationTimes[i] = activeAnimations[i].time;
                }

                cachedBoneTransforms.resize(1);
                cachedBoneTransforms[0] = finalBoneMatrices;
                cacheValid = true;
            }
        }
        
        // Sync animatorTime with activeAnimations[0].time for backward compatibility
        if (!activeAnimations.empty()) {
            animatorTime = activeAnimations[0].time;
        }
    }
    else
    {
        // Handle legacy system if no active animations
        Animation *savedCurrent = current;
        if (!DEBUG_PAUSE_ANIM && current)
        {
            // Apply animation speed multiplier
            float ticksPerSecond = current->GetTicksPerSecond();
            float speed = current->speed;  // Speed multiplier (1.0 = normal)
            animatorTime += dt * ticksPerSecond * speed;
            animatorTime = fmod(animatorTime, current->GetDuration());
        }
        else
        {
            current = nullptr;
        }

        EvaluateNode(skeleton->rootNode, glm::mat4(1.0f), nullptr, animatorTime);
        current = savedCurrent;
    }

    // Compute current bone positions
    for (size_t i = 0; i < globalBoneMatrices.size(); ++i)
    {
        currBoneWorldPos[i] =
            glm::vec3(globalBoneMatrices[i] * glm::vec4(0, 0, 0, 1));
    }

    // Root motion
    rootMotionDelta = glm::vec3(0.0f);

    int rootIdx = skeleton->rootBoneIndex;
    if (rootIdx >= 0 && rootIdx < (int)currBoneWorldPos.size())
    {
        glm::vec3 currRoot = currBoneWorldPos[rootIdx];

        if (!hasPrevRoot)
        {
            prevRootPos = currRoot;
            hasPrevRoot = true;
        }
        else
        {
            rootMotionDelta = currRoot - prevRootPos;
            prevRootPos = currRoot;
        }

        rootMotionDelta.y = 0.0f;
    }
    
    // Cleanup: Remove animations with 0 weight to prevent buildup
    if (activeAnimations.size() > 4) {
        activeAnimations.erase(
            std::remove_if(activeAnimations.begin(), activeAnimations.end(),
                [](const AnimationLayer& layer) { 
                    return layer.weight < 0.01f && layer.targetWeight < 0.01f; 
                }),
            activeAnimations.end());
    }
}

void Animator::EvaluateNode(
    const AssimpNodeData &node,
    const glm::mat4 &parent,
    Animation *blendAnim,
    float blendFactor)
{
    std::string name = NormalizeBoneName(node.name);

    glm::mat4 bindLocal = node.transform;

    // ---- Decompose bind pose ----
    glm::vec3 bindScale, bindPos, skew;
    glm::quat bindRot;
    glm::vec4 perspective;

    glm::decompose(bindLocal, bindScale, bindRot, bindPos, skew, perspective);
    bindRot = glm::normalize(bindRot);

    // ---- Start from bind pose ----
    // Note: We'll override these with animation data if available
    glm::vec3 pos = bindPos;
    glm::quat rot = bindRot;
    glm::vec3 scale = bindScale;

    // ---- Apply animation (override bind channels) ----
    Animation *animationToUse = blendAnim ? blendAnim : current;
    float timeToUse = blendAnim ? blendFactor : animatorTime;

    if (animationToUse)
    {
        if (const BoneAnimation *boneAnim = animationToUse->GetBoneAnimation(name))
        {
            // Animation completely replaces bind pose
            if (boneAnim->HasPositionAnimation())
            {
                pos = boneAnim->InterpolatePosition(timeToUse);
                
                // Lock root position if enabled (prevents sliding)
                if (lockRootPosition && node.boneIndex == skeleton->rootBoneIndex) {
                    pos = bindPos;  // Use bind pose position, ignore animation
                }
            }

            if (boneAnim->HasRotationAnimation())
            {
                rot = boneAnim->InterpolateRotation(timeToUse);
            }

            if (boneAnim->HasScaleAnimation())
            {
                scale = boneAnim->InterpolateScale(timeToUse);
            }

            if (debugForceIdentityScale)
                scale = glm::vec3(1.0f);
        }
    }

    // ---- Rebuild local transform ----
    glm::mat4 localTransform =
        glm::translate(glm::mat4(1.0f), pos) *
        glm::mat4_cast(rot) *
        glm::scale(glm::mat4(1.0f), scale);

    // ---- Global ----
    glm::mat4 globalTransform = parent * localTransform;

    int boneIndex = node.boneIndex;

    if (boneIndex != -1)
    {
        glm::mat4 ikOffsetMat =
            glm::translate(glm::mat4(1.0f), ikOffsets[boneIndex]);

        glm::mat4 finalGlobal = globalTransform * ikOffsetMat;

        globalBoneMatrices[boneIndex] = finalGlobal;

        // SKINNING FORMULA:
        // finalBoneMatrix = boneGlobalTransform * boneOffset
        // This transforms vertices from bind pose to current pose
        finalBoneMatrices[boneIndex] =
            finalGlobal *
            skeleton->bones[boneIndex].offset;

        // Pass the finalGlobal (with IK offset) as parent to children to maintain proper hierarchy
        for (const auto &child : node.children)
        {
            EvaluateNode(child, finalGlobal, blendAnim, blendFactor);
        }
    }
    else
    {
        // Still pass the global transform to children for proper hierarchy
        for (const auto &child : node.children)
        {
            EvaluateNode(child, globalTransform, blendAnim, blendFactor);
        }
    }
}

glm::vec3 Animator::GetBoneWorldPosition(int bone, const glm::mat4 &modelMat) const
{
    if (bone < 0 || bone >= (int)globalBoneMatrices.size())
        return glm::vec3(0.0f);

    glm::mat4 world = modelMat * globalBoneMatrices[bone];
    return glm::vec3(world * glm::vec4(0, 0, 0, 1));
}

bool Animator::IsFootPlanted(int bone) const
{
    if (bone < 0 || bone >= (int)currBoneWorldPos.size() || bone >= (int)prevBoneWorldPos.size())
        return false;

    // Check if the bone's Y position hasn't changed significantly (indicating it's planted on the ground)
    float verticalVelocity = std::abs(currBoneWorldPos[bone].y - prevBoneWorldPos[bone].y);
    
    // Also check if foot is near ground level
    float footHeight = currBoneWorldPos[bone].y;
    bool nearGround = footHeight < (footIKSettings.floorHeight + 0.2f);
    
    return (verticalVelocity < 0.01f) && nearGround;
}

// Helper function to get foot planting status for debugging
bool Animator::IsCharacterGrounded() const
{
    if (!footIKSettings.enabled) return false;
    
    int leftFoot = footIKSettings.leftFootBone;
    int rightFoot = footIKSettings.rightFootBone;
    
    bool leftPlanted = (leftFoot >= 0) ? IsFootPlanted(leftFoot) : false;
    bool rightPlanted = (rightFoot >= 0) ? IsFootPlanted(rightFoot) : false;
    
    // Character is grounded if at least one foot is planted
    return leftPlanted || rightPlanted;
}

void Animator::AddIKOffset(int bone, const glm::vec3 &offset, float weight)
{
    if (bone < 0 || bone >= (int)ikOffsets.size())
        return;
    ikOffsets[bone] += offset * weight;
}

glm::vec3 Animator::ConsumeRootMotion()
{
    glm::vec3 d = rootMotionDelta;
    rootMotionDelta = glm::vec3(0.0f);
    return d;
}

const std::vector<glm::mat4> &Animator::GetFinalBoneMatrices() const
{
    return finalBoneMatrices;
}

// ============================================================================
// BONE MATRIX BUFFER IMPLEMENTATION (UBO/SSBO)
// ============================================================================

bool Animator::InitializeBoneBuffer(size_t maxBones, const BoneBufferConfig& config)
{
    if (!boneBuffer) {
        boneBuffer = std::make_unique<BoneMatrixBuffer>();
    }
    
    bool success = boneBuffer->Initialize(maxBones, config);
    
    if (success) {
        std::cout << "[Animator] Bone buffer initialized: " << maxBones << " bones (" 
                  << boneBuffer->GetBufferTypeString() << ")\n";
    }
    
    return success;
}

bool Animator::UpdateBoneBuffer()
{
    if (!boneBuffer || !boneBuffer->IsInitialized()) {
        return false;
    }
    
    return boneBuffer->Update(finalBoneMatrices);
}

void Animator::BindBoneBuffer(GLuint bindingPoint) const
{
    if (boneBuffer && boneBuffer->IsInitialized()) {
        boneBuffer->Bind(bindingPoint);
    }
}

void Animator::PrintBoneBufferStats() const
{
    if (boneBuffer) {
        boneBuffer->PrintStats();
    }
}

Animation* Animator::GetCurrentAnimation() const
{
    // Return the primary active animation (highest weight)
    if (activeAnimations.empty()) return nullptr;
    
    // Find animation with highest weight
    const AnimationLayer* bestLayer = nullptr;
    float maxWeight = -1.0f;
    
    for (const auto& layer : activeAnimations) {
        if (layer.animation && layer.weight > maxWeight && layer.enabled) {
            maxWeight = layer.weight;
            bestLayer = &layer;
        }
    }
    
    return bestLayer ? bestLayer->animation : current;
}

float Animator::GetActiveAnimationTime(int layerIndex) const
{
    if (layerIndex < 0 || layerIndex >= static_cast<int>(activeAnimations.size())) {
        return 0.0f;
    }
    return activeAnimations[layerIndex].time;
}

void Animator::BlendToWithWeight(Animation *anim, float targetWeight, float duration)
{
    if (!anim)
        return;

    // Find if the animation is already in the active animations
    auto it = std::find_if(activeAnimations.begin(), activeAnimations.end(),
                           [anim](const AnimationLayer& layer) { return layer.animation == anim; });

    if (it != activeAnimations.end()) {
        // If the animation exists, update its target weight and blend duration
        it->targetWeight = targetWeight;
        it->blendDuration = duration;
        it->blendProgress = 0.0f;
    } else {
        // If the animation doesn't exist, add it as a new layer
        activeAnimations.emplace_back(anim, 0.0f, duration);
        activeAnimations.back().targetWeight = targetWeight;
    }
}

// GRADIENT BAND INTERPOLATION: Blend two animations with explicit weights
void Animator::BlendTwoAnimations(Animation *anim1, float weight1, Animation *anim2, float weight2, float dt)
{
    if (!anim1 && !anim2) return;
    
    // Normalize weights
    float totalWeight = weight1 + weight2;
    if (totalWeight < 0.001f) return;
    
    weight1 /= totalWeight;
    weight2 /= totalWeight;
    
    // Clear existing layers and set up two-animation blend
    activeAnimations.clear();
    
    if (anim1) {
        activeAnimations.emplace_back(anim1, weight1, 0.1f);
        activeAnimations.back().targetWeight = weight1;
        activeAnimations.back().blendProgress = 1.0f;  // Already blended
    }
    if (anim2) {
        activeAnimations.emplace_back(anim2, weight2, 0.1f);
        activeAnimations.back().targetWeight = weight2;
        activeAnimations.back().blendProgress = 1.0f;  // Already blended
    }
}

void Animator::AddAnimationLayer(Animation *animation, float weight, float blendDuration)
{
    if (!animation)
        return;

    // Add the animation as a new layer with the specified weight
    activeAnimations.emplace_back(animation, weight, blendDuration);
}

void Animator::UploadToTexture(GLuint texID)
{
    if (finalBoneMatrices.empty())
        return;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texID);

    // Upload the final bone matrices to the texture
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 4, finalBoneMatrices.size(), 0,
                 GL_RGBA, GL_FLOAT, finalBoneMatrices.data());

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

void Animator::Upload(Shader &shader)
{
    if (finalBoneMatrices.empty())
        return;

    // Upload the final bone matrices to the shader
    shader.use();
    
    // Assuming the shader has a uniform array for bone matrices
    for (unsigned int i = 0; i < finalBoneMatrices.size(); i++)
    {
        std::string uniformName = "finalBonesMatrices[" + std::to_string(i) + "]";
        shader.setMat4(uniformName.c_str(), finalBoneMatrices[i]);
    }
}

void Animator::SetAnimationWeight(int layerIndex, float weight)
{
    if (layerIndex < 0 || layerIndex >= (int)activeAnimations.size())
        return;

    activeAnimations[layerIndex].targetWeight = weight;
    activeAnimations[layerIndex].weight = weight; // Set current weight to target immediately
}

void Animator::RemoveAnimationLayer(int layerIndex)
{
    if (layerIndex < 0 || layerIndex >= (int)activeAnimations.size())
        return;

    activeAnimations.erase(activeAnimations.begin() + layerIndex);
}

void Animator::RegisterAnimationEvent(Animation* animation, float time, const std::string& eventName)
{
    AnimationEventTrigger event;
    event.animation = animation;
    event.time = time;
    event.eventName = eventName;
    event.callback = nullptr;
    event.triggered = false;
    event.repeatable = true;
    
    animationEvents.insert(event);
}

void Animator::RegisterAnimationEventWithCallback(Animation* animation, float time, const std::string& eventName, std::function<void()> callback)
{
    AnimationEventTrigger event;
    event.animation = animation;
    event.time = time;
    event.eventName = eventName;
    event.callback = callback;
    event.triggered = false;
    event.repeatable = true;
    
    animationEvents.insert(event);
}

void Animator::UnregisterAnimationEvent(Animation* animation, float time, const std::string& eventName)
{
    AnimationEventTrigger event;
    event.animation = animation;
    event.time = time;
    event.eventName = eventName;
    
    animationEvents.erase(event);
}

void Animator::ClearAnimationEvents()
{
    animationEvents.clear();
}

void Animator::SetEventRepeatable(Animation* animation, const std::string& eventName, bool repeatable)
{
    for (auto& event : animationEvents) {
        if (event.animation == animation && event.eventName == eventName) {
            const_cast<AnimationEventTrigger&>(event).repeatable = repeatable;
            if (!repeatable) {
                const_cast<AnimationEventTrigger&>(event).triggered = false;
            }
        }
    }
}

void Animator::SetAnimationEventCallback(const AnimationEventCallback& callback)
{
    eventCallback = callback;
}

void Animator::UpdateAnimationBlending(float dt)
{
    // First pass: update times and detect loops
    std::vector<int> layersNeedingCrossFade;

    for (size_t i = 0; i < activeAnimations.size(); i++)
    {
        auto& layer = activeAnimations[i];
        if (!layer.animation || !layer.enabled) continue;

        float animDuration = layer.animation->GetDuration();
        float speed = layer.animation->speed;  // Speed multiplier

        if (animDuration <= 0.01f) continue;  // Skip very short animations

        float prevTime = layer.time;
        // layer.time is in seconds, dt is in seconds
        layer.time += dt * speed;

        // If we crossed the loop point this frame, mark for cross-fade
        if (prevTime < animDuration && layer.time >= animDuration) {
            layersNeedingCrossFade.push_back(i);
            layer.time = fmod(layer.time, animDuration);
        }
    }

    // Create cross-fade layers for animations that looped
    for (int layerIdx : layersNeedingCrossFade) {
        if (activeAnimations.size() >= 6) break;  // Limit layers

        auto& sourceLayer = activeAnimations[layerIdx];
        float crossFadeDuration = 0.15f;  // 150ms cross-fade

        // Create new layer starting from beginning
        AnimationLayer crossFadeLayer(sourceLayer.animation, 1.0f, crossFadeDuration);
        crossFadeLayer.time = 0.0f;
        crossFadeLayer.targetWeight = 1.0f;
        crossFadeLayer.blendProgress = 0.0f;

        // Mark source layer to fade out
        sourceLayer.targetWeight = 0.0f;
        sourceLayer.blendDuration = crossFadeDuration;
        sourceLayer.blendProgress = 0.0f;

        activeAnimations.push_back(crossFadeLayer);
    }

    // Second pass: update blend weights
    for (auto& layer : activeAnimations)
    {
        if (!layer.animation || !layer.enabled) continue;

        // Update blend progress and interpolate weight
        if (layer.blendProgress < 1.0f && layer.blendDuration > 0.0f)
        {
            layer.blendProgress = glm::min(1.0f, layer.blendProgress + (dt / layer.blendDuration));
            float blendSpeed = 1.0f / layer.blendDuration;
            layer.weight = glm::mix(layer.weight, layer.targetWeight, blendSpeed * dt);

            if (layer.blendProgress >= 1.0f) {
                layer.weight = layer.targetWeight;
            }
        }

        // Trigger animation events
        for (auto& event : animationEvents)
        {
            if (event.animation == layer.animation)
            {
                float lastFrameTime = layer.time - (dt * layer.animation->GetTicksPerSecond());
                bool shouldTrigger = false;

                if ((lastFrameTime <= event.time && layer.time >= event.time) ||
                    (lastFrameTime > layer.time && (lastFrameTime >= event.time || layer.time <= event.time)))
                {
                    shouldTrigger = true;
                }

                if (shouldTrigger && (event.repeatable || !event.triggered))
                {
                    if (!event.repeatable) {
                        const_cast<AnimationEventTrigger&>(event).triggered = true;
                    }

                    if (event.callback) {
                        event.callback();
                    }
                    else if (eventCallback) {
                        eventCallback(event.eventName);
                    }
                }
            }
        }
    }

    // Cleanup: Remove layers that have faded out (aggressive cleanup)
    if (activeAnimations.size() > 1) {
        size_t before = activeAnimations.size();
        activeAnimations.erase(
            std::remove_if(activeAnimations.begin(), activeAnimations.end(),
                [](const AnimationLayer& layer) {
                    return layer.weight < 0.01f && layer.targetWeight < 0.01f;
                }),
            activeAnimations.end());
        if (activeAnimations.size() != before) {
            std::cout << "[Cleanup] Removed " << (before - activeAnimations.size()) << " faded layers\n";
        }
    }
    
    // Reorder: Put layer with highest target weight first (this is the "current" animation)
    if (activeAnimations.size() > 1) {
        size_t bestIdx = 0;
        float bestTarget = activeAnimations[0].targetWeight;
        for (size_t i = 1; i < activeAnimations.size(); i++) {
            if (activeAnimations[i].targetWeight > bestTarget) {
                bestTarget = activeAnimations[i].targetWeight;
                bestIdx = i;
            }
        }
        if (bestIdx != 0) {
            std::iter_swap(activeAnimations.begin(), activeAnimations.begin() + bestIdx);
            std::cout << "[Reorder] Swapped layers, now first has target=" << bestTarget << "\n";
        }
    }

    // Update queued animations
    for (auto& queued : queuedAnimations)
    {
        queued.blendProgress = glm::min(1.0f, queued.blendProgress + (dt / queued.blendDuration));
        queued.weight = glm::mix(0.0f, queued.targetWeight, queued.blendProgress);

        if (queued.blendProgress >= 1.0f)
        {
            activeAnimations.push_back(queued);
        }
    }

    queuedAnimations.erase(
        std::remove_if(queuedAnimations.begin(), queuedAnimations.end(),
                      [](const AnimationLayer& layer) { return layer.blendProgress >= 1.0f; }),
        queuedAnimations.end());
}

void Animator::SetLayerPriority(int layerIndex, int priority)
{
    if (layerIndex < 0 || layerIndex >= (int)activeAnimations.size())
        return;
        
    activeAnimations[layerIndex].priority = priority;
}

void Animator::SetLayerAdditive(int layerIndex, bool additive)
{
    if (layerIndex < 0 || layerIndex >= (int)activeAnimations.size())
        return;
        
    activeAnimations[layerIndex].additive = additive;
}

void Animator::SetLayerBoneMask(int layerIndex, const std::vector<bool>& mask)
{
    if (layerIndex < 0 || layerIndex >= (int)activeAnimations.size())
        return;
        
    activeAnimations[layerIndex].boneMask = mask;
}

void Animator::FadeInLayer(int layerIndex, float duration)
{
    if (layerIndex < 0 || layerIndex >= (int)activeAnimations.size())
        return;
        
    activeAnimations[layerIndex].fadeInDuration = duration;
    activeAnimations[layerIndex].enabled = true;
}

void Animator::FadeOutLayer(int layerIndex, float duration)
{
    if (layerIndex < 0 || layerIndex >= (int)activeAnimations.size())
        return;
        
    activeAnimations[layerIndex].fadeOutDuration = duration;
}

void Animator::EnableLayer(int layerIndex, bool enable)
{
    if (layerIndex < 0 || layerIndex >= (int)activeAnimations.size())
        return;
        
    activeAnimations[layerIndex].enabled = enable;
}

int Animator::GetLayerByAnimation(Animation* animation) const
{
    for (size_t i = 0; i < activeAnimations.size(); ++i)
    {
        if (activeAnimations[i].animation == animation)
        {
            return static_cast<int>(i);
        }
    }
    return -1; // Not found
}

void Animator::ClearCache()
{
    cachedAnimationTimes.clear();
    cachedBoneTransforms.clear();
    cacheValid = false;
}

void Animator::SetLayerBlendType(int layerIndex, BlendType blendType)
{
    if (layerIndex < 0 || layerIndex >= (int)activeAnimations.size())
        return;
        
    activeAnimations[layerIndex].blendType = blendType;
}

void Animator::SetDirectionalBlendParams(int layerIndex, const glm::vec2& direction, float angleThreshold)
{
    if (layerIndex < 0 || layerIndex >= (int)activeAnimations.size())
        return;
        
    activeAnimations[layerIndex].direction = direction;
    activeAnimations[layerIndex].angleThreshold = angleThreshold;
}

void Animator::SetUpperLowerBodySeparation(int layerIndex, bool upperBodyOnly, bool lowerBodyOnly)
{
    if (layerIndex < 0 || layerIndex >= (int)activeAnimations.size())
        return;
        
    activeAnimations[layerIndex].upperBodyOnly = upperBodyOnly;
    activeAnimations[layerIndex].lowerBodyOnly = lowerBodyOnly;
}

void Animator::CrossFadeBetweenLayers(int fromLayer, int toLayer, float duration)
{
    if (fromLayer < 0 || fromLayer >= (int)activeAnimations.size() ||
        toLayer < 0 || toLayer >= (int)activeAnimations.size())
        return;
        
    // Fade out the 'from' layer
    activeAnimations[fromLayer].targetWeight = 0.0f;
    activeAnimations[fromLayer].blendDuration = duration;
    activeAnimations[fromLayer].blendProgress = 0.0f;
    
    // Fade in the 'to' layer
    activeAnimations[toLayer].targetWeight = 1.0f;
    activeAnimations[toLayer].blendDuration = duration;
    activeAnimations[toLayer].blendProgress = 0.0f;
}

void Animator::SetLayerSyncPoint(int layerIndex, Animation* animation, float syncTime)
{
    if (layerIndex < 0 || layerIndex >= (int)activeAnimations.size())
        return;
        
    if (activeAnimations[layerIndex].animation == animation) {
        activeAnimations[layerIndex].syncTime = syncTime;
    }
}

Animation Animator::RetargetAnimationToSkeleton(const Animation& sourceAnim, const Skeleton& targetSkel, float scale)
{
    if (!skeleton) {
        // If no source skeleton is available, return the original animation
        return sourceAnim;
    }
    
    // Use the AnimationRetargeting utility class
    return AnimationRetargeting::RetargetAnimation(sourceAnim, *skeleton, targetSkel, scale);
}

Animation Animator::RetargetMixamoAnimationToSkeleton(const Animation& sourceAnim, const Skeleton& targetSkel, float scale)
{
    if (!skeleton) {
        // If no source skeleton is available, return the original animation
        return sourceAnim;
    }
    
    // First normalize the Mixamo animation to fix pose issues
    Animation normalizedAnim = AnimationRetargeting::NormalizeToStandardPose(sourceAnim, *skeleton);
    
    // Then retarget to the target skeleton
    return AnimationRetargeting::RetargetAnimation(normalizedAnim, *skeleton, targetSkel, scale);
}

Animation Animator::NormalizeAnimation(const Animation& sourceAnim, const Skeleton& skeleton)
{
    // Normalize the animation to fix common pose issues
    return AnimationRetargeting::NormalizeToStandardPose(sourceAnim, skeleton);
}

void Animator::SetAnimationQuality(AnimationQualityLevel level)
{
    qualityLevel = level;
    
    // Apply quality settings to all active animations
    switch (level) {
        case AnimationQualityLevel::HIGH:
            // No compression, full detail
            for (auto& layer : activeAnimations) {
                if (layer.animation) {
                    layer.animation->ReduceKeyframes(0.001f); // Minimal reduction
                }
            }
            break;
        case AnimationQualityLevel::MEDIUM:
            for (auto& layer : activeAnimations) {
                if (layer.animation) {
                    layer.animation->ReduceKeyframes(0.01f);
                }
            }
            break;
        case AnimationQualityLevel::LOW:
            for (auto& layer : activeAnimations) {
                if (layer.animation) {
                    layer.animation->ReduceKeyframes(0.05f);
                }
            }
            break;
        case AnimationQualityLevel::VERY_LOW:
            for (auto& layer : activeAnimations) {
                if (layer.animation) {
                    layer.animation->ReduceKeyframes(0.1f);
                }
            }
            break;
    }
}

void Animator::SetDistanceBasedLOD(float nearDist, float farDist)
{
    lodNearDistance = nearDist;
    lodFarDistance = farDist;
}

void Animator::SetBoneLODThreshold(int highDetailBones, int lowDetailBones)
{
    highDetailBoneThreshold = highDetailBones;
    lowDetailBoneThreshold = lowDetailBones;
}

void Animator::UpdateAnimationLOD(const glm::vec3& viewerPosition, const glm::vec3& modelPosition)
{
    if (!lodEnabled) return;
    
    float distance = glm::distance(viewerPosition, modelPosition);
    
    if (distance < lodNearDistance) {
        // Close - high detail
        SetAnimationQuality(AnimationQualityLevel::HIGH);
    } else if (distance < lodFarDistance) {
        // Medium distance - medium detail
        SetAnimationQuality(AnimationQualityLevel::MEDIUM);
    } else {
        // Far away - low detail
        SetAnimationQuality(AnimationQualityLevel::LOW);
    }

    // Also adjust based on number of bones if needed
    if (skeleton && static_cast<int>(skeleton->bones.size()) > highDetailBoneThreshold) {
        // Too many bones for high quality, reduce quality
        if (qualityLevel == AnimationQualityLevel::HIGH) {
            SetAnimationQuality(AnimationQualityLevel::MEDIUM);
        }
    }
}

// ============================================================
// FOOT IK IMPLEMENTATION
// ============================================================

void Animator::SetFootIKEnabled(bool enabled)
{
    footIKSettings.enabled = enabled;
}

void Animator::SetFootIKSettings(const FootIKSettings& settings)
{
    footIKSettings = settings;
}

void Animator::SetFloorHeight(float height)
{
    footIKSettings.floorHeight = height;
}

void Animator::SetFootBones(int leftFoot, int rightFoot, int leftToe, int rightToe)
{
    footIKSettings.leftFootBone = leftFoot;
    footIKSettings.rightFootBone = rightFoot;
    footIKSettings.leftToeBone = leftToe;
    footIKSettings.rightToeBone = rightToe;
}

void Animator::UpdateFootIK(float dt, const glm::mat4& modelMatrix, bool isMoving)
{
    if (!footIKSettings.enabled || !skeleton) return;

    // Disable foot locking when moving - feet should follow animation naturally
    if (isMoving) {
        // Release any locked feet
        leftFootIK.isLocked = false;
        leftFootIK.lockWeight = glm::max(0.0f, leftFootIK.lockWeight - dt * footIKSettings.footLockReleaseSpeed * 2.0f);
        leftFootIK.ankleOffset = glm::vec3(0.0f);
        
        rightFootIK.isLocked = false;
        rightFootIK.lockWeight = glm::max(0.0f, rightFootIK.lockWeight - dt * footIKSettings.footLockReleaseSpeed * 2.0f);
        rightFootIK.ankleOffset = glm::vec3(0.0f);
        return;
    }

    int leftFoot = footIKSettings.leftFootBone;
    int rightFoot = footIKSettings.rightFootBone;

    if (leftFoot < 0 || rightFoot < 0) return;  // Foot bones not set

    float floorY = footIKSettings.floorHeight;
    
    // Update left foot IK
    if (leftFoot >= 0 && leftFoot < (int)currBoneWorldPos.size()) {
        glm::vec3 footWorldPos = modelMatrix * glm::vec4(currBoneWorldPos[leftFoot], 1.0f);
        
        // Check if foot is near floor
        float distToFloor = footWorldPos.y - floorY;
        bool nearFloor = distToFloor < 0.1f && distToFloor > -0.05f;
        
        // Check if foot is moving slowly (planted)
        float footSpeed = glm::length(currBoneWorldPos[leftFoot] - prevBoneWorldPos[leftFoot]);
        bool isStationary = footSpeed < 0.05f;
        
        // Lock foot when it's near floor and stationary
        if (nearFloor && isStationary && !leftFootIK.isLocked) {
            leftFootIK.isLocked = true;
            leftFootIK.lockedPosition = footWorldPos;
            leftFootIK.lockedPosition.y = floorY;  // Snap to floor
            leftFootIK.targetPosition = footWorldPos;
        }
        
        // Update lock weight
        if (leftFootIK.isLocked) {
            leftFootIK.lockWeight = glm::min(1.0f, leftFootIK.lockWeight + dt * footIKSettings.footLockBlend);
            leftFootIK.timeSinceLock += dt;
            
            // Release lock when foot moves up
            if (footWorldPos.y > floorY + 0.1f || !nearFloor) {
                leftFootIK.isLocked = false;
            }
        } else {
            leftFootIK.lockWeight = glm::max(0.0f, leftFootIK.lockWeight - dt * footIKSettings.footLockReleaseSpeed);
            leftFootIK.timeSinceLock = 0.0f;
        }
        
        // Calculate IK offset
        float ikWeight = leftFootIK.lockWeight * footIKSettings.ikStrength;
        if (ikWeight > 0.001f) {
            glm::vec3 targetPos = leftFootIK.lockedPosition;
            glm::vec3 currentPos = footWorldPos;
            
            // Apply offset in world space, then convert to local
            glm::vec3 offset = targetPos - currentPos;
            
            // Clamp offset
            if (glm::length(offset) > footIKSettings.maxIKDistance) {
                offset = glm::normalize(offset) * footIKSettings.maxIKDistance;
            }
            
            leftFootIK.ankleOffset = offset * ikWeight;
        } else {
            leftFootIK.ankleOffset = glm::vec3(0.0f);
        }
    }
    
    // Update right foot IK (same logic)
    if (rightFoot >= 0 && rightFoot < (int)currBoneWorldPos.size()) {
        glm::vec3 footWorldPos = modelMatrix * glm::vec4(currBoneWorldPos[rightFoot], 1.0f);
        
        float distToFloor = footWorldPos.y - floorY;
        bool nearFloor = distToFloor < 0.1f && distToFloor > -0.05f;
        
        float footSpeed = glm::length(currBoneWorldPos[rightFoot] - prevBoneWorldPos[rightFoot]);
        bool isStationary = footSpeed < 0.05f;
        
        if (nearFloor && isStationary && !rightFootIK.isLocked) {
            rightFootIK.isLocked = true;
            rightFootIK.lockedPosition = footWorldPos;
            rightFootIK.lockedPosition.y = floorY;
            rightFootIK.targetPosition = footWorldPos;
        }
        
        if (rightFootIK.isLocked) {
            rightFootIK.lockWeight = glm::min(1.0f, rightFootIK.lockWeight + dt * footIKSettings.footLockBlend);
            rightFootIK.timeSinceLock += dt;
            
            if (footWorldPos.y > floorY + 0.1f || !nearFloor) {
                rightFootIK.isLocked = false;
            }
        } else {
            rightFootIK.lockWeight = glm::max(0.0f, rightFootIK.lockWeight - dt * footIKSettings.footLockReleaseSpeed);
            rightFootIK.timeSinceLock = 0.0f;
        }
        
        float ikWeight = rightFootIK.lockWeight * footIKSettings.ikStrength;
        if (ikWeight > 0.001f) {
            glm::vec3 targetPos = rightFootIK.lockedPosition;
            glm::vec3 currentPos = footWorldPos;
            glm::vec3 offset = targetPos - currentPos;
            
            if (glm::length(offset) > footIKSettings.maxIKDistance) {
                offset = glm::normalize(offset) * footIKSettings.maxIKDistance;
            }
            
            rightFootIK.ankleOffset = offset * ikWeight;
        } else {
            rightFootIK.ankleOffset = glm::vec3(0.0f);
        }
    }
    
    // Apply IK offsets
    if (leftFoot >= 0 && leftFoot < (int)ikOffsets.size()) {
        ikOffsets[leftFoot] += leftFootIK.ankleOffset;
    }
    if (rightFoot >= 0 && rightFoot < (int)ikOffsets.size()) {
        ikOffsets[rightFoot] += rightFootIK.ankleOffset;
    }
}

void Animator::DebugDrawFootIK()
{
    // This would be implemented with debug rendering
    // For now, just print status
    if (footIKSettings.enabled) {
        std::cout << "[FootIK] Left: locked=" << (leftFootIK.isLocked ? "YES" : "NO") 
                  << " weight=" << leftFootIK.lockWeight
                  << " | Right: locked=" << (rightFootIK.isLocked ? "YES" : "NO")
                  << " weight=" << rightFootIK.lockWeight << "\n";
    }
}
