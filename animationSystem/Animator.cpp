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

    std::cout << "[Animator] Created with skeleton containing " << boneCount << " bones\n";
    if (skeleton)
    {
        std::cout << "[Animator] Skeleton root bone index: " << skeleton->rootBoneIndex << "\n";
        std::cout << "[Animator] Global inverse transform:\n";
        std::cout << "  " << glm::to_string(skeleton->globalInverseTransform[0]) << "\n";
        std::cout << "  " << glm::to_string(skeleton->globalInverseTransform[1]) << "\n";
        std::cout << "  " << glm::to_string(skeleton->globalInverseTransform[2]) << "\n";
        std::cout << "  " << glm::to_string(skeleton->globalInverseTransform[3]) << "\n";
    }
    
    // Initialize caching system
    cachedAnimationTimes.reserve(10); // Reserve space for up to 10 animations
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

void Animator::BlendTo(Animation *anim, float duration)
{
    if (!anim || anim == current)
        return;

    next = anim;
    blendTime = 0.0f;
    blendDuration = glm::max(duration, 0.001f);

    // Also handle the new animation system
    if (activeAnimations.empty() && current) {
        // If we're using the old system, convert to new system
        activeAnimations.emplace_back(current, 1.0f, duration);
        activeAnimations.emplace_back(anim, 0.0f, duration);
    } else {
        // Add the new animation with 0 initial weight
        activeAnimations.emplace_back(anim, 0.0f, duration);
        
        // Reduce the weight of the previous animations
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
        std::cout << "[Animator::Update] ERROR: No skeleton!\n";
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

    std::cout << "[Animator::Update] dt=" << dt << ", active animations: " << activeAnimations.size() << "\n";

    // Initialize matrices
    finalBoneMatrices.assign(skeleton->bones.size(), glm::mat4(1.0f));
    globalBoneMatrices.assign(skeleton->bones.size(), glm::mat4(1.0f));

    std::cout << "[Animator::Update] About to evaluate nodes, skeleton root node name: " << skeleton->rootNode.name << "\n";
    std::cout << "[Animator::Update] Number of skeleton bones: " << skeleton->bones.size() << "\n";

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
                }
                else
                {
                    // Multiple animations - we need to blend them
                    // This is a simplified approach that prioritizes the animation with highest weight
                    const AnimationLayer* dominantLayer = nullptr;
                    float maxWeight = -1.0f;
                    
                    for (const auto& layer : activeAnimations)
                    {
                        if (layer.animation && layer.weight > maxWeight && layer.enabled)
                        {
                            maxWeight = layer.weight;
                            dominantLayer = &layer;
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
    }
    else
    {
        // Handle legacy system if no active animations
        Animation *savedCurrent = current;
        if (!DEBUG_PAUSE_ANIM && current)
        {
            animatorTime += dt * current->GetTicksPerSecond();
            animatorTime = fmod(animatorTime, current->GetDuration());
            std::cout << "[Animator::Update] Animation time updated: " << animatorTime << "/" << current->GetDuration() << "\n";
        }
        else
        {
            current = nullptr;
            std::cout << "[Animator::Update] Animation paused or null\n";
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
        std::cout << "[Animator::Update] Root bone position: " << glm::to_string(currRoot) << "\n";

        if (!hasPrevRoot)
        {
            prevRootPos = currRoot;
            hasPrevRoot = true;
            std::cout << "[Animator::Update] Initialized root position\n";
        }
        else
        {
            rootMotionDelta = currRoot - prevRootPos;
            prevRootPos = currRoot;
            std::cout << "[Animator::Update] Root motion delta: " << glm::to_string(rootMotionDelta) << "\n";
        }

        rootMotionDelta.y = 0.0f;
    }

    std::cout << "[Animator::Update] Completed update, final matrices size: " << finalBoneMatrices.size() << "\n";
}

void Animator::EvaluateNode(
    const AssimpNodeData &node,
    const glm::mat4 &parent,
    Animation *blendAnim,
    float blendFactor)
{
    std::string name = NormalizeBoneName(node.name);

    std::cout << "[EvaluateNode] Processing bone: " << name << ", node.boneIndex: " << node.boneIndex << "\n";

    glm::mat4 bindLocal = node.transform;

    // ---- Decompose bind pose ----
    glm::vec3 bindScale, bindPos, skew;
    glm::quat bindRot;
    glm::vec4 perspective;

    glm::decompose(bindLocal, bindScale, bindRot, bindPos, skew, perspective);
    bindRot = glm::normalize(bindRot);

    std::cout << "[EvaluateNode] Bind pose - Pos: " << glm::to_string(bindPos)
              << ", Rot: " << glm::to_string(bindRot)
              << ", Scale: " << glm::to_string(bindScale) << "\n";

    // ---- Start from bind pose ----
    glm::vec3 pos = bindPos;
    glm::quat rot = bindRot;
    glm::vec3 scale = bindScale;

    // ---- Apply animation (override bind channels) ----
    Animation *animationToUse = blendAnim ? blendAnim : current;
    float timeToUse = blendAnim ? blendFactor : animatorTime; // Use the blendFactor as the time for the blend animation

    if (animationToUse)
    {
        std::cout << "[EvaluateNode] Animation to use: " << animationToUse->name << "\n";
        if (const BoneAnimation *boneAnim = animationToUse->GetBoneAnimation(name))
        {
            std::cout << "[EvaluateNode] Found bone animation for: " << name << "\n";

            // Animation processing for all bones

            if (boneAnim->HasRotationAnimation())
            {
                rot = boneAnim->InterpolateRotation(timeToUse);
                std::cout << "[EvaluateNode] Applied rotation animation to: " << name << "\n";
            }

            if (boneAnim->HasScaleAnimation())
            {
                scale = boneAnim->InterpolateScale(timeToUse);
                std::cout << "[EvaluateNode] Applied scale animation to: " << name << "\n";
            }

            if (debugForceIdentityScale)
                scale = glm::vec3(1.0f);

            if (boneAnim->HasPositionAnimation())
            {
                pos = boneAnim->InterpolatePosition(timeToUse);
                std::cout << "[EvaluateNode] Applied position animation to bone: " << name << "\n";
            }
            else
            {
                std::cout << "[EvaluateNode] Kept bind pose position for bone: " << name << "\n";
            }
        }
        else
        {
            std::cout << "[EvaluateNode] No bone animation found for: " << name << "\n";
        }
    }
    else
    {
        std::cout << "[EvaluateNode] No animation to use\n";
    }

    // ---- Rebuild local transform ----
    glm::mat4 localTransform =
        glm::translate(glm::mat4(1.0f), pos) *
        glm::mat4_cast(rot) *
        glm::scale(glm::mat4(1.0f), scale);

    std::cout << "[EvaluateNode] Local transform built for: " << name << "\n";

    // ---- Global ----
    glm::mat4 globalTransform = parent * localTransform;

    int boneIndex = node.boneIndex;

    if (boneIndex != -1)
    {
        std::cout << "[EvaluateNode] Processing bone index: " << boneIndex << " for: " << name << "\n";

        glm::mat4 ikOffsetMat =
            glm::translate(glm::mat4(1.0f), ikOffsets[boneIndex]);

        glm::mat4 finalGlobal = globalTransform * ikOffsetMat;

        globalBoneMatrices[boneIndex] = finalGlobal;

        finalBoneMatrices[boneIndex] =
            skeleton->globalInverseTransform *
            finalGlobal *
            skeleton->bones[boneIndex].offset;

        std::cout << "[EvaluateNode] Final bone matrix computed for index: " << boneIndex << "\n";

        // Pass the finalGlobal (with IK offset) as parent to children to maintain proper hierarchy
        for (const auto &child : node.children)
        {
            std::cout << "[EvaluateNode] Recursing to child of: " << name << "\n";
            EvaluateNode(child, finalGlobal, blendAnim, blendFactor);
        }
    }
    else
    {
        std::cout << "[EvaluateNode] Bone index is -1 for: " << name << ", skipping matrix computation\n";

        // Still pass the global transform to children for proper hierarchy
        for (const auto &child : node.children)
        {
            std::cout << "[EvaluateNode] Recursing to child of: " << name << "\n";
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
    return std::abs(currBoneWorldPos[bone].y - prevBoneWorldPos[bone].y) < 0.001f;
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
    std::cout << "[GetFinalBoneMatrices] Returning " << finalBoneMatrices.size() << " matrices\n";
    if (!finalBoneMatrices.empty())
    {
        std::cout << "[GetFinalBoneMatrices] First matrix: " << glm::to_string(finalBoneMatrices[0][0]) << "\n";
        std::cout << "[GetFinalBoneMatrices] Last matrix: " << glm::to_string(finalBoneMatrices.back()[0]) << "\n";
    }
    return finalBoneMatrices;
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
    // Update animation times and blend weights
    for (auto& layer : activeAnimations)
    {
        if (layer.animation && layer.enabled)
        {
            layer.time += dt * layer.animation->GetTicksPerSecond();
            layer.time = fmod(layer.time, layer.animation->GetDuration());
            
            // Update blend progress if needed
            if (layer.blendProgress < 1.0f)
            {
                layer.blendProgress = glm::min(1.0f, layer.blendProgress + (dt / layer.blendDuration));
                
                // Interpolate weight based on blend progress
                layer.weight = glm::mix(layer.weight, layer.targetWeight, layer.blendProgress);
            }
            
            // Handle fade in/out effects
            // Note: We would need to store original durations to properly calculate progress
            // For now, we'll implement a simpler approach by tracking fade state separately
            // This would require extending the AnimationLayer structure to store original values
            // For simplicity, we'll just handle the timing here
            if (layer.fadeInDuration > 0.0f)
            {
                // Fade in logic
                layer.fadeInDuration -= dt;
                if (layer.fadeInDuration <= 0.0f)
                {
                    layer.fadeInDuration = 0.0f;
                }
            }
            
            if (layer.fadeOutDuration > 0.0f)
            {
                // Fade out logic
                layer.fadeOutDuration -= dt;
                if (layer.fadeOutDuration <= 0.0f)
                {
                    layer.fadeOutDuration = 0.0f;
                    // Optionally disable the layer when fade out is complete
                    // layer.enabled = false; // Uncomment if you want auto-disable
                }
            }
            
            // Trigger animation events if any occur at this time
            for (auto& event : animationEvents)
            {
                if (event.animation == layer.animation)
                {
                    // Check if we just passed an event time
                    float lastFrameTime = layer.time - (dt * layer.animation->GetTicksPerSecond());
                    bool shouldTrigger = false;
                    
                    // Check if the event should trigger
                    if ((lastFrameTime <= event.time && layer.time >= event.time) ||
                        (lastFrameTime > layer.time && (lastFrameTime >= event.time || layer.time <= event.time))) // Handle loop-around
                    {
                        shouldTrigger = true;
                    }
                    
                    if (shouldTrigger && (event.repeatable || !event.triggered))
                    {
                        // Mark as triggered if not repeatable
                        if (!event.repeatable) {
                            const_cast<AnimationEventTrigger&>(event).triggered = true;
                        }
                        
                        // Call the custom callback if available
                        if (event.callback) {
                            event.callback();
                        }
                        // Otherwise call the default callback
                        else if (eventCallback) {
                            eventCallback(event.eventName);
                        }
                    }
                }
            }
        }
    }
    
    // Update queued animations
    for (auto& queued : queuedAnimations)
    {
        queued.blendProgress = glm::min(1.0f, queued.blendProgress + (dt / queued.blendDuration));
        queued.weight = glm::mix(0.0f, queued.targetWeight, queued.blendProgress);
        
        if (queued.blendProgress >= 1.0f)
        {
            // Move to active animations
            activeAnimations.push_back(queued);
        }
    }
    
    // Remove completed queued animations
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
    if (skeleton && skeleton->bones.size() > highDetailBoneThreshold) {
        // Too many bones for high quality, reduce quality
        if (qualityLevel == AnimationQualityLevel::HIGH) {
            SetAnimationQuality(AnimationQualityLevel::MEDIUM);
        }
    }
}
