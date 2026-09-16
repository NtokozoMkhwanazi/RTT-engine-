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
#include <functional>
#include <iostream>
#include <map>
#include <set>

// No local normalization - use canonical NormalizeBoneName from BoneName.h

// ============================================================
// Helper Functions for Matrix Blending
// ============================================================

/**
 * Decompose a matrix into translation, rotation, and scale
 */
static void decomposeMatrix(const glm::mat4& matrix, glm::vec3& translation, glm::quat& rotation, glm::vec3& scale) {
    glm::vec3 skew;
    glm::vec4 perspective;
    glm::decompose(matrix, scale, rotation, translation, skew, perspective);
}

/**
 * Compose a matrix from translation, rotation, and scale
 */
static glm::mat4 composeMatrix(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale) {
    glm::mat4 matrix = glm::mat4(1.0f);
    matrix = glm::translate(matrix, translation);
    matrix *= glm::mat4_cast(rotation);
    matrix = glm::scale(matrix, scale);
    return matrix;
}

/**
 * Blend two TRS values (fast - no matrix operations)
 */
static Animator::BoneTRS BlendTRS(const Animator::BoneTRS& a, const Animator::BoneTRS& b, float t) {
    Animator::BoneTRS result;
    result.translation = glm::mix(a.translation, b.translation, t);
    result.rotation = glm::normalize(glm::slerp(a.rotation, b.rotation, t));
    result.scale = glm::mix(a.scale, b.scale, t);
    return result;
}

// ============================================================
// Animator Implementation
// ============================================================

// No local normalization - use canonical NormalizeBoneName from BoneName.h

// ------------------------------------------------------------
// Animator
// ------------------------------------------------------------
Animator::~Animator() {
    // Clear all owned resources
    boneBuffer.reset();
    
    // Clear vectors to free memory
    finalBoneMatrices.clear();
    globalBoneMatrices.clear();
    prevBoneWorldPos.clear();
    currBoneWorldPos.clear();
    ikOffsets.clear();
    ikRotations.clear();
    activeAnimations.clear();
    queuedAnimations.clear();
    cachedAnimationTimes.clear();
    cachedBoneTransforms.clear();
    animationEvents.clear();
    
    // Reset pointers (these are NOT owned, just references)
    skeleton = nullptr;
    current = nullptr;
    next = nullptr;
}

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
    ikRotations.resize(boneCount, glm::mat4(1.0f));
    ikFootTilt.resize(boneCount, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

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
    // Reset pelvis drop so the character doesn't snap/jump when a new
    // animation starts (the old drop would be baked into the new pose).
    currentPelvisDropY = 0.0f;
    pelvisDropVelocity = 0.0f;
    cachedPelvisDropY = 0.0f;
}

void Animator::SetCurrentTime(float time)
{
    // If `current` is null (e.g. after Play(nullptr) cleared the old-style
    // animation pointer during idle snapshot capture), but activeAnimations
    // still holds layers with valid animation pointers, derive the time from
    // the first active layer instead.  Returning early here left the layer
    // time and animatorTime stale, which manifested as a 1-frame T-pose snap
    // when the matcher immediately called SetCurrentTime on the recovery frame.
    if (!current) {
        if (!activeAnimations.empty() && activeAnimations[0].animation) {
            float animDuration = activeAnimations[0].animation->GetDuration();
            animatorTime = fmod(time, animDuration);
            if (animatorTime < 0.0f) animatorTime += animDuration;
            activeAnimations[0].time = animatorTime;
        }
        return;
    }

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
    
    // CRITICAL FIX: Reset root motion state when transitioning to new animation
    // This prevents old root motion from affecting the new animation
    prevRootPos = glm::vec3(0.0f);
    rootMotionDelta = glm::vec3(0.0f);
    hasPrevRoot = false;
    // Reset pelvis drop to avoid a visual pop when switching animations.
    currentPelvisDropY = 0.0f;
    pelvisDropVelocity = 0.0f;
    cachedPelvisDropY = 0.0f;
    std::cout << "[BlendTo] Reset root motion state for transition\n";
}

void Animator::BlendToAt(Animation *anim, float time, float duration)
{
    if (!anim) {
        std::cout << "[BlendToAt] NULL animation!\n";
        return;
    }

    // First do the blend
    BlendTo(anim, duration);
    
    // Then set the time to start at the specified position
    SetCurrentTime(time);
    
    // Set the time on the layer(s) that actually play this animation.
    // BlendTo appends the incoming animation at the end of the layer list,
    // so indexing [0] would hit the OLD animation instead.  Match by pointer.
    for (auto& layer : activeAnimations) {
        if (layer.animation == anim) {
            layer.time = time;
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
    lastDeltaTime = dt;

    // Save previous positions BEFORE overwriting
    prevBoneWorldPos = currBoneWorldPos;

    // NOTE: IK offsets are NOT cleared here anymore. UpdateFootIK() SETS the
    // foot offsets every frame (it no longer accumulates), and AddIKOffset()
    // also uses set semantics, so user-set offsets (e.g. HybridMMFSM root
    // snap) persist correctly across frames instead of being wiped before
    // the skeleton is evaluated.

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

        // Keep animatorTime (returned by GetCurrentTime) in lock-step with the
        // active layer's play time. Previously this sync lived only inside
        // evaluateBoneMatrices(), which early-returns when the skeleton has no
        // bones (e.g. test fixtures / headless animation), silently freezing
        // playback time at whatever SetCurrentTime last wrote. Advancing it here
        // — right after the blend tick, before bone evaluation — keeps the clock
        // running even with an empty bone list. (evaluateBoneMatrices() still
        // re-syncs for the bone path, so this is intentionally redundant.)
        if (!activeAnimations.empty())
            animatorTime = activeAnimations[0].time;
    }

    evaluateBoneMatrices();

    // Compute current bone positions (AFTER ikRotations are applied)
    for (size_t i = 0; i < globalBoneMatrices.size(); ++i)
    {
        currBoneWorldPos[i] =
            glm::vec3(globalBoneMatrices[i] * glm::vec4(0, 0, 0, 1));
    }

    // Root motion extraction
    // CRITICAL FIX: Mixamo animations often have static "mixamo.com" root
    // but moving "Hips" bone. We need to check BOTH for root motion.
    rootMotionDelta = glm::vec3(0.0f);

    // First try: Use the skeleton's root bone index
    int rootIdx = skeleton->rootBoneIndex;
    glm::vec3 currRoot(0.0f);
    bool foundRoot = false;

    if (rootIdx >= 0 && rootIdx < (int)currBoneWorldPos.size())
    {
        currRoot = currBoneWorldPos[rootIdx];
        foundRoot = true;
    }

    // Second try: If root bone has no motion, try "Hips" bone (Mixamo style)
    // This fixes the footskating issue where root is static but hips move
    int hipsIdx = skeleton->GetBoneIndex("Hips");
    if (hipsIdx >= 0 && hipsIdx < (int)currBoneWorldPos.size())
    {
        // Check if hips has more motion than root
        glm::vec3 hipsPos = currBoneWorldPos[hipsIdx];
        
        if (!hasPrevRoot)
        {
            prevRootPos = hipsPos;
            hasPrevRoot = true;
            currRoot = hipsPos;
            foundRoot = true;
        }
        else
        {
            glm::vec3 hipsMotion = hipsPos - prevRootPos;
            float hipsMotionMag = glm::length(hipsMotion);
            
            // If hips has significant motion, use it instead of root
            if (hipsMotionMag > 0.001f)
            {
                currRoot = hipsPos;
                foundRoot = true;
            }
        }
    }

    // Compute root motion delta
    if (foundRoot)
    {
        if (!hasPrevRoot)
        {
            prevRootPos = currRoot;
            hasPrevRoot = true;
        }
        else
        {
            rootMotionDelta = (currRoot - prevRootPos) * ikWorldScale;
            prevRootPos = currRoot;
        }

        // Zero out vertical motion (we only want horizontal movement)
        rootMotionDelta.y = 0.0f;
    }

    // DEBUG: Print root motion info if significant
    static float rootMotionDebugTimer = 0.0f;
    rootMotionDebugTimer += dt;
    if (rootMotionDebugTimer > 5.0f && glm::length(rootMotionDelta) > 0.001f)
    {
        std::cout << "[RootMotion] Delta=(" << rootMotionDelta.x << ", " 
                  << rootMotionDelta.y << ", " << rootMotionDelta.z << ")"
                  << " Mag=" << glm::length(rootMotionDelta) << "\n";
        rootMotionDebugTimer = 0.0f;
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

// ------------------------------------------------------------------
// evaluateBoneMatrices — skeleton evaluation (cache-checked) with IK
// applied.  Extracted from Update() so RevalidateIK() can re-run it
// after UpdateFootIK sets fresh ikRotations, eliminating the 1-frame
// IK lag that caused knee stretch+jitter during motion-matching
// transitions.
// ------------------------------------------------------------------
void Animator::evaluateBoneMatrices()
{
    if (!skeleton || skeleton->bones.empty()) return;

    // Save the previous frame's matrices so that the T-pose fallback (total
    // weight ~ 0) can HOLD the last good pose instead of snapping to the
    // bind pose.  This happens on the 1-frame window when the idle snapshot
    // is released and the matcher hasn't yet re-established a layer.
    std::vector<glm::mat4> holdFinal = finalBoneMatrices;
    std::vector<glm::mat4> holdGlobal = globalBoneMatrices;

    // Initialize matrices
    finalBoneMatrices.assign(skeleton->bones.size(), glm::mat4(1.0f));
    globalBoneMatrices.assign(skeleton->bones.size(), glm::mat4(1.0f));

    // Process active animations with proper blending
    if (!activeAnimations.empty())
    {
        // Check if we can use cached transforms
        bool canUseCache = useCaching && cacheValid;
        // Invalidate the cache when the pelvis drop changes — cached
        // finalBoneMatrices would otherwise bake in a stale Hips offset.
        if (canUseCache &&
            std::abs(cachedPelvisDropY - currentPelvisDropY) > 0.001f) {
            canUseCache = false;
        }
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

            for (const auto& layer : activeAnimations)
            {
                if (layer.animation && layer.weight > 0.0f && layer.enabled)
                {
                    totalWeight += std::abs(layer.weight);
                }
            }

            // If total weight is too small, hold the last good pose instead of
            // snapping to bind pose (T-pose).  The layer stack is present but
            // all weights are ~0 (e.g. mid-crossfade during idle-snapshot
            // release) — there is nothing valid to sample this frame.
            if (totalWeight < 0.0001f)
            {
                // DIAGNOSTICS: this is the T-pose path — log active state so
                // the snap trigger can be identified at runtime.
                std::cerr << "[T-POSE] evaluateBoneMatrices: activeAnimations.size()="
                          << activeAnimations.size()
                          << " current=" << static_cast<void*>(current)
                          << " animatorTime=" << animatorTime;
                for (size_t li = 0; li < activeAnimations.size(); ++li) {
                    const auto& layer = activeAnimations[li];
                    std::cerr << " L" << li << "={anim=" << static_cast<void*>(layer.animation)
                              << " w=" << layer.weight << " tw=" << layer.targetWeight
                              << " en=" << layer.enabled << " t=" << layer.time << "}";
                }
                std::cerr << "\n";
                // Hold last good pose — keep the previous frame's matrices
                // rather than evaluating into the bind-pose root node.
                finalBoneMatrices = holdFinal;
                globalBoneMatrices = holdGlobal;
            }
            else
            {
                // Full weighted blending implementation
                if (activeAnimations.size() == 1 && activeAnimations[0].animation && activeAnimations[0].enabled)
                {
                    // Single animation case - straightforward
                    EvaluateNode(skeleton->rootNode, glm::mat4(1.0f), activeAnimations[0].animation, activeAnimations[0].time);
                    dominantLayerIdx = 0;
                }
                else if (activeAnimations.size() > 1)
                {
                    // Multiple animations - TRS-based weighted blending
                    float totalWeight2 = 0.0f;
                    for (const auto& layer : activeAnimations) {
                        if (layer.animation && layer.enabled) {
                            totalWeight2 += layer.weight;
                        }
                    }

                    // FIX (v5 todo Area 2): Clamp over-saturated weights to 1.0f.
                    // When totalWeight2 > 1.0 the layer weights are out of
                    // [0,1] range, which skews the weighted blend of translation,
                    // scale, and rotation before they enter SolveLegIK. Normalise
                    // each layer's weight proportionally so relative ratios are
                    // preserved but the total never exceeds 1.0 (a "uniform
                    // whole"), preventing the leg-length distortion that
                    // over-weighted bones produce.
                    float weightNorm = 1.0f;
                    if (totalWeight2 > 1.0f) {
                        weightNorm = 1.0f / totalWeight2;
                    }

                    if (totalWeight2 > 0.0f)
                    {
                        size_t numBones = finalBoneMatrices.size();

                        std::vector<BoneTRS> blendedTRS(numBones);
                        {
                            std::vector<glm::vec3> accTrans(numBones, glm::vec3(0.0f));
                            std::vector<glm::vec3> accScale(numBones, glm::vec3(0.0f));
                            std::vector<glm::quat> accRot(numBones, glm::quat(0.0f, 0.0f, 0.0f, 0.0f));
                            std::vector<float> accW(numBones, 0.0f);

                            for (const auto& layer : activeAnimations) {
                                if (!layer.animation || !layer.enabled || layer.weight <= 0.0f) continue;

                                // Apply the over-saturation clamp factor.
                                const float w = layer.weight * weightNorm;

                                std::vector<BoneTRS> animTRS;
                                EvaluateNodeTRS(skeleton->rootNode, glm::mat4(1.0f), layer.animation, layer.time, animTRS);

                                size_t blendCount = std::min(numBones, animTRS.size());
                                for (size_t j = 0; j < blendCount; j++) {
                                    if (accW[j] > 0.0f && glm::dot(accRot[j], animTRS[j].rotation) < 0.0f) {
                                        animTRS[j].rotation = -animTRS[j].rotation;
                                    }
                                    accTrans[j] += w * animTRS[j].translation;
                                    accScale[j] += w * animTRS[j].scale;
                                    accRot[j] += w * animTRS[j].rotation;
                                    accW[j] += w;
                                }
                            }

                            for (size_t j = 0; j < numBones; j++) {
                                if (accW[j] > 0.0f) {
                                    float invW = 1.0f / accW[j];
                                    blendedTRS[j].translation = accTrans[j] * invW;
                                    blendedTRS[j].scale = accScale[j] * invW;
                                    // FIX (v5 todo Area 2): Divide by accW[j] before
                                    // normalising so all three channels use the same
                                    // weight normalisation. Mathematically equivalent
                                    // to glm::normalize(accRot[j]) for unit-sum weights,
                                    // but explicit and robust when weights are clamped.
                                    blendedTRS[j].rotation = glm::normalize(accRot[j] * invW);
                                    if (!std::isfinite(blendedTRS[j].rotation.w) ||
                                        !std::isfinite(blendedTRS[j].rotation.x)) {
                                        blendedTRS[j].rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                                    }
                                }
                            }
                        }

                        std::function<void(const AssimpNodeData&, const glm::mat4&)> composeHierarchy;
                        composeHierarchy = [&](const AssimpNodeData& node, const glm::mat4& parentGlobal) {
                            int bIdx = node.boneIndex;
                            if (bIdx >= 0 && bIdx < static_cast<int>(numBones)) {
                                BoneTRS trs = blendedTRS[bIdx];

                                if (ikWorldScale > 0.0f) {
                                    static int hipsBoneIdx = -2;
                                    if (hipsBoneIdx == -2) hipsBoneIdx = skeleton->GetBoneIndex("Hips");
                                    if (bIdx == hipsBoneIdx && hipsBoneIdx != -1) {
                                        trs.translation.y -= (currentPelvisDropY / ikWorldScale);

                                    }
                                }

                                glm::mat4 local =
                                    glm::translate(glm::mat4(1.0f), trs.translation) *
                                    glm::mat4_cast(trs.rotation) *
                                    glm::scale(glm::mat4(1.0f), trs.scale);

                                glm::mat4 global = parentGlobal * local;

                                glm::mat4 ikXform(1.0f);
                                if (static_cast<size_t>(bIdx) < ikRotations.size())
                                    ikXform = ikRotations[bIdx];
                                if ((size_t)bIdx < ikFootTilt.size() &&
                                    glm::length(ikFootTilt[bIdx] - glm::quat(1.0f,0.0f,0.0f,0.0f)) > 1e-6f)
                                    ikXform = ikXform * glm::mat4_cast(ikFootTilt[bIdx]);
                                if (static_cast<size_t>(bIdx) < ikOffsets.size() &&
                                    ikOffsets[bIdx] != glm::vec3(0.0f))
                                    ikXform = ikXform *
                                        glm::translate(glm::mat4(1.0f), ikOffsets[bIdx]);
                                // Root rotation offset (quintic inertialization).
                                if (bIdx == skeleton->rootBoneIndex &&
                                    rootQuatOffset != glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
                                    ikXform = ikXform * glm::mat4_cast(rootQuatOffset);

                                glm::mat4 finalGlobal = global * ikXform;

                                globalBoneMatrices[bIdx] = finalGlobal;
                                finalBoneMatrices[bIdx] =
                                    skeleton->globalInverseTransform *
                                    finalGlobal *
                                    skeleton->bones[bIdx].offset;

                                for (const auto& child : node.children) {
                                    composeHierarchy(child, finalGlobal);
                                }
                            } else {
                                for (const auto& child : node.children) {
                                    composeHierarchy(child, parentGlobal);
                                }
                            }
                        };
                        composeHierarchy(skeleton->rootNode, glm::mat4(1.0f));

                        dominantLayerIdx = 0;
                    }
                    else
                    {
                        // All multi-layer weights zeroed — hold last good pose.
                        finalBoneMatrices = holdFinal;
                        globalBoneMatrices = holdGlobal;
                    }
                }
                else
                {
                    // Unhandled layer count — hold last good pose.
                    finalBoneMatrices = holdFinal;
                    globalBoneMatrices = holdGlobal;
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
                cachedPelvisDropY = currentPelvisDropY;
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
        // Legacy system — hold last good pose if current is null (no
        // animation to sample), otherwise evaluate normally.
        if (current) {
            EvaluateNode(skeleton->rootNode, glm::mat4(1.0f), current, animatorTime);
        } else {
            finalBoneMatrices = holdFinal;
            globalBoneMatrices = holdGlobal;
        }
    }
}

// ------------------------------------------------------------------
// RevalidateIK — re-evaluate the skeleton with the freshly computed
// ikRotations / ikOffsets from UpdateFootIK, so the visual pose
// matches the IK solve in the SAME frame.  Called by
// AnimatedCharacter after matcher->ApplyFootIK().
// ------------------------------------------------------------------
void Animator::RevalidateIK()
{
    // Force cache invalidation — ikRotations changed but the cache
    // key (animation time + pelvis drop) may not reflect that.
    cacheValid = false;

    // Save the matrices from the just-completed evaluateBoneMatrices pass
    // so the T-pose fallback can hold the last good pose instead of
    // snapping to bind pose.  (Same rationale as in evaluateBoneMatrices.)
    std::vector<glm::mat4> holdFinal = finalBoneMatrices;
    std::vector<glm::mat4> holdGlobal = globalBoneMatrices;

    // Re-run the bone hierarchy evaluation with the updated ikRotations.
    // The animation time / pose is unchanged (dt=0 semantics), so this
    // just re-applies the new IK transforms on top of the same animated
    // pose.  prevBoneWorldPos / currBoneWorldPos are NOT touched — they
    // retain the values UpdateFootIK just consumed.
    //
    // Reset only globalBoneMatrices (finalBoneMatrices is not used by
    // the IK path); EvaluateNode / composeHierarchy will repopulate both.
    finalBoneMatrices.assign(skeleton->bones.size(), glm::mat4(1.0f));
    globalBoneMatrices.assign(skeleton->bones.size(), glm::mat4(1.0f));

    if (!activeAnimations.empty())
    {
        float totalWeight = 0.0f;
        for (const auto& layer : activeAnimations)
            if (layer.animation && layer.weight > 0.0f && layer.enabled)
                totalWeight += std::abs(layer.weight);

            if (totalWeight < 0.0001f)
        {
            // DIAGNOSTICS: T-pose path in RevalidateIK — same trigger as
            // evaluateBoneMatrices. Log so we can catch snaps during foot IK.
            std::cerr << "[T-POSE] RevalidateIK: activeAnimations.size()="
                      << activeAnimations.size()
                      << " current=" << static_cast<void*>(current)
                      << " animatorTime=" << animatorTime;
            for (size_t li = 0; li < activeAnimations.size(); ++li) {
                const auto& layer = activeAnimations[li];
                std::cerr << " L" << li << "={anim=" << static_cast<void*>(layer.animation)
                          << " w=" << layer.weight << " tw=" << layer.targetWeight
                          << " en=" << layer.enabled << " t=" << layer.time << "}";
            }
            std::cerr << "\n";
            // Hold last good pose instead of bind-pose snap.
            finalBoneMatrices = holdFinal;
            globalBoneMatrices = holdGlobal;
        }
        else if (activeAnimations.size() == 1 && activeAnimations[0].animation && activeAnimations[0].enabled)
        {
            EvaluateNode(skeleton->rootNode, glm::mat4(1.0f),
                         activeAnimations[0].animation, activeAnimations[0].time);
        }
        else if (activeAnimations.size() > 1)
        {
            // Multi-layer blend: re-evaluate each layer to TRS and blend,
            // then compose through the hierarchy (same as evaluateBoneMatrices).
            size_t numBones = finalBoneMatrices.size();
            float tw = 0.0f;
            for (const auto& layer : activeAnimations)
                if (layer.animation && layer.enabled) tw += layer.weight;

            if (tw > 0.0f)
            {
                std::vector<BoneTRS> blendedTRS(numBones);
                {
                    std::vector<glm::vec3> accTrans(numBones, glm::vec3(0.0f));
                    std::vector<glm::vec3> accScale(numBones, glm::vec3(0.0f));
                    std::vector<glm::quat> accRot(numBones, glm::quat(0.0f, 0.0f, 0.0f, 0.0f));
                    std::vector<float> accW(numBones, 0.0f);

                    for (const auto& layer : activeAnimations) {
                        if (!layer.animation || !layer.enabled || layer.weight <= 0.0f) continue;
                        std::vector<BoneTRS> animTRS;
                        EvaluateNodeTRS(skeleton->rootNode, glm::mat4(1.0f),
                                        layer.animation, layer.time, animTRS);
                        size_t bc = std::min(numBones, animTRS.size());
                        for (size_t j = 0; j < bc; j++) {
                            if (accW[j] > 0.0f && glm::dot(accRot[j], animTRS[j].rotation) < 0.0f)
                                animTRS[j].rotation = -animTRS[j].rotation;
                            accTrans[j] += layer.weight * animTRS[j].translation;
                            accScale[j] += layer.weight * animTRS[j].scale;
                            accRot[j]  += layer.weight * animTRS[j].rotation;
                            accW[j]    += layer.weight;
                        }
                    }
                    for (size_t j = 0; j < numBones; j++) {
                        if (accW[j] > 0.0f) {
                            blendedTRS[j].translation = accTrans[j] / accW[j];
                            blendedTRS[j].scale = accScale[j] / accW[j];
                            blendedTRS[j].rotation = glm::normalize(accRot[j]);
                            if (!std::isfinite(blendedTRS[j].rotation.w) ||
                                !std::isfinite(blendedTRS[j].rotation.x))
                                blendedTRS[j].rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                        }
                    }
                }

                std::function<void(const AssimpNodeData&, const glm::mat4&)> compose;
                compose = [&](const AssimpNodeData& node, const glm::mat4& parentGlobal) {
                    int bIdx = node.boneIndex;
                    if (bIdx >= 0 && bIdx < static_cast<int>(numBones)) {
                        BoneTRS trs = blendedTRS[bIdx];
                        if (ikWorldScale > 0.0f) {
                            static int hipsBoneIdx = -2;
                            if (hipsBoneIdx == -2) hipsBoneIdx = skeleton->GetBoneIndex("Hips");
                            if (bIdx == hipsBoneIdx && hipsBoneIdx != -1) {
                                trs.translation.y -= (currentPelvisDropY / ikWorldScale);
                            }
                        }
                        glm::mat4 local = glm::translate(glm::mat4(1.0f), trs.translation) *
                                          glm::mat4_cast(trs.rotation) *
                                          glm::scale(glm::mat4(1.0f), trs.scale);
                        glm::mat4 global = parentGlobal * local;
                        glm::mat4 ikXform(1.0f);
                        if ((size_t)bIdx < ikRotations.size()) ikXform = ikRotations[bIdx];
                        if ((size_t)bIdx < ikFootTilt.size() &&
                            glm::length(ikFootTilt[bIdx] - glm::quat(1.0f,0.0f,0.0f,0.0f)) > 1e-6f)
                            ikXform = ikXform * glm::mat4_cast(ikFootTilt[bIdx]);
                        if ((size_t)bIdx < ikOffsets.size() && ikOffsets[bIdx] != glm::vec3(0.0f))
                            ikXform = ikXform * glm::translate(glm::mat4(1.0f), ikOffsets[bIdx]);
                        if (bIdx == skeleton->rootBoneIndex &&
                            rootQuatOffset != glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
                            ikXform = ikXform * glm::mat4_cast(rootQuatOffset);
                        glm::mat4 finalGlobal = global * ikXform;
                        globalBoneMatrices[bIdx] = finalGlobal;
                        finalBoneMatrices[bIdx] = skeleton->globalInverseTransform *
                                                   finalGlobal *
                                                   skeleton->bones[bIdx].offset;
                        for (const auto& child : node.children)
                            compose(child, finalGlobal);
                    } else {
                        for (const auto& child : node.children)
                            compose(child, parentGlobal);
                    }
                };
                compose(skeleton->rootNode, glm::mat4(1.0f));
            }
            else
            {
                // All multi-layer weights zeroed — hold last good pose.
                finalBoneMatrices = holdFinal;
                globalBoneMatrices = holdGlobal;
            }
        }
        else
        {
            // Unhandled layer count — hold last good pose.
            finalBoneMatrices = holdFinal;
            globalBoneMatrices = holdGlobal;
        }
    }
    else
    {
        // Legacy system — hold last good pose if current is null.
        if (current) {
            EvaluateNode(skeleton->rootNode, glm::mat4(1.0f), current, animatorTime);
        } else {
            finalBoneMatrices = holdFinal;
            globalBoneMatrices = holdGlobal;
        }
    }

    // [debug] Verify visual knee matches IK solve
    static int dbgReval = 0;
    if (dbgReval < 2000) {
        dbgReval++;
        int lt = footIKSettings.leftUpLegBone;
        int ls = footIKSettings.leftLegBone;
        int lf = footIKSettings.leftFootBone;
        if (lt >= 0 && lt < (int)globalBoneMatrices.size() &&
            ls >= 0 && ls < (int)globalBoneMatrices.size() &&
            lf >= 0 && lf < (int)globalBoneMatrices.size()) {
            glm::vec3 h = glm::vec3(globalBoneMatrices[lt][3]);
            glm::vec3 k = glm::vec3(globalBoneMatrices[ls][3]);
            glm::vec3 a = glm::vec3(globalBoneMatrices[lf][3]);
            glm::vec3 hk = h - k;
            glm::vec3 ak = a - k;
            float knee = acosf(glm::clamp(glm::dot(hk, ak) / (glm::length(hk) * glm::length(ak)), -1.0f, 1.0f));
            if (glm::degrees(knee) < 140.0f) {  // only print when knee is too low
                bool thighNonId = (size_t)lt < ikRotations.size() && ikRotations[lt][0][0] < 0.999f;
                bool shinNonId = (size_t)ls < ikRotations.size() && ikRotations[ls][0][0] < 0.999f;
                std::cout << "[RevalidateIK] visualKnee=" << glm::degrees(knee)
                          << " thighNonId=" << (thighNonId?"YES":"no")
                          << " shinNonId=" << (shinNonId?"YES":"no")
                          << " lt=" << lt << " ls=" << ls << " lf=" << lf
                          << " thighW=" << ikRotations[lt][3][0]
                          << "\n";
            }
        }
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

            // FIX (todo: "Terminate Micro-Scale Drift"): Flatten sub-0.5% scale
            // keys to identity before they accumulate down the Hips->Leg chain.
            // FBX/Mixamo clips bake tiny ~0.999x scale drift into bone keys;
            // compounded over the leg hierarchy this distorts L1/L2 limb lengths
            // and surfaces as leg stretching (more visible at larger visualScale).
            if (debugForceIdentityScale ||
                (std::abs(scale.x - 1.0f) < 0.005f &&
                 std::abs(scale.y - 1.0f) < 0.005f &&
                 std::abs(scale.z - 1.0f) < 0.005f))
                scale = glm::vec3(1.0f);
        }
    }

    // ---- PELVIS HEIGHT ADJUSTMENT: inject the smoothed pelvis drop as a
    // local-Y translation on the Hips bone so it propagates down both leg
    // chains (thigh → shin → ankle). Computed from the previous frame's IK
    // targets in CalculatePelvisAdjustment, which runs after this evaluation
    // step — so the drop is applied with a 1-frame lag, masked by the
    // asymmetric smoothing (drop fast, rise slow).
    if (node.boneIndex != -1 && ikWorldScale > 0.0f) {
        static int hipsBoneIdx = -2;
        if (hipsBoneIdx == -2) hipsBoneIdx = skeleton->GetBoneIndex("Hips");
        if (node.boneIndex == hipsBoneIdx && hipsBoneIdx != -1) {
            pos.y -= (currentPelvisDropY / ikWorldScale);
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
        // Apply IK: a LOCAL-frame rotation (knee-bend, identity by default)
        // followed by the ankle TRANSLATE, then propagate to children via the
        // recursion (parentGlobal = finalGlobal) - so a thigh rotation swings
        // the shin+foot, a shin rotation swings the foot, exactly like ikOffsets.
        glm::mat4 ikXform = ikRotations[boneIndex];
        if ((size_t)boneIndex < ikFootTilt.size() &&
            glm::length(ikFootTilt[boneIndex] - glm::quat(1.0f,0.0f,0.0f,0.0f)) > 1e-6f)
            ikXform = ikXform * glm::mat4_cast(ikFootTilt[boneIndex]);
        if (ikOffsets[boneIndex] != glm::vec3(0.0f))
            ikXform = ikXform * glm::translate(glm::mat4(1.0f), ikOffsets[boneIndex]);
        if (boneIndex == skeleton->rootBoneIndex &&
            rootQuatOffset != glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
            ikXform = ikXform * glm::mat4_cast(rootQuatOffset);

        glm::mat4 finalGlobal = globalTransform * ikXform;

        globalBoneMatrices[boneIndex] = finalGlobal;

        // SKINNING FORMULA (standard Assimp):
        // finalBoneMatrix = globalInverseTransform * globalTransform * boneOffset
        // Without globalInverseTransform, FBX assets whose root node has a
        // non-identity transform (e.g. Mixamo's "mixamo.com" root with a
        // baked rotation/scale) place vertices in root-local space instead of
        // model space → T-pose / deformed vertices. The globalInverseTransform
        // cancels the root node transform so offset matrices (which map from
        // model space) line up with the animated global transforms.
        finalBoneMatrices[boneIndex] =
            skeleton->globalInverseTransform *
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

// ============================================================
// TRS-based Evaluation (for fast multi-layer blending)
// ============================================================
void Animator::EvaluateNodeTRS(
    const AssimpNodeData &node,
    const glm::mat4& parentGlobal,
    Animation* anim,
    float time,
    std::vector<Animator::BoneTRS>& outTRS)
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
    glm::vec3 pos = bindPos;
    glm::quat rot = bindRot;
    glm::vec3 scale = bindScale;

    // ---- Apply animation (override bind channels) ----
    if (anim)
    {
        if (const BoneAnimation *boneAnim = anim->GetBoneAnimation(name))
        {
            if (boneAnim->HasPositionAnimation())
            {
                pos = boneAnim->InterpolatePosition(time);
                
                if (lockRootPosition && node.boneIndex == skeleton->rootBoneIndex) {
                    pos = bindPos;
                }
            }

            if (boneAnim->HasRotationAnimation())
            {
                rot = boneAnim->InterpolateRotation(time);
            }

            if (boneAnim->HasScaleAnimation())
            {
                scale = boneAnim->InterpolateScale(time);
            }

            // FIX (todo: "Terminate Micro-Scale Drift"): Flatten sub-0.5% scale
            // keys to identity before they accumulate down the Hips->Leg chain.
            // FBX/Mixamo clips bake tiny ~0.999x scale drift into bone keys;
            // compounded over the leg hierarchy this distorts L1/L2 limb lengths
            // and surfaces as leg stretching (more visible at larger visualScale).
            if (debugForceIdentityScale ||
                (std::abs(scale.x - 1.0f) < 0.005f &&
                 std::abs(scale.y - 1.0f) < 0.005f &&
                 std::abs(scale.z - 1.0f) < 0.005f))
                scale = glm::vec3(1.0f);
        }
    }

    int boneIndex = node.boneIndex;

    if (boneIndex != -1)
    {
        if (boneIndex >= static_cast<int>(outTRS.size())) {
            outTRS.resize(boneIndex + 1);
        }

        // NOTE: The pelvis drop (currentPelvisDropY) is intentionally NOT
        // applied here. EvaluateNodeTRS feeds into multi-layer TRS blending,
        // and the authoritative application of the Hips local-Y offset lives
        // in the composeHierarchy lambda inside evaluateBoneMatrices (it
        // subtracts currentPelvisDropY / ikWorldScale on the already-blended
        // Hips TRS). Doing it here AND there was a double-dip: the Hips were
        // pushed down twice as far as intended every frame, which made
        // SolveLegIK see an over-extended/failed reach, fire the Non-linear
        // Posture Spring Relaxer, break the foot lock, and reset on the next
        // frame — the high-velocity feedback jitter on idle/walk (see todo file).
        outTRS[boneIndex] = {pos, rot, scale};
    }

    // ---- Compute global for children ----
    glm::mat4 localTransform =
        glm::translate(glm::mat4(1.0f), pos) *
        glm::mat4_cast(rot) *
        glm::scale(glm::mat4(1.0f), scale);

    glm::mat4 globalTransform = parentGlobal * localTransform;

    for (const auto &child : node.children)
    {
        EvaluateNodeTRS(child, globalTransform, anim, time, outTRS);
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
    ikOffsets[bone] = offset * weight;
}

void Animator::AddRootRotationOffset(const glm::quat& rot) {
    rootQuatOffset = rot;
}

// ------------------------------------------------------------------
// Structural Motion Graph: Transition clips replace runtime pose blends.
// The ApplyStaturePoseBlend / ClearStaturePoseBlend methods have been
// REMOVED — all-joint slerp + 2D alignment is now pre-computed in
// TransitionClipGenerator at load time.
// ------------------------------------------------------------------

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

const Animator::AnimationLayer* Animator::GetActiveLayer(int index) const {
    if (index < 0 || index >= static_cast<int>(activeAnimations.size())) return nullptr;
    return &activeAnimations[index];
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

    // CRITICAL FIX: Don't clear layers - preserve animation time!
    // Find or create layer for anim1
    bool foundAnim1 = false;
    for (auto& layer : activeAnimations) {
        if (layer.animation == anim1) {
            // Smoothly interpolate weight to prevent popping
            layer.targetWeight = weight1;
            if (std::abs(layer.weight - weight1) > 0.01f) {
                layer.weight = glm::mix(layer.weight, weight1, 10.0f * dt);
            } else {
                layer.weight = weight1;
            }
            layer.enabled = true;
            foundAnim1 = true;
            break;
        }
    }
    if (!foundAnim1 && anim1) {
        activeAnimations.emplace_back(anim1, weight1, 0.0f);
        activeAnimations.back().targetWeight = weight1;
        activeAnimations.back().blendProgress = 1.0f;
    }
    
    // Find or create layer for anim2
    bool foundAnim2 = false;
    for (auto& layer : activeAnimations) {
        if (layer.animation == anim2) {
            // Smoothly interpolate weight to prevent popping
            layer.targetWeight = weight2;
            if (std::abs(layer.weight - weight2) > 0.01f) {
                layer.weight = glm::mix(layer.weight, weight2, 10.0f * dt);
            } else {
                layer.weight = weight2;
            }
            layer.enabled = true;
            foundAnim2 = true;
            break;
        }
    }
    if (!foundAnim2 && anim2) {
        activeAnimations.emplace_back(anim2, weight2, 0.0f);
        activeAnimations.back().targetWeight = weight2;
        activeAnimations.back().blendProgress = 1.0f;
    }
    
    // Disable any other layers that shouldn't be active (but don't remove them)
    for (auto& layer : activeAnimations) {
        if (layer.animation != anim1 && layer.animation != anim2) {
            layer.targetWeight = 0.0f;
            layer.enabled = false;
        }
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

        // layer.time is in seconds, dt is in seconds
        layer.time += dt * speed;

        // If we crossed the loop point this frame, mark for cross-fade
        // DISABLED for motion matching - it handles looping manually
        // float prevTime = layer.time;
        // if (prevTime < animDuration && layer.time >= animDuration) {
        //     layersNeedingCrossFade.push_back(i);
        //     layer.time = fmod(layer.time, animDuration);
        // }

        // Simple looping without cross-fade
        if (layer.time >= animDuration) {
            layer.time = fmod(layer.time, animDuration);
        }
    }

    // DISABLED: Create cross-fade layers for animations that looped
    // Motion matching handles smooth transitions, we don't need cross-fade on loop
    /*
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
    */

    // Second pass: update blend weights
    for (auto& layer : activeAnimations)
    {
        if (!layer.animation || !layer.enabled) continue;

        // Update blend progress and interpolate weight.
        // The ramp must also run for layers that already finished their fade-IN
        // (blendProgress == 1.0) but now have a new targetWeight to fade OUT to
        // (a newer clip was blended over them) - otherwise old clips wedge at
        // full weight forever.
        if (layer.blendDuration > 0.0f)
        {
            if (layer.blendProgress < 1.0f)
            {
                layer.blendProgress = glm::min(1.0f, layer.blendProgress + (dt / layer.blendDuration));
            }

            float blendSpeed = 1.0f / layer.blendDuration;
            float rampT = glm::clamp(blendSpeed * dt, 0.0f, 1.0f);
            layer.weight = glm::mix(layer.weight, layer.targetWeight, rampT);

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

    // Cleanup: Remove layers that have faded out
    // CRITICAL FIX: Be VERY conservative - only remove layers that have been 
    // fully faded for multiple frames to prevent animation popping
    static std::map<Animation*, int> s_fadeOutCounter;  // Track frames at zero weight
    
    if (activeAnimations.size() > 1) {
        size_t before = activeAnimations.size();
        
        // First pass: mark layers for removal
        std::vector<size_t> layersToRemove;
        for (size_t i = 0; i < activeAnimations.size(); i++) {
            auto& layer = activeAnimations[i];
            
            // Only consider removal if:
            // 1. Weight is essentially zero
            // 2. Target weight is zero
            // 3. Blend is complete
            // 4. Has been at zero for multiple frames (prevents popping)
            if (layer.weight < 0.001f 
                && layer.targetWeight < 0.001f
                && layer.blendProgress >= 1.0f)
            {
                // Track how long this layer has been faded
                s_fadeOutCounter[layer.animation]++;
                
                // Only remove after 10 frames (~0.16s) at zero weight
                if (s_fadeOutCounter[layer.animation] > 10) {
                    layersToRemove.push_back(i);
                    s_fadeOutCounter.erase(layer.animation);
                }
            } else {
                // Reset counter if layer becomes active again
                s_fadeOutCounter.erase(layer.animation);
            }
        }
        
        // Remove marked layers (in reverse order to preserve indices)
        for (auto it = layersToRemove.rbegin(); it != layersToRemove.rend(); ++it) {
            activeAnimations.erase(activeAnimations.begin() + *it);
        }

        if (activeAnimations.size() != before && activeAnimations.size() > 0) {
            std::cout << "[Cleanup] Removed " << (before - activeAnimations.size()) << " faded layers\n";
        }
    }

    // CRITICAL FIX: Disable automatic layer reordering
    // Reordering causes animation time resets and popping
    // The blend weights should determine which animation is dominant, not layer order
    /*
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
    */

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
    // Invalidate the cached leg-reach lengths (L1+L2). After a retarget or a
    // full cache reset these must recompute against the live skeleton, or the
    // two-bone IK solver reuses stale pre-retarget limb lengths and the 92-97%
    // posture-spring relaxer mis-judges reach -> leg stretch.
    dynamicLeftLegLength = -1.0f;
    dynamicRightLegLength = -1.0f;
    // Stature offset fields removed — structural transition clips replace
    // runtime stature inertialization. No stale pelvis-elevation to clear.
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

void Animator::SetTerrainHeightFn(const std::function<float(float,float)>& fn)
{
    m_terrainFn = fn;
}

void Animator::SetFootBones(int leftFoot, int rightFoot, int leftToe, int rightToe,
                          int leftUpLeg, int rightUpLeg, int leftLeg, int rightLeg)
{
    footIKSettings.leftFootBone = leftFoot;
    footIKSettings.rightFootBone = rightFoot;
    footIKSettings.leftToeBone = leftToe;
    footIKSettings.rightToeBone = rightToe;
    footIKSettings.leftUpLegBone = leftUpLeg;
    footIKSettings.rightUpLegBone = rightUpLeg;
    footIKSettings.leftLegBone = leftLeg;
    footIKSettings.rightLegBone = rightLeg;
}

void Animator::SetIKWorldScale(float scale)
{
    ikWorldScale = scale;
}

// ============================================================
// PELVIS HEIGHT ADJUSTMENT (knee-bend stretch fix)
// ============================================================
// Problem: when a planted foot target sits below the animated ankle
// (walking down a step, squatting, etc.), the two-bone IK solver
// clamps the reach to L1+L2 and shoves the residual into ankleOffset.
// This visibly STRETCHES the leg (ankle dragged past the knee) and
// the knee-bend looks unnatural / lagged.
//
// Solution: measure how far each leg is from fully reaching its target,
// then drop the Hips bone (propagates down both leg chains) by the
// deficit so the knee bend carries the foot down instead of the ankle
// offset stretching the leg.

void Animator::CacheLegLengths()
{
    // Lazily computed — only once (guarded by > 0.0f check).
    // Re-evaluates if the cache is invalidated (e.g. bone reindexing).
    if (dynamicLeftLegLength > 0.0f && dynamicRightLegLength > 0.0f) return;

    const auto& bp = currBoneWorldPos;
    // currBoneWorldPos is in MODEL space (pre-model-matrix). The model
    // matrix carries an import scale (ikWorldScale ≈ 0.003) that converts
    // model units to world metres. Multiply by ikWorldScale so the cached
    // reach radii match the world-space distances computed in
    // CalculatePelvisAdjustment (which transforms through modelMatrix).
    const float scaleFactor = (ikWorldScale > 0.0f) ? ikWorldScale : 1.0f;
    int lt = footIKSettings.leftUpLegBone;
    int ls = footIKSettings.leftLegBone;
    int lf = footIKSettings.leftFootBone;
    if (lt >= 0 && ls >= 0 && lf >= 0 &&
        (size_t)lt < bp.size() && (size_t)ls < bp.size() && (size_t)lf < bp.size()) {
        float L1 = glm::length(bp[ls] - bp[lt]) * scaleFactor;
        float L2 = glm::length(bp[lf] - bp[ls]) * scaleFactor;
        dynamicLeftLegLength = (L1 > 1e-4f && L2 > 1e-4f) ? (L1 + L2) : 0.85f;
    } else {
        dynamicLeftLegLength = 0.85f;
    }

    int rt = footIKSettings.rightUpLegBone;
    int rs = footIKSettings.rightLegBone;
    int rf = footIKSettings.rightFootBone;
    if (rt >= 0 && rs >= 0 && rf >= 0 &&
        (size_t)rt < bp.size() && (size_t)rs < bp.size() && (size_t)rf < bp.size()) {
        float L1 = glm::length(bp[rs] - bp[rt]) * scaleFactor;
        float L2 = glm::length(bp[rf] - bp[rs]) * scaleFactor;
        dynamicRightLegLength = (L1 > 1e-4f && L2 > 1e-4f) ? (L1 + L2) : 0.85f;
    } else {
        dynamicRightLegLength = 0.85f;
    }

    if (dynamicLeftLegLength > 0.0f) {
        std::cout << "[PelvisIK] Left leg reach  (L1+L2) = " << dynamicLeftLegLength << "m\n";
    }
    if (dynamicRightLegLength > 0.0f) {
        std::cout << "[PelvisIK] Right leg reach (L1+L2) = " << dynamicRightLegLength << "m\n";
    }
}

// Exposed leg-reach metrics for the MotionMatcher pelvis-damping pass (Fix 2
// from updated todo).
float Animator::GetLeftLegMaxReach() const {
    if (dynamicLeftLegLength <= 0.0f)
        const_cast<Animator*>(this)->CacheLegLengths();
    return dynamicLeftLegLength > 0.0f ? dynamicLeftLegLength : 0.85f;
}
float Animator::GetRightLegMaxReach() const {
    if (dynamicRightLegLength <= 0.0f)
        const_cast<Animator*>(this)->CacheLegLengths();
    return dynamicRightLegLength > 0.0f ? dynamicRightLegLength : 0.85f;
}

void Animator::CalculatePelvisAdjustment(float dt, const glm::mat4& modelMatrix)
{
    // Snapshot the pelvis drop that was active during THIS frame's first
    // skeleton pass (evaluateBoneMatrices in Update). SolveLegIK runs against
    // the first-pass bone positions, so it needs the delta between that pass
    // and the final revalidation pass to stay in sync — preventing the 1-frame
    // leg stretch/jitter when the pelvis drops mid-frame. Must happen before
    // the early-out below mutates currentPelvisDropY.
    prevPelvisDropY = currentPelvisDropY;

    if (!footIKSettings.enabled || !skeleton || dt <= 0.0f) {
        // No IK active — smoothly release any residual pelvis drop back to 0.
        currentPelvisDropY = glm::mix(currentPelvisDropY, 0.0f, 10.0f * dt);
        return;
    }

    CacheLegLengths();

    // Leave a 5 % safety cushion so the analytical solver never approaches its
    // 175-degree hyper-extension limit — that keeps knees permanently
    // micro-bent (anatomically correct) and eliminates the "locked-straight"
    // look on flat ground.
    float leftMaxReach  = dynamicLeftLegLength  * 0.95f;
    float rightMaxReach = dynamicRightLegLength * 0.95f;

    float leftDeficit  = 0.0f;
    float rightDeficit = 0.0f;

    // --- Left leg ---
    int lt = footIKSettings.leftUpLegBone;
    if (lt >= 0 && leftFootIK.isLocked &&
        (size_t)lt < currBoneWorldPos.size()) {
        glm::vec3 hipWorld = glm::vec3(modelMatrix * glm::vec4(currBoneWorldPos[lt], 1.0f));
        float currentDist = glm::distance(hipWorld, leftFootIK.targetPosition);
        if (currentDist > leftMaxReach) {
            leftDeficit = currentDist - leftMaxReach;
        }
    }

    // --- Right leg ---
    int rt = footIKSettings.rightUpLegBone;
    if (rt >= 0 && rightFootIK.isLocked &&
        (size_t)rt < currBoneWorldPos.size()) {
        glm::vec3 hipWorld = glm::vec3(modelMatrix * glm::vec4(currBoneWorldPos[rt], 1.0f));
        float currentDist = glm::distance(hipWorld, rightFootIK.targetPosition);
        if (currentDist > rightMaxReach) {
            rightDeficit = currentDist - rightMaxReach;
        }
    }

    // Drop the pelvis by whichever leg is straining the most — the Hips bone
    // translates downward in model space and propagates to both thighs/shins.
    float targetDrop = glm::max(leftDeficit, rightDeficit);

    // Asymmetric smoothing via critically-damped spring envelope:
    //   - Drops fast (25 s⁻¹) to keep feet planted on sudden terrain dips.
    //   - Rises slowly (10 s⁻¹) to avoid the stomp/jerk on recovery.
    // Using 1 - exp(-rate * dt) instead of glm::mix(..., rate*dt) gives
    // frame-rate-independent, non-linear decay and never overshoots past
    // the target.
    //
    // Leg Extension Damping Protection: when the deficit spikes (e.g.
    // stepping off a curb), a pure positional spring lags 2–3 frames before
    // the pelvis reaches the target — during that gap the knee hyper-extends
    // and the leg stretches. To prevent this, we measure the *velocity* of
    // the target change and inject extra drop proportional to the spike,
    // giving the two-bone solver room to bend the knee before it clamps.
    float targetDelta = targetDrop - currentPelvisDropY;
    float dropRate    = (targetDelta > 0.0f) ? 30.0f : 10.0f;  // 30 on descent (crouch/step-down), 10 on recovery

    // Preemptively over-drop by a fraction of the deficit spike when the
    // change is sudden (more than 2 cm in a single frame). This buys the
    // knee-bend solver the 2-3 frames of lead time it was missing.
    float spikeThreshold = 0.02f;
    float legExtensionGuard = 0.0f;
    if (targetDelta > spikeThreshold) {
        // Scale the guard: larger spikes get proportionally more lead drop
        // (capped at 40% of the deficit). The pelvis spring will correct
        // back smoothly — this is a one-shot predictive nudge.
        legExtensionGuard = 0.4f * targetDelta;
    }

    float effectiveTarget = targetDrop + legExtensionGuard;

    // Critically-damped exponential decay (frame-rate independent)
    float decayFactor = 1.0f - std::exp(-dropRate * dt);
    currentPelvisDropY = currentPelvisDropY + (effectiveTarget - currentPelvisDropY) * decayFactor;
    // Track the rate of change for external consumers (SolveLegIK uses the
    // delta to sync its mid-frame revalidation pass).
    pelvisDropVelocity = (targetDelta > spikeThreshold)
        ? (currentPelvisDropY - prevPelvisDropY) / std::max(dt, 1e-5f)
        : 0.0f;

    // Cap the crouch so the pelvis never sinks into the floor.
    float absoluteMaxCrouch = glm::min(dynamicLeftLegLength, dynamicRightLegLength) * 0.4f;
    currentPelvisDropY = glm::clamp(currentPelvisDropY, 0.0f, absoluteMaxCrouch);
}

void Animator::UpdateFootIK(float dt, const glm::mat4& modelMatrix, bool isMoving)
{
    // Reset root rotation offset each frame — it's set by the quintic
    // inertialization pass and should not persist across frames where no
    // transition is active.
    rootQuatOffset = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

    // Save the previous frame's leg ikRotations BEFORE resetting, so SolveLegIK
    // can compute true "no-IK" bone matrices by undoing them. If we reset first,
    // gT_noIK = globalBoneMatrices * inverse(identity) = the OLD-IK matrix,
    // which bakes stale leg twist into the base the two-bone solver rotates.
    glm::mat4 oldLeftThighIk(1.0f), oldRightThighIk(1.0f);
    glm::mat4 oldLeftShinIk(1.0f), oldRightShinIk(1.0f);
    {
        const int lt = footIKSettings.leftUpLegBone;
        const int rt = footIKSettings.rightUpLegBone;
        const int ls = footIKSettings.leftLegBone;
        const int rs = footIKSettings.rightLegBone;
        if (lt >= 0 && lt < (int)ikRotations.size()) oldLeftThighIk  = ikRotations[lt];
        if (rt >= 0 && rt < (int)ikRotations.size()) oldRightThighIk = ikRotations[rt];
        if (ls >= 0 && ls < (int)ikRotations.size()) oldLeftShinIk   = ikRotations[ls];
        if (rs >= 0 && rs < (int)ikRotations.size()) oldRightShinIk  = ikRotations[rs];
    }

    // Reset two-bone knee-bend rotations every frame (SET semantics, like
    // ikOffsets) so a disabled/stale frame never leaves a stale leg twist.
    {
        const int legBones[] = {
            footIKSettings.leftUpLegBone, footIKSettings.rightUpLegBone,
            footIKSettings.leftLegBone,  footIKSettings.rightLegBone
        };
        for (int b : legBones) {
            if (b >= 0 && b < (int)ikRotations.size())
                ikRotations[b] = glm::mat4(1.0f);
        }
        // Clear per-foot ground tilt (recomputed below from this frame's pose so
        // a foot that's no longer planted goes flat instead of holding last slope).
        const int footBones[] = { footIKSettings.leftFootBone, footIKSettings.rightFootBone };
        for (int b : footBones) {
            if (b >= 0 && b < (int)ikFootTilt.size())
                ikFootTilt[b] = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        }
    }

    // Make the saved old IK transforms available to SolveLegIK.
    savedOldLeftThighIk  = oldLeftThighIk;
    savedOldRightThighIk = oldRightThighIk;
    savedOldLeftShinIk   = oldLeftShinIk;
    savedOldRightShinIk  = oldRightShinIk;

    if (!footIKSettings.enabled || !skeleton) return;

    int leftFoot = footIKSettings.leftFootBone;
    int rightFoot = footIKSettings.rightFootBone;

    if (leftFoot < 0 || rightFoot < 0) return;  // Foot bones not set

    // NaN protection: Check if bone positions are valid
    if (leftFoot >= (int)currBoneWorldPos.size() || rightFoot >= (int)currBoneWorldPos.size()) {
        static int warnCount = 0;
        if (++warnCount < 5) {
            std::cout << "[FootIK] WARNING: Bone index out of range! L=" << leftFoot 
                      << " R=" << rightFoot << " size=" << currBoneWorldPos.size() << "\n";
        }
        return;
    }

    // Check for NaN in bone positions
    glm::vec3& leftFootPos = currBoneWorldPos[leftFoot];
    glm::vec3& rightFootPos = currBoneWorldPos[rightFoot];
    
    bool leftValid = std::isfinite(leftFootPos.x) && std::isfinite(leftFootPos.y) && std::isfinite(leftFootPos.z);
    bool rightValid = std::isfinite(rightFootPos.x) && std::isfinite(rightFootPos.y) && std::isfinite(rightFootPos.z);
    
    if (!leftValid || !rightValid) {
        static int warnCount = 0;
        if (++warnCount < 5) {
            std::cout << "[FootIK] WARNING: NaN in bone positions! L=(" 
                      << leftFootPos.x << "," << leftFootPos.y << "," << leftFootPos.z << ") R=("
                      << rightFootPos.x << "," << rightFootPos.y << "," << rightFootPos.z << ")\n";
            // Debug: Check a few more bones to see pattern
            if (currBoneWorldPos.size() > 0) {
                glm::vec3& rootPos = currBoneWorldPos[0];
                std::cout << "[FootIK]   Root bone[0]: (" << rootPos.x << "," << rootPos.y << "," << rootPos.z << ")\n";
            }
            if (currBoneWorldPos.size() > 55) {
                glm::vec3& hipPos = currBoneWorldPos[55];
                std::cout << "[FootIK]   Bone[55] (rightupleg): (" << hipPos.x << "," << hipPos.y << "," << hipPos.z << ")\n";
            }
        }
        // Clear IK offsets to prevent NaN propagation
        if (leftFoot < (int)ikOffsets.size()) ikOffsets[leftFoot] = glm::vec3(0.0f);
        if (rightFoot < (int)ikOffsets.size()) ikOffsets[rightFoot] = glm::vec3(0.0f);
        return;
    }

    float floorY = footIKSettings.floorHeight;

    // DEBUG: Print foot IK status every 30 frames
    static int debugFrame = 0;
    bool printDebug = (++debugFrame % 30 == 0);

    if (printDebug) {
        std::cout << "[FootIK] floorY=" << floorY << " charPos.y≈" << (currBoneWorldPos.size() > 0 ? currBoneWorldPos[0].y : -1) << "\n";
        std::cout << "[FootIK] modelMatrix translation: (" << modelMatrix[3].x << "," << modelMatrix[3].y << "," << modelMatrix[3].z << ")\n";
        std::cout << "[FootIK] foot bone[" << leftFoot << "] local: (" 
                  << (currBoneWorldPos.size() > (size_t)leftFoot ? currBoneWorldPos[leftFoot].x : -999) << ","
                  << (currBoneWorldPos.size() > (size_t)leftFoot ? currBoneWorldPos[leftFoot].y : -999) << ","
                  << (currBoneWorldPos.size() > (size_t)leftFoot ? currBoneWorldPos[leftFoot].z : -999) << ")\n";
    }

    // CRITICAL FIX: Foot IK must work DURING movement!
    // isMoving only affects how quickly we release locked feet
    float releaseSpeedMult = isMoving ? 2.0f : 1.0f;  // Faster release when moving

    // Update left foot IK
    if (leftFoot >= 0 && leftFoot < (int)currBoneWorldPos.size()) {
        glm::vec3 footLocalPos = currBoneWorldPos[leftFoot];
        glm::vec3 prevFootLocalPos = currBoneWorldPos[leftFoot];
        
        // Debug: Check for NaN before transform
        bool footValid = std::isfinite(footLocalPos.x) && std::isfinite(footLocalPos.y) && std::isfinite(footLocalPos.z);
        bool modelValid = std::isfinite(modelMatrix[3].x) && std::isfinite(modelMatrix[3].y) && std::isfinite(modelMatrix[3].z);
        
        if (!footValid) {
            static int warnCount = 0;
            if (++warnCount < 3) {
                std::cout << "[FootIK] ERROR: footLocalPos is NaN! bone=" << leftFoot 
                          << " pos=(" << footLocalPos.x << "," << footLocalPos.y << "," << footLocalPos.z << ")\n";
            }
            footLocalPos = glm::vec3(0.0f);  // Prevent NaN propagation
        }
        
        glm::vec3 footWorldPos = modelMatrix * glm::vec4(footLocalPos, 1.0f);
        glm::vec3 prevFootPos = modelMatrix * glm::vec4(prevBoneWorldPos[leftFoot], 1.0f);

        // Check if foot is near floor
        // How close the foot must be to the floor to even be *considered* for a
        // lock. Was 0.8 ("catch the plant phase"), but a jog/crouch SWING foot
        // also sits 0.3-0.8m above the ground and is slow (footSpeed < 0.08) —
        // so the wide band + low speed test false-locked swing feet mid-swing,
        // sticking them under a body that was already moving on (legs "trail")
        // and toggling on idle noise (jitter). 0.15 means only a foot that is
        // actually contacting/almost-contacting the ground is eligible; the
        // residual descent (up to maxIKDistance=0.2) is closed by the knee +
        // ankle offset, so contact is caught cleanly.
        float distToFloor = footWorldPos.y - floorY;
        bool nearFloor = distToFloor < 0.15f && distToFloor > -0.1f;

        // Check if foot is moving slowly (planted)
        float footSpeed = glm::length(footWorldPos - prevFootPos);
        float plantThreshold = isMoving ? 0.08f : 0.05f;  // Higher threshold when moving
        bool isStationary = footSpeed < plantThreshold;

        // Only plant when the foot is NOT rising into its swing arc. A foot at
        // the apex of a mid-leg-swing sits low + stationary momentarily, which
        // the old nearFloor/isStationary test mistook for a contact - the IK
        // then yanked the ankle down to floorY, stretching the leg and
        // twitching the foot (plant → stretch → release → relatch each step).
        // Requiring the foot to be descending or settled (not rising) cuts the
        // false mid-swing lock that produced the stretching/twitch.
        float footVy = footWorldPos.y - prevFootPos.y;
        bool notRising = footVy <= 0.05f;

        // CRITICAL FIX: Check for animation loop discontinuity
        if (leftFootIK.isLocked) {
            float distFromLock = glm::length(footWorldPos - leftFootIK.lockedPosition);
            if (distFromLock > 0.3f) {  // Foot moved too far - likely animation loop
                leftFootIK.isLocked = false;
                leftFootIK.lockWeight = 0.0f;
                leftFootIK.ankleOffset = glm::vec3(0.0f);
                leftFootIK.contactConfirm = 0;
                if (printDebug) std::cout << "[FootIK] LEFT: Force release (discontinuity " << distFromLock << "m)\n";
            }
        }

        // Lock hysteresis: a foot must be SUSTAINED grounded for
        // footLockConfirmFrames before locking, so a slow swing that merely
        // grazes the floor (or idle animation noise) can't false-lock / toggle.
        if (!leftFootIK.isLocked) {
            if (nearFloor && isStationary && notRising) {
                leftFootIK.contactConfirm++;
            } else {
                leftFootIK.contactConfirm = 0;
            }
        }

        // Lock foot when it's near floor, stationary and not rising into a
        // swing (works during walking!). The ankleOffset clamp in SolveLegIK
        // bounds any residual reach so a contact that can't quite be met by
        // the knee bend no longer stretches the leg.
        if (nearFloor && isStationary && notRising && !leftFootIK.isLocked &&
            leftFootIK.contactConfirm >= footIKSettings.footLockConfirmFrames) {
            leftFootIK.isLocked = true;
            leftFootIK.contactConfirm = 0;
            leftFootIK.lockedPosition = footWorldPos;  // anim foot pos - for release / loop detection
            leftFootIK.targetPosition = footWorldPos;
            leftFootIK.targetPosition.y = floorY;        // plant the foot ON the terrain surface
            if (printDebug) std::cout << "[FootIK] LEFT: LOCKED @ y=" << footWorldPos.y << " (speed=" << footSpeed << ")\n";
        }

        // On a force-release (loop discontinuity) also reset the confirmation
        // counter so the foot re-locks cleanly only after re-settling.

        // Update lock weight
        if (leftFootIK.isLocked) {
            leftFootIK.lockWeight = glm::min(1.0f, leftFootIK.lockWeight + dt * footIKSettings.footLockBlend);
            leftFootIK.timeSinceLock += dt;

            // Auto-release after a max lock duration only as a true safety net
            // for a stuck foot. The old 1.0s cap still fired mid-stance on slow
            // crouch/idle walks (a full stance can exceed 1s), forcing a
            // release -> 5-frame re-lock cycle -> jitter. 3.0s covers even a
            // very slow crouched stance while still clearing a genuinely stuck
            // foot; normal release is handled by the lift / discontinuity paths.
            if (leftFootIK.timeSinceLock > 3.0f && isMoving) {
                leftFootIK.isLocked = false;
                leftFootIK.timeSinceLock = 0.0f;
                if (printDebug) std::cout << "[FootIK] LEFT: Timeout release\n";
            }

            // Release lock when foot moves up significantly from LOCKED position
            float releaseThreshold = isMoving ? 0.12f : 0.08f;
            float distFromLock = footWorldPos.y - leftFootIK.lockedPosition.y;
            if (distFromLock > releaseThreshold || (isMoving && footSpeed > 0.25f)) {
                leftFootIK.isLocked = false;
                if (printDebug) std::cout << "[FootIK] LEFT: Lift release (dy=" << distFromLock << " speed=" << footSpeed << ")\n";
            }

            // Over-stride / body-commit release: at SLOW motion the lazy lift/
            // drift releases (0.08m rise / 0.3m drift, speed>0.25) rarely fire,
            // so a planted foot holds while the body strides past the leg's max
            // reach -> "body slides off the feet". Release on a TRUE over-reach
            // instead: the hip has moved past the planted foot farther than
            // knee-straight + half the ankle-offset budget can bridge, measured on
            // the final hip (this frame's pelvis crouch applied) so a body merely
            // leaning over the foot (dist ~0.95..L) never trips it. Gated on
            // isMoving so an idle pose (hip ~at leg length) stays locked — without
            // the gate the threshold unlocked standing feet onto the raw animation.
            {
                const int ltOs = footIKSettings.leftUpLegBone;
                if (ltOs >= 0 && ltOs < (int)currBoneWorldPos.size() && dynamicLeftLegLength > 0.0f) {
                    glm::vec3 hipW = glm::vec3(modelMatrix * glm::vec4(currBoneWorldPos[ltOs], 1.0f));
                    float pelvisDeltaY = currentPelvisDropY - prevPelvisDropY;
                    hipW.y -= pelvisDeltaY;  // final-frame hip (this frame's crouch applied)
                    float hipToFoot = glm::length(hipW - leftFootIK.targetPosition);
                    float maxBridge = dynamicLeftLegLength + 0.5f * footIKSettings.maxIKDistance;
                    // Velocity-robust release: at high horizontal speeds the hip
                    // legitimately swings past the planted foot on sharp turns —
                    // a distance-only threshold would falsely release in that case
                    // (foot pops mid-stride → relock jitter). Scale the bridge
                    // threshold up proportional to body speed so only a GENUINE
                    // over-reach (hip beyond what body momentum can explain)
                    // triggers a release. The 0.5m cap keeps sprint over-strides
                    // (where the whole leg budget is used) still caught.
                    float horizSpeed = glm::length(glm::vec2(characterVelocity.x, characterVelocity.z));
                    float velocityBonus = glm::min(maxBridge * 0.5f, horizSpeed * 0.3f);
                    float dynamicBridge = maxBridge + velocityBonus;
                    // Fire ONCE per over-reach (armed flag), so a sustained lean past
                    // the foot on a sharp turn doesn't churn lock->release->relock.
                    if (isMoving && hipToFoot > dynamicBridge && leftFootIK.overStrideArmed) {
                        leftFootIK.isLocked = false;
                        leftFootIK.lockWeight = 0.0f;
                        leftFootIK.ankleOffset = glm::vec3(0.0f);
                        leftFootIK.ankleOffsetSmoothed = glm::vec3(0.0f);
                        leftFootIK.contactConfirm = 0;
                        leftFootIK.overStrideArmed = false;  // re-armed when hip returns within reach
                        if (printDebug) std::cout << "[FootIK] LEFT: Over-stride release (hip->foot=" << hipToFoot << " > bridge=" << dynamicBridge << " velBonus=" << velocityBonus << ")\n";
                    } else if (hipToFoot < dynamicLeftLegLength) {
                        leftFootIK.overStrideArmed = true;
                    }
                }
            }
        } else {
            leftFootIK.lockWeight = glm::max(0.0f, leftFootIK.lockWeight - dt * footIKSettings.footLockReleaseSpeed * releaseSpeedMult);
            leftFootIK.timeSinceLock = 0.0f;
        }

        // Calculate IK offset + two-bone knee bend.
        float ikWeight = leftFootIK.lockWeight * footIKSettings.ikStrength;
        const int lt = footIKSettings.leftUpLegBone;  // thigh
        const int ls = footIKSettings.leftLegBone;    // shin
        glm::vec3 leftAnkleWE(0.0f), leftKneeWE(0.0f);
        if (ikWeight > 0.001f) {
            bool bent = false;
            if (lt >= 0 && ls >= 0 && footIKSettings.kneeBendWeight > 0.0f) {
                glm::mat4 thighRot, shinRot;
                glm::vec3 ankleOff;
                if (DebugSolveLegIK(lt, ls, leftFoot, modelMatrix,
                               leftFootIK.targetPosition, thighRot, shinRot, ankleOff,
                               leftAnkleWE, leftKneeWE)) {
                    float bendW = ikWeight * footIKSettings.kneeBendWeight;
                    glm::quat qt = glm::slerp(glm::quat(1.0f,0,0,0), glm::quat_cast(thighRot), bendW);
                    glm::quat qs = glm::slerp(glm::quat(1.0f,0,0,0), glm::quat_cast(shinRot), bendW);
                    if (lt < (int)ikRotations.size()) ikRotations[lt] = glm::mat4_cast(qt);
                    if (ls < (int)ikRotations.size()) ikRotations[ls] = glm::mat4_cast(qs);
                    // residual: exact floor contact after the knee bend (~0 when reachable)
                    leftFootIK.ankleOffset = ankleOff * ikWeight;
                    bent = true;
                }
            }
            if (!bent) {
                // Fallback (no leg bones / solve failed): pure ankle translate -
                // the verified foot-planting behaviour, applied as a MODEL-SPACE
                // translate (ankleOffset) with the world->model conversion so the
                // foot actually reaches the floor instead of moving 1/ikWorldScale.
                glm::vec3 targetPos = leftFootIK.targetPosition;
                glm::vec3 currentPos = footWorldPos;
                glm::vec3 offset = targetPos - currentPos;
                if (glm::length(offset) > footIKSettings.maxIKDistance) {
                    offset = glm::normalize(offset) * footIKSettings.maxIKDistance;
                }
                glm::vec3 modelOffset = (ikWorldScale > 0.0f) ? offset / ikWorldScale : offset;
                leftFootIK.ankleOffset = modelOffset * ikWeight;
            }
        } else {
            leftFootIK.ankleOffset = glm::vec3(0.0f);
        }

        // Plant the ankle INSTANTLY when the leg actually reaches the floor
        // (the residual ankleOffset is small — a legit contact), so the foot
        // drops straight to the ground with NO float. Only the LARGE
        // over-extension / transition spike (ankleOffset beyond half the IK
        // budget — a stale foot target on a motion-match switch, or the hip
        // yanked away from a planted foot) is smoothed, which kills the
        // single-frame stretch pull without ever letting the foot lag the
        // ground after a normal plant. (Snapping the small offset is what made
        // the foot read as "mis-positioned on the leg" under the old uniform
        // 3.0 m/s rate-limit on every plant.)
        {
            float ankleMagModel = glm::length(leftFootIK.ankleOffset);
            // ankleMagModel is in MODEL space (cm), but maxIKDistance is a
            // WORLD-m budget (0.2 m). Convert via ikWorldScale (cm->m) so the
            // gate actually fires on a normal plant: ankle <= ~half the IK
            // budget snaps instantly (no 3 m/s lag on quick turns), while a
            // genuine over-extension / transition spike (>half budget) still
            // smooths. (Previously the raw 0.1 vs cm-magnitude almost never
            // snapped -> the ankle was always rate-limited, lagging ~2 frames
            // on sharp turns and micro-jittering the planted foot.)
            float snapBudgetModel = (ikWorldScale > 0.0f)
                ? (0.5f * footIKSettings.maxIKDistance / ikWorldScale)
                : 0.5f * footIKSettings.maxIKDistance;
            if (ankleMagModel <= snapBudgetModel) {
                // Leg reached the target (foot flat on the ground): snap, no lag.
                leftFootIK.ankleOffsetSmoothed = leftFootIK.ankleOffset;
            } else {
                // Over-extension / transition spike: soften the instant jump.
                float maxStepWorld = footIKSettings.ankleOffsetMaxSpeed * dt;
                float maxStepModel = (ikWorldScale > 0.0f) ? (maxStepWorld / ikWorldScale) : maxStepWorld;
                glm::vec3 delta = leftFootIK.ankleOffset - leftFootIK.ankleOffsetSmoothed;
                float dl = glm::length(delta);
                if (dl > maxStepModel && maxStepModel > 0.0f) delta *= (maxStepModel / dl);
                leftFootIK.ankleOffsetSmoothed += delta;
            }
            leftFootIK.ankleOffset = leftFootIK.ankleOffsetSmoothed;
        }

        // Ground-normal foot tilt (todo Part 3, Option A): sample the terrain
        // heightfield around the planted foot, derive the slope normal, and
        // blend the align-quat in with the plant weight so planting / lifting /
        // slope changes stay smooth. Flat terrain -> identity -> no-op at eval.
        if (m_terrainFn && leftFoot >= 0 && leftFoot < (int)ikFootTilt.size()) {
            // STABILIZE Ground-Normal Flutter (Fix 1): pass the raw terrain raycast
            // normal through an adaptive Kalman filter before building the foot
            // tilt quat. At idle the central-difference sampler flicker 0.2° frame
            // to frame on triangle edges; the filter's covariance envelope rejects
            // this while still tracking genuine slope changes under locomotion.
            glm::vec3 rawNormal = SampleTerrainNormal(m_terrainFn, footWorldPos);
            glm::vec3 filteredNormal = FilterGroundNormal(rawNormal, /*isLeft=*/true, dt);
            glm::quat targetTilt = ComputeFootTiltQuat(filteredNormal);
            glm::quat wanted = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), targetTilt, ikWeight);
            float t = glm::clamp(dt * 20.0f, 0.05f, 1.0f);
            ikFootTilt[leftFoot] = glm::slerp(ikFootTilt[leftFoot], wanted, t);
        }

        if (printDebug) {
            // Actual final ankle world Y = (ankle after knee rotation) + ankle
            // translate. This equals floorY by construction when the two-bone
            // solve runs; the old "ankle translate only" formula hid the knee
            // rotation contribution and read high while the leg was still
            // straightening on the over-reach (leg shorter than hip->floor).
            glm::vec3 ankleOffWorld = glm::mat3(modelMatrix) * leftFootIK.ankleOffset;
            float plantedY = leftAnkleWE.y + ankleOffWorld.y;
            float kneeDeg = -1.0f;
            if (lt >= 0 && lt < (int)currBoneWorldPos.size()) {
                glm::vec3 hipW = glm::vec3(modelMatrix * glm::vec4(currBoneWorldPos[lt], 1.0f));
                glm::vec3 thighD = hipW - leftKneeWE;
                glm::vec3 shinD  = leftAnkleWE - leftKneeWE;
                float Lth = glm::length(thighD), Lsh = glm::length(shinD);
                if (Lth > 1e-4f && Lsh > 1e-4f)
                    kneeDeg = glm::degrees(acosf(glm::clamp(glm::dot(thighD, shinD) / (Lth * Lsh), -1.0f, 1.0f)));
            }
            // Over-stride metric (for diagnostics): hip->planted-target distance
            // vs the leg's max bridge. Fires the velocity-robust release only
            // while isMoving and past maxBridge.
            float hipToFootL = -1.0f, maxBridgeL = -1.0f;
            if (lt >= 0 && lt < (int)currBoneWorldPos.size() && dynamicLeftLegLength > 0.0f) {
                glm::vec3 hW = glm::vec3(modelMatrix * glm::vec4(currBoneWorldPos[lt], 1.0f));
                hW.y -= (currentPelvisDropY - prevPelvisDropY);
                hipToFootL = glm::length(hW - leftFootIK.targetPosition);
                maxBridgeL = dynamicLeftLegLength + 0.5f * footIKSettings.maxIKDistance;
            }
            std::cout << "[FootIK Debug] LEFT: pos=(" << footWorldPos.x << "," << footWorldPos.y << "," << footWorldPos.z
                      << ") speed=" << footSpeed << " locked=" << (leftFootIK.isLocked ? "YES" : "NO")
                      << " weight=" << leftFootIK.lockWeight << " isMoving=" << isMoving
                      << " floorY=" << floorY << " plantedY=" << plantedY
                      << " ankleOffY_m=" << (leftFootIK.ankleOffset.y * ikWorldScale)
                      << " knee=" << kneeDeg
                      << " cc=" << leftFootIK.contactConfirm
                      << " hip2foot=" << hipToFootL << " reach=" << maxBridgeL
                      << " target=(" << leftFootIK.targetPosition.x << "," << leftFootIK.targetPosition.y << "," << leftFootIK.targetPosition.z
                      << ")\n";
        }
    }

    // Update right foot IK (same logic)
    if (rightFoot >= 0 && rightFoot < (int)currBoneWorldPos.size()) {
        glm::vec3 footWorldPos = modelMatrix * glm::vec4(currBoneWorldPos[rightFoot], 1.0f);
        glm::vec3 prevFootPos = modelMatrix * glm::vec4(prevBoneWorldPos[rightFoot], 1.0f);

        float distToFloor = footWorldPos.y - floorY;
        bool nearFloor = distToFloor < 0.15f && distToFloor > -0.1f;  // close-to-ground contact only (see left foot)

        float footSpeed = glm::length(footWorldPos - prevFootPos);
        float plantThreshold = isMoving ? 0.08f : 0.05f;
        bool isStationary = footSpeed < plantThreshold;

        // Don't lock a foot that is rising into its swing arc - the apex of a
        // mid-leg-swing sits low + stationary and was mistaken for a contact,
        // yanking the ankle to the floor (leg stretch + foot twitch).
        float footVy = footWorldPos.y - prevFootPos.y;
        bool notRising = footVy <= 0.05f;

        // CRITICAL FIX: Check for animation loop discontinuity
        if (rightFootIK.isLocked) {
            float distFromLock = glm::length(footWorldPos - rightFootIK.lockedPosition);
            if (distFromLock > 0.3f) {
                rightFootIK.isLocked = false;
                rightFootIK.lockWeight = 0.0f;
                rightFootIK.ankleOffset = glm::vec3(0.0f);
                rightFootIK.contactConfirm = 0;
                if (printDebug) std::cout << "[FootIK] RIGHT: Force release (discontinuity " << distFromLock << "m)\n";
            }
        }

        // Lock hysteresis (see left foot) — require sustained ground contact
        // before locking so slow swings don't false-lock and idle noise can't
        // toggle the lock.
        if (!rightFootIK.isLocked) {
            if (nearFloor && isStationary && notRising) {
                rightFootIK.contactConfirm++;
            } else {
                rightFootIK.contactConfirm = 0;
            }
        }

        if (nearFloor && isStationary && notRising && !rightFootIK.isLocked &&
            rightFootIK.contactConfirm >= footIKSettings.footLockConfirmFrames) {
            rightFootIK.isLocked = true;
            rightFootIK.contactConfirm = 0;
            rightFootIK.lockedPosition = footWorldPos;  // anim foot pos - for release / loop detection
            rightFootIK.targetPosition = footWorldPos;
            rightFootIK.targetPosition.y = floorY;       // plant the foot ON the terrain surface
            if (printDebug) std::cout << "[FootIK] RIGHT: LOCKED @ y=" << footWorldPos.y << " (speed=" << footSpeed << ")\n";
        }

        if (rightFootIK.isLocked) {
            rightFootIK.lockWeight = glm::min(1.0f, rightFootIK.lockWeight + dt * footIKSettings.footLockBlend);
            rightFootIK.timeSinceLock += dt;

            if (rightFootIK.timeSinceLock > 3.0f && isMoving) {
                rightFootIK.isLocked = false;
                rightFootIK.timeSinceLock = 0.0f;
                if (printDebug) std::cout << "[FootIK] RIGHT: Timeout release\n";
            }

            // Release lock when foot moves up significantly from LOCKED position
            float releaseThreshold = isMoving ? 0.12f : 0.08f;
            float distFromLock = footWorldPos.y - rightFootIK.lockedPosition.y;
            if (distFromLock > releaseThreshold || (isMoving && footSpeed > 0.25f)) {
                rightFootIK.isLocked = false;
                if (printDebug) std::cout << "[FootIK] RIGHT: Lift release (dy=" << distFromLock << " speed=" << footSpeed << ")\n";
            }

            // Over-stride / body-commit release — see left foot.
            {
                const int rtOs = footIKSettings.rightUpLegBone;
                if (rtOs >= 0 && rtOs < (int)currBoneWorldPos.size() && dynamicRightLegLength > 0.0f) {
                    glm::vec3 hipW = glm::vec3(modelMatrix * glm::vec4(currBoneWorldPos[rtOs], 1.0f));
                    float pelvisDeltaY = currentPelvisDropY - prevPelvisDropY;
                    hipW.y -= pelvisDeltaY;
                    float hipToFoot = glm::length(hipW - rightFootIK.targetPosition);
                    float maxBridge = dynamicRightLegLength + 0.5f * footIKSettings.maxIKDistance;
                    // Velocity-robust release (see left foot for rationale)
                    float horizSpeed = glm::length(glm::vec2(characterVelocity.x, characterVelocity.z));
                    float velocityBonus = glm::min(maxBridge * 0.5f, horizSpeed * 0.3f);
                    float dynamicBridge = maxBridge + velocityBonus;
                    if (isMoving && hipToFoot > dynamicBridge && rightFootIK.overStrideArmed) {
                        rightFootIK.isLocked = false;
                        rightFootIK.lockWeight = 0.0f;
                        rightFootIK.ankleOffset = glm::vec3(0.0f);
                        rightFootIK.ankleOffsetSmoothed = glm::vec3(0.0f);
                        rightFootIK.contactConfirm = 0;
                        rightFootIK.overStrideArmed = false;
                        if (printDebug) std::cout << "[FootIK] RIGHT: Over-stride release (hip->foot=" << hipToFoot << " > bridge=" << dynamicBridge << " velBonus=" << velocityBonus << ")\n";
                    } else if (hipToFoot < dynamicRightLegLength) {
                        rightFootIK.overStrideArmed = true;
                    }
                }
            }
        } else {
            rightFootIK.lockWeight = glm::max(0.0f, rightFootIK.lockWeight - dt * footIKSettings.footLockReleaseSpeed * releaseSpeedMult);
            rightFootIK.timeSinceLock = 0.0f;
        }

        float ikWeight = rightFootIK.lockWeight * footIKSettings.ikStrength;
        const int rt = footIKSettings.rightUpLegBone; // thigh
        const int rs = footIKSettings.rightLegBone;   // shin
        glm::vec3 rightAnkleWE(0.0f), rightKneeWE(0.0f);
        if (ikWeight > 0.001f) {
            bool bent = false;
            if (rt >= 0 && rs >= 0 && footIKSettings.kneeBendWeight > 0.0f) {
                glm::mat4 thighRot, shinRot;
                glm::vec3 ankleOff;
                if (DebugSolveLegIK(rt, rs, rightFoot, modelMatrix,
                               rightFootIK.targetPosition, thighRot, shinRot, ankleOff,
                               rightAnkleWE, rightKneeWE)) {
                    float bendW = ikWeight * footIKSettings.kneeBendWeight;
                    glm::quat qt = glm::slerp(glm::quat(1.0f,0,0,0), glm::quat_cast(thighRot), bendW);
                    glm::quat qs = glm::slerp(glm::quat(1.0f,0,0,0), glm::quat_cast(shinRot), bendW);
                    if (rt < (int)ikRotations.size()) ikRotations[rt] = glm::mat4_cast(qt);
                    if (rs < (int)ikRotations.size()) ikRotations[rs] = glm::mat4_cast(qs);
                    rightFootIK.ankleOffset = ankleOff * ikWeight;   // residual: exact floor contact
                    bent = true;
                }
            }
            if (!bent) {
                // Fallback (no leg bones / solve failed): pure ankle translate.
                glm::vec3 targetPos = rightFootIK.targetPosition;
                glm::vec3 currentPos = footWorldPos;
                glm::vec3 offset = targetPos - currentPos;
                if (glm::length(offset) > footIKSettings.maxIKDistance) {
                    offset = glm::normalize(offset) * footIKSettings.maxIKDistance;
                }
                glm::vec3 modelOffset = (ikWorldScale > 0.0f) ? offset / ikWorldScale : offset;
                rightFootIK.ankleOffset = modelOffset * ikWeight;
            }
        } else {
            rightFootIK.ankleOffset = glm::vec3(0.0f);
        }

        // Snap the ankle to the floor on a normal plant / lift, smooth only the
        // over-extension spike (see left foot comment for the full rationale).
        {
            float ankleMagModel = glm::length(rightFootIK.ankleOffset);
            // See left foot: maxIKDistance is world-m, ankleMagModel is model (cm);
            // divide by ikWorldScale so the snap gate matches units and the foot
            // plants instantly on a normal contact (only the >half-budget spike smooths).
            float snapBudgetModel = (ikWorldScale > 0.0f)
                ? (0.5f * footIKSettings.maxIKDistance / ikWorldScale)
                : 0.5f * footIKSettings.maxIKDistance;
            if (ankleMagModel <= snapBudgetModel) {
                rightFootIK.ankleOffsetSmoothed = rightFootIK.ankleOffset;
            } else {
                float maxStepWorld = footIKSettings.ankleOffsetMaxSpeed * dt;
                float maxStepModel = (ikWorldScale > 0.0f) ? (maxStepWorld / ikWorldScale) : maxStepWorld;
                glm::vec3 delta = rightFootIK.ankleOffset - rightFootIK.ankleOffsetSmoothed;
                float dl = glm::length(delta);
                if (dl > maxStepModel && maxStepModel > 0.0f) delta *= (maxStepModel / dl);
                rightFootIK.ankleOffsetSmoothed += delta;
            }
            rightFootIK.ankleOffset = rightFootIK.ankleOffsetSmoothed;
        }

        // Ground-normal foot tilt (todo Part 3, Option A) — see left foot.
        if (m_terrainFn && rightFoot >= 0 && rightFoot < (int)ikFootTilt.size()) {
            glm::vec3 rawNormal = SampleTerrainNormal(m_terrainFn, footWorldPos);
            glm::vec3 filteredNormal = FilterGroundNormal(rawNormal, /*isLeft=*/false, dt);
            glm::quat targetTilt = ComputeFootTiltQuat(filteredNormal);
            glm::quat wanted = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), targetTilt, ikWeight);
            float t = glm::clamp(dt * 20.0f, 0.05f, 1.0f);
            ikFootTilt[rightFoot] = glm::slerp(ikFootTilt[rightFoot], wanted, t);
        }

        if (printDebug) {
            glm::vec3 ankleOffWorld = glm::mat3(modelMatrix) * rightFootIK.ankleOffset;
            float plantedY = rightAnkleWE.y + ankleOffWorld.y;
            float kneeDeg = -1.0f;
            if (rt >= 0 && rt < (int)currBoneWorldPos.size()) {
                glm::vec3 hipW = glm::vec3(modelMatrix * glm::vec4(currBoneWorldPos[rt], 1.0f));
                glm::vec3 thighD = hipW - rightKneeWE;
                glm::vec3 shinD  = rightAnkleWE - rightKneeWE;
                float Lth = glm::length(thighD), Lsh = glm::length(shinD);
                if (Lth > 1e-4f && Lsh > 1e-4f)
                    kneeDeg = glm::degrees(acosf(glm::clamp(glm::dot(thighD, shinD) / (Lth * Lsh), -1.0f, 1.0f)));
            }
            float hipToFootR = -1.0f, maxBridgeR = -1.0f;
            if (rt >= 0 && rt < (int)currBoneWorldPos.size() && dynamicRightLegLength > 0.0f) {
                glm::vec3 hW = glm::vec3(modelMatrix * glm::vec4(currBoneWorldPos[rt], 1.0f));
                hW.y -= (currentPelvisDropY - prevPelvisDropY);
                hipToFootR = glm::length(hW - rightFootIK.targetPosition);
                maxBridgeR = dynamicRightLegLength + 0.5f * footIKSettings.maxIKDistance;
            }
            std::cout << "[FootIK Debug] RIGHT: pos=(" << footWorldPos.x << "," << footWorldPos.y << "," << footWorldPos.z
                      << ") speed=" << footSpeed << " locked=" << (rightFootIK.isLocked ? "YES" : "NO")
                      << " weight=" << rightFootIK.lockWeight << " isMoving=" << isMoving
                      << " floorY=" << floorY << " plantedY=" << plantedY
                      << " ankleOffY_m=" << (rightFootIK.ankleOffset.y * ikWorldScale)
                      << " knee=" << kneeDeg
                      << " cc=" << rightFootIK.contactConfirm
                      << " hip2foot=" << hipToFootR << " reach=" << maxBridgeR
                      << " target=(" << rightFootIK.targetPosition.x << "," << rightFootIK.targetPosition.y << "," << rightFootIK.targetPosition.z
                      << ")\n";
        }
    }

    // Apply IK offsets - SET not ADD (we cleared them at frame start)
    if (leftFoot >= 0 && leftFoot < (int)ikOffsets.size()) {
        ikOffsets[leftFoot] = leftFootIK.ankleOffset;
    }
    if (rightFoot >= 0 && rightFoot < (int)ikOffsets.size()) {
        ikOffsets[rightFoot] = rightFootIK.ankleOffset;
    }
}

// ---------------------------------------------------------------------------
// Two-bone leg IK helpers (knee bend).
//
// The leg chain is hip -> knee -> ankle (thigh / shin / foot bones). We solve
// for the knee position that puts the ankle at targetWorld while keeping the
// thigh (L1) and shin (L2) lengths, swinging the thigh about the hip and the
// shin about the knee. The knee is kept on the animated side (no 180deg flip)
// and the target is clamped to the leg's reach sphere so an unreachable foot
// fully extends the leg instead of popping.
// ---------------------------------------------------------------------------

// Rotation (in WORLD space) that maps unit vector `from` onto unit `to`.
static glm::quat SafeQuatFromTo(const glm::vec3& from, const glm::vec3& to)
{
    glm::vec3 a = glm::normalize(from);
    glm::vec3 b = glm::normalize(to);
    float d = glm::dot(a, b);
    if (d >= 0.9995f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);       // parallel
    if (d <= -0.9995f) {                                              // antiparallel: 180 flip
        glm::vec3 axis = (std::fabs(a.x) < 0.9f) ? glm::vec3(1, 0, 0)
                                                 : glm::vec3(0, 1, 0);
        return glm::angleAxis(glm::pi<float>(), axis);
    }
    return glm::rotation(a, b);
}

// Public static terrain-normal sampler (moved from file-static free fn; see
// Animator.h). Central difference on the caller-supplied heightmap fn:
// world (x,z) -> y in metres. Returns world-up (0,1,0) when the fn is null or
// degenerate. The engine's PhysicsWorld raycast only sees axis-aligned box
// bodies + a flat floor plane, so it cannot return a terrain SLOPE normal —
// the terrain callable is the only source of ground slope here.
glm::vec3 Animator::SampleTerrainNormal(const std::function<float(float,float)>& fn,
                                        const glm::vec3& worldPos, float eps)
{
    if (!fn) return glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec2 xz(worldPos.x, worldPos.z);
    const float h   = fn(xz.x,      xz.y);
    const float hx1 = fn(xz.x + eps, xz.y);
    const float hx2 = fn(xz.x - eps, xz.y);
    const float hz1 = fn(xz.x,      xz.y + eps);
    const float hz2 = fn(xz.x,      xz.y - eps);
    if (!std::isfinite(h) || !std::isfinite(hx1) || !std::isfinite(hx2) ||
        !std::isfinite(hz1) || !std::isfinite(hz2))
        return glm::vec3(0.0f, 1.0f, 0.0f);
    // heightmap z=f(x,y) with y up: grad=(dh/dx,dh/dz); up-normal ~ (-dh/dx,1,-dh/dz)
    glm::vec3 grad(-(hx1 - hx2), 2.0f * eps, -(hz1 - hz2));
    float l = glm::length(grad);
    if (l < 1e-6f) return glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 n = grad / l;           // y is always +2*eps > 0 -> points up
    if (!std::isfinite(n.x)) return glm::vec3(0.0f, 1.0f, 0.0f);
    return n;
}

// Adaptive 1-D Kalman filter for ground-normal smoothing (Fix 1 from updated
// todo: eliminates sub-pixel terrain raycast flutter at idle). The filter's
// measurement noise R is dynamically scaled by ankle speed: when the foot is
// stationary, R is high relative to the tiny measurement noise, so the filter
// trusts its prediction (flat) and rejects the 0.2° edge-flicker. When the leg
// sweeps quickly, ankleSpeed raises the denominator, R drops, and the filter
// tracks the genuine slope change without lag.
glm::vec3 Animator::FilterGroundNormal(const glm::vec3& rawMeasurement,
                                       bool isLeftLeg, float dt)
{
    (void)dt;  // reserved for future gain-scheduling
    auto& k = isLeftLeg ? leftAnkleKalman : rightAnkleKalman;

    // Normalize incoming input to protect vector-space integrity
    glm::vec3 z = glm::length(rawMeasurement) > 1e-4f
                      ? glm::normalize(rawMeasurement)
                      : glm::vec3(0.0f, 1.0f, 0.0f);

    // 1. Predict Phase — ground slope doesn't mutate unless the character
    //    steps onto a new face, so grow the covariance by process noise.
    glm::vec3 p_pred = k.P + glm::vec3(k.Q);

    // 2. Dynamic Adaptive Gain Pass — scale R by ankle speed so stationary
    //    filtering is heavy and locomotion tracks are light.
    float ankleSpeed = glm::length(isLeftLeg ? leftFootIK.ankleOffset
                                             : rightFootIK.ankleOffset);
    float dynamicR = k.R * (1.0f / (1.0f + ankleSpeed * 25.0f));

    // 3. Update Phase
    glm::vec3 K_gain = p_pred / (p_pred + glm::vec3(dynamicR));
    k.x = k.x + K_gain * (z - k.x);
    k.P = (glm::vec3(1.0f) - K_gain) * p_pred;

    // Re-verify mathematical unit-sphere constraint
    float len = glm::length(k.x);
    if (len < 1e-6f) return glm::vec3(0.0f, 1.0f, 0.0f);
    return k.x / len;
}

// Free fn (declared in Animator.h) — world rotation that maps `footUp` onto the
// ground normal. Delegates to SafeQuatFromTo so parallel/antiparallel cases are
// guarded (no NaN, no 180 pop). Foot IK terrain alignment (todo Part 3, Option A).
glm::quat ComputeFootTiltQuat(const glm::vec3& groundNormal, const glm::vec3& footUp)
{
    glm::vec3 n = groundNormal;
    float l = glm::length(n);
    if (l < 1e-6f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);   // degenerate -> flat foot
    n /= l;
    // SafeQuatFromTo maps `footUp` onto the (now unit) ground normal; for flat
    // ground (n==up) it returns identity, so the no-tilt common case is a no-op.
    return SafeQuatFromTo(footUp, n);
}

// Extract a pure rotation quaternion from a (possibly scaled) matrix. The
// bone matrices carry the 0.01 world scale from the model matrix, which would
// make glm::quat_cast return a non-unit (wrong) quaternion; normalising the
// column axes strips uniform+non-uniform scale first.
static glm::quat QuatFromMat(const glm::mat4& m)
{
    glm::vec3 c0(m[0].x, m[0].y, m[0].z);
    glm::vec3 c1(m[1].x, m[1].y, m[1].z);
    glm::vec3 c2(m[2].x, m[2].y, m[2].z);
    if (glm::length(c0) < 1e-6f) c0 = glm::vec3(1, 0, 0);
    if (glm::length(c1) < 1e-6f) c1 = glm::vec3(0, 1, 0);
    if (glm::length(c2) < 1e-6f) c2 = glm::vec3(0, 0, 1);
    c0 = glm::normalize(c0);
    c1 = glm::normalize(c1);
    c2 = glm::normalize(c2);
    return glm::quat_cast(glm::mat3(c0, c1, c2));
}

bool Animator::SolveLegIK(int thighBone, int shinBone, int ankleBone,
                          const glm::mat4& modelMatrix, const glm::vec3& targetWorld,
                          glm::mat4& outThighRot, glm::mat4& outShinRot,
                          glm::vec3& outAnkleOffsetModel,
                          glm::vec3& outAnkleWorldEnd, glm::vec3& outKneeWorldEnd) const
{
    outThighRot = glm::mat4(1.0f);
    outShinRot = glm::mat4(1.0f);
    outAnkleOffsetModel = glm::vec3(0.0f);
    outAnkleWorldEnd = glm::vec3(0.0f);
    outKneeWorldEnd = glm::vec3(0.0f);

    if (thighBone < 0 || shinBone < 0 || ankleBone < 0) return false;
    if ((size_t)thighBone >= globalBoneMatrices.size() ||
        (size_t)shinBone  >= globalBoneMatrices.size() ||
        (size_t)ankleBone >= globalBoneMatrices.size()) return false;
    if (ikWorldScale <= 0.0f) return false;

    // --- Compute no-IK bone world matrices for correct local-frame conversion ---
    // globalBoneMatrices[bone] includes the PREVIOUS frame's ikXform. For the
    // world->local conversion below (outThighRot = inv(qT) * Rthigh * qT), we
    // need qT/qS to be the bone's world rotation WITHOUT ik, because that's what
    // RevalidateIK's EvaluateNode uses as the base (globalTransform * ikXform).
    // Thigh and shin bones carry only ikRotations (no ikOffsets), and the thigh's
    // parent (Hips) has no ik at all, so undoing is straightforward.
    //
    // CRITICAL: H, K, A (used for the two-bone solve) must ALSO come from the
    // no-IK bone positions. Otherwise Rthigh is computed to swing the OLD-IK knee
    // direction to Ktarget, but outThighRot is applied to the no-IK bone in
    // RevalidateIK, where the knee is in a different position → visual knee
    // never matches the IK solve.
    //
    // ikRotations[] were RESET to identity by UpdateFootIK before this call, so
    // reading them directly gives identity — that makes gT_noIK =
    // globalBoneMatrices (still baked with last frame's IK). We saved the old
    // values before reset in savedOld{Left,Right}{Thigh,Shin,Ik} and use those here.
    bool isLeftLeg = (shinBone == footIKSettings.leftLegBone);
    glm::mat4 oldThighIk = isLeftLeg ? savedOldLeftThighIk  : savedOldRightThighIk;
    glm::mat4 oldShinIk  = isLeftLeg ? savedOldLeftShinIk   : savedOldRightShinIk;
    glm::mat4 ikXformA(1.0f);
    if ((size_t)ankleBone < ikRotations.size()) ikXformA = ikRotations[ankleBone];
    // Ankle bone also carries an ikOffset (ankleOffset for floor contact).
    if ((size_t)ankleBone < ikOffsets.size() && ikOffsets[ankleBone] != glm::vec3(0.0f))
        ikXformA = ikXformA * glm::translate(glm::mat4(1.0f), ikOffsets[ankleBone]);

    // Undo each bone's PREVIOUS-FRAME ikXform to get the true no-IK world matrix.
    glm::mat4 gT_noIK = globalBoneMatrices[thighBone] * glm::inverse(oldThighIk);
    glm::mat4 gS_noIK = gT_noIK * glm::inverse(globalBoneMatrices[thighBone]) *
                        (globalBoneMatrices[shinBone] * glm::inverse(oldShinIk));
    // Ankle: undo ankle's ikXform (rotation + offset), re-parent under no-IK shin.
    glm::mat4 gA_noIK = gS_noIK * glm::inverse(globalBoneMatrices[shinBone]) *
                        (globalBoneMatrices[ankleBone] * glm::inverse(ikXformA));

    // World joint positions from the no-IK bone matrices — these match the
    // base RevalidateIK uses (animated pose, no old IK) as its starting point.
    // These H/K/A were sampled during the FIRST skeleton pass (the
    // evaluateBoneMatrices call inside Update), so they still carry the
    // PREVIOUS-frame pelvis drop (prevPelvisDropY). The final render pass
    // (RevalidateIK) re-evaluates with the live currentPelvisDropY, so the
    // joints are re-aligned to the live pelvis height just below before the
    // two-bone solve runs. Leg lengths L1/L2 come from the no-IK bone chain.
    glm::vec3 H = glm::vec3(modelMatrix * gT_noIK * glm::vec4(0, 0, 0, 1.0f)); // hip
    glm::vec3 K = glm::vec3(modelMatrix * gS_noIK * glm::vec4(0, 0, 0, 1.0f)); // knee
    glm::vec3 A = glm::vec3(modelMatrix * gA_noIK * glm::vec4(0, 0, 0, 1.0f)); // ankle

    // =========================================================================
    // INJECT HERE: Re-align joint positions with the live pelvis height profile
    // to prevent 1-frame lag. H/K/A above carry the previous-frame pelvis
    // drop (prevPelvisDropY); the final revalidation pass uses the live
    // currentPelvisDropY. Shift the joints along world-Y to match.
    // =========================================================================
    float pelvisDeltaY = currentPelvisDropY - prevPelvisDropY;
    if (std::abs(pelvisDeltaY) > 0.0001f) {
        // Shift joint positions down along the world-Y axis to match the new drop
        H.y -= pelvisDeltaY;
        K.y -= pelvisDeltaY;
        A.y -= pelvisDeltaY;
    }

    float L1 = glm::length(K - H); // thigh length
    float L2 = glm::length(A - K); // shin length
    if (L1 < 1e-4f || L2 < 1e-4f) return false;

    // UE phase 3 (non-elastic) + phase 2 (soft-extension) clamp on the
    // hip->target distance, so an unreachable foot fully extends the leg to its
    // max reach instead of stretching/popping the leg geometry. The residual
    // foot->floor gap is then absorbed by the ankleOffset (capped at
    // maxIKDistance below), never by elongating the bones.
    glm::vec3 toTarget = targetWorld - H;
    float d = glm::length(toTarget);
    // --- NON-LINEAR POSTURE SPRING RELAXER (Fix 2 from updated todo) ---
    // When a joint is pushed beyond its geometric limit, clipping the math
    // creates sharp data spikes. Instead of a hard clamp, we map the
    // over-extension to a smooth transcendental decay curve that pulls the
    // target back within native bounds while keeping the knee fluidly flexed.
    // Begin padding at 92% extension; hard floor at 97%.
    float totalLegLength = L1 + L2;
    float softExtensionLimitStart = totalLegLength * 0.92f;
    if (d > softExtensionLimitStart) {
        float overextensionScaleZone = totalLegLength - softExtensionLimitStart;
        // Smooth compression map: decelerates distance elongation toward zero
        // asymptote, eliminating the sudden velocity shockwave that caused the
        // 0.0486 m/s lock-breaking feedback loop.
        d = softExtensionLimitStart +
            overextensionScaleZone *
                (1.0f - std::exp(-(d - softExtensionLimitStart) / overextensionScaleZone));
        // Final safety floor: never let d pass 97% of total leg length so the
        // knee always retains a mandatory 3% bend margin under deep loads.
        if (d > (totalLegLength * 0.97f)) {
            d = totalLegLength * 0.97f;
        }
    }
    float maxReach = L1 + L2 - 1e-4f;
    // UE phase 2 - Extension damping (soft-limiting): as d enters the last
    // `softExtensionFactor` (5%) of max leg length, ease it toward maxReach with
    // a non-linear exponential curve. Flat ground wants d == maxReach (knee
    // -> 180, locked/snapping); this holds a microscopic knee bend instead.
    // Knee-pop/stretch is worse on the larger 70%-scale bot, which hits full
    // extension more often.
    float softLimitStart = maxReach * (1.0f - footIKSettings.softExtensionFactor);
    if (d > softLimitStart && !footIKSettings.bAllowStretching) {
        float scaleZone = maxReach - softLimitStart; // == maxReach * softExtensionFactor
        d = softLimitStart + scaleZone * (1.0f - std::exp(-(d - softLimitStart) / scaleZone));
    }
    // UE phase 3 - Non-elastic target clamp (no stretch): cap d at maxReach.
    // bAllowStretching (off by default) instead scales the bone lengths
    // uniformly by d/maxReach for a cartoony elastic stretch.
    if (d > maxReach) {
        if (footIKSettings.bAllowStretching) {
            float stretchRatio = d / maxReach;
            L1 *= stretchRatio;
            L2 *= stretchRatio;
        } else {
            d = maxReach;
        }
    }
    if (d < 1e-4f) return false;
    glm::vec3 dir = glm::normalize(toTarget);
    glm::vec3 Tgt = H + dir * d;                       // reachable foot target

    // Knee position on the reachable arc (analytical two-bone).
    float cosA = (L1 * L1 + d * d - L2 * L2) / (2.0f * L1 * d);
    cosA = glm::clamp(cosA, -1.0f, 1.0f);
    // Never hyperextend the knee past ikKneeClampDeg (default 175deg): on flat
    // ground the over-reach two-bone wants to fully straighten the leg (knee
    // -> 180 = pi, which is both anatomically locked and the (0,pi) failure
    // bound). Clamp the hip angle A so knee = pi - 2*A >= ikKneeClampDeg.
    {
        float Amin = (glm::pi<float>() - glm::radians(footIKSettings.ikKneeClampDeg)) * 0.5f;
        cosA = glm::min(cosA, cosf(Amin));
    }
    // Never crouch the knee past ikMinKneeClampDeg (default 140deg): during
    // force-release + re-lock (e.g. a 0.3m pose-switch discontinuity) the foot
    // target can sit close enough to the hip that the bare two-bone solver
    // folds the knee to 90-110°, producing the visible stretch+jitter. This
    // floor on A keeps the knee in [ikMinKneeClampDeg, ikKneeClampDeg]; the
    // ankleOffset residual (clamped by maxIKDistance) absorbs the remaining
    // plant gap so the foot still contacts the floor.
    {
        float Amax = (glm::pi<float>() - glm::radians(footIKSettings.ikMinKneeClampDeg)) * 0.5f;
        cosA = glm::max(cosA, cosf(Amax));
    }
    float sinA = sqrtf(glm::max(0.0f, 1.0f - cosA * cosA));

    // Keep the knee on the animated side: leg-plane normal -> perpendicular
    // toward the animated knee. Falls back to +Z if the leg is nearly straight.
    glm::vec3 planeN = glm::normalize(glm::cross(K - H, A - H));
    if (!std::isfinite(planeN.x) || glm::length(planeN) < 1e-4f) planeN = glm::vec3(0, 0, 1);
    glm::vec3 perp = glm::normalize(glm::cross(planeN, dir));
    if (!std::isfinite(perp.x) || glm::length(perp) < 1e-4f) perp = glm::vec3(0, 0, 1);
    if (glm::dot(perp, K - H) < 0.0f) perp = -perp;
    glm::vec3 Ktarget = H + dir * (L1 * cosA) + perp * (L1 * sinA);

    // WORLD rotations: swing thigh about the hip, then shin about the knee.
    glm::quat Rthigh = SafeQuatFromTo(K - H, Ktarget - H);
    glm::vec3 A_thigh = H + glm::rotate(Rthigh, A - H);          // whole leg swung about the hip
    glm::quat Rshin  = SafeQuatFromTo(A_thigh - Ktarget, Tgt - Ktarget);
    glm::vec3 A_final = Ktarget + glm::rotate(Rshin, A_thigh - Ktarget);

    // Post-solve knee-angle clamp: the cosA clamp above controls the hip
    // angle A, but the ACTUAL knee angle at Ktarget is governed by the
    // two-rotation solve. When cosA is clamped, |Tgt-Ktarget| ≠ L2 (the
    // law-of-cosines identity breaks), so A_final drifts from Tgt and the
    // knee can still fold to < 110° even though cosA says "140°". Iteratively
    // push Ktarget toward the straight-leg position until the measured
    // knee angle meets the visual minimum, so the rendered leg never shows
    // an extreme crouch. The ankleOffset residual absorbs the gap.
    {
        float minKneeRad = glm::radians(footIKSettings.ikMinKneeClampDeg);
        glm::vec3 HmK = H - Ktarget;
        glm::vec3 AmK = A_final - Ktarget;
        float bentKnee = acosf(glm::clamp(glm::dot(HmK, AmK) /
                                  (glm::length(HmK) * glm::length(AmK)), -1.0f, 1.0f));
        static int dbgClamp = 0;
        if (bentKnee < minKneeRad && cosA < 0.9995f) {
            dbgClamp++;
            if (dbgClamp < 5) {
                std::cout << "[CLAMP_DEBUG] enter: cosA=" << cosA << " bentKnee=" << glm::degrees(bentKnee)
                          << " L1=" << L1 << " L2=" << L2 << " d=" << d
                          << " cosAmax=" << cosf((glm::pi<float>() - glm::radians(footIKSettings.ikMinKneeClampDeg)) * 0.5f)
                          << "\n";
            }
            // Ktarget is currently too bent; march cosA toward 1.0 (unbend
            // the knee) and re-solve until the knee angle meets the floor.
            // Use binary search between cosA and 0.9999 for efficiency: each
            // trial bisects the remaining interval, so 20 iterations give
            // cosA precision of (1-cosA)/2^20 ≈ 1e-6 — more than enough.
            float lo = cosA;
            float hi = 0.9999f;
            float bestCosA = cosA;
            float bestKnee = bentKnee;
            for (int it = 0; it < 20; ++it) {
                float trialCosA = 0.5f * (lo + hi);
                float trialSinA = sqrtf(glm::max(0.0f, 1.0f - trialCosA * trialCosA));
                glm::vec3 Kt = H + dir * (L1 * trialCosA) + perp * (L1 * trialSinA);
                glm::quat Rt = SafeQuatFromTo(K - H, Kt - H);
                glm::vec3 At = H + glm::rotate(Rt, A - H);
                glm::quat Rs = SafeQuatFromTo(At - Kt, Tgt - Kt);
                glm::vec3 Af = Kt + glm::rotate(Rs, At - Kt);
                glm::vec3 hmk = H - Kt;
                glm::vec3 amk = Af - Kt;
                float trialKnee = acosf(glm::clamp(glm::dot(hmk, amk) /
                                                  (glm::length(hmk) * glm::length(amk)), -1.0f, 1.0f));
                if (dbgClamp <= 5) {
                    std::cout << "[CLAMP_DEBUG]   it=" << it << " cosA=" << trialCosA
                              << " knee=" << glm::degrees(trialKnee) << "\n";
                }
                if (trialKnee >= minKneeRad) {
                    // Accept and keep searching higher for a larger knee angle.
                    bestCosA = trialCosA;
                    bestKnee = trialKnee;
                    cosA    = trialCosA;
                    sinA    = trialSinA;
                    Ktarget = Kt;
                    Rthigh  = Rt;
                    A_thigh = At;
                    Rshin   = Rs;
                    A_final = Af;
                    lo = trialCosA;  // try to push even higher
                } else {
                    // Trial didn't reach 140°. Track the best (highest) knee.
                    if (trialKnee > bestKnee) {
                        bestKnee = trialKnee;
                        bestCosA = trialCosA;
                    }
                    hi = trialCosA;  // need more cosA (more straight)
                }
            }
            // Fallback: if no trial reached the minimum knee angle (geometry
            // makes 140° physically impossible because the foot target is
            // closer to the hip than the leg length), accept the trial with
            // the HIGHEST knee angle found (closest to the minimum).
            if (bestKnee < minKneeRad) {
                float trialSinA = sqrtf(glm::max(0.0f, 1.0f - bestCosA * bestCosA));
                glm::vec3 Kt = H + dir * (L1 * bestCosA) + perp * (L1 * trialSinA);
                glm::quat Rt = SafeQuatFromTo(K - H, Kt - H);
                glm::vec3 At = H + glm::rotate(Rt, A - H);
                glm::quat Rs = SafeQuatFromTo(At - Kt, Tgt - Kt);
                glm::vec3 Af = Kt + glm::rotate(Rs, At - Kt);
                cosA    = bestCosA;
                sinA    = trialSinA;
                Ktarget = Kt;
                Rthigh  = Rt;
                A_thigh = At;
                Rshin   = Rs;
                A_final = Af;
            }
            if (dbgClamp <= 5 || dbgClamp > 994) {
                std::cout << "[CLAMP_DEBUG] exit: finalKnee=" << glm::degrees(bentKnee)
                          << " finalCosA=" << cosA << "\n";
            }
            dbgClamp++;
        }
        if (dbgClamp > 994 && dbgClamp <= 1000) {
            std::cout << "[CLAMP_DEBUG] exit: finalKnee=" << glm::degrees(bentKnee)
                      << " finalCosA=" << cosA << "\n";
        }
    }

    // [debug] - one-shot solver diagnostics: reachability + bent knee angle.
    static int dbgN = 0;
    dbgN++;
    if (dbgN < 8 || dbgN > 250) {
        glm::vec3 toTarget = targetWorld - H;
        float dd = glm::length(toTarget);
        float maxReach = L1 + L2;
        float animKnee = acosf(glm::clamp(glm::dot(K - H, A - K) / (L1 * L2), -1.0f, 1.0f));
        float bentKnee = acosf(glm::clamp(glm::dot(H - Ktarget, A_final - Ktarget) /
                                          (glm::length(H - Ktarget) * glm::length(A_final - Ktarget)), -1.0f, 1.0f));
        std::cout << "[SolveLegIK] dbgN="<<dbgN<<" thigh="<<thighBone<<" shin="<<shinBone<<" ankle="<<ankleBone
                  << " L1="<<L1<<" L2="<<L2<<" d="<<dd<<" maxReach="<<maxReach
                  << " reach="<<(dd<=maxReach?"YES":"NO")
                  << " animKnee="<<glm::degrees(animKnee)
                  << " bentKnee="<<glm::degrees(bentKnee)
                  << " A_final.y="<<A_final.y<<" tgt.y="<<targetWorld.y
                  << " noIK_fix=yes"
                  << "\n";
    }

    // Convert the world swings into each bone's LOCAL frame. The eval applies
    // ikRotation as  globalBoneMatrices[b] * ikRotation  (model space), so the
    // local rotation is  Rb⁻¹ * Rswing * Rb  where Rb is the bone's world rot.
    // CRITICAL: qT_world / qS_world must be the NO-IK bone rotation (the one
    // EvalNode uses as the base before applying ikXform). Using the with-IK
    // matrix from globalBoneMatrices would bake the old frame's ik into the
    // local->world conversion, causing the re-validated knee to miss the clamp.
    glm::quat qT_world = QuatFromMat(modelMatrix * gT_noIK);          // thigh world rot (no IK)
    glm::quat qS_world = QuatFromMat(modelMatrix * gS_noIK);          // shin  world rot (no IK)
    glm::quat qS_after = Rthigh * qS_world;                      // shin world rot after thigh swing
    outThighRot = glm::mat4_cast(glm::inverse(qT_world) * Rthigh * qT_world);
    outShinRot  = glm::mat4_cast(glm::inverse(qS_after) * Rshin * qS_after);

    // [debug] Check Rshin and outShinRot
    if (dbgN < 8 || dbgN > 250) {
        float rshinAngle = glm::angle(Rshin);
        float outShinAngle = glm::angle(glm::quat_cast(outShinRot));
        std::cout << "[SolveLegIK] Rshin_angle=" << glm::degrees(rshinAngle)
                  << " outShin_angle=" << glm::degrees(outShinAngle)
                  << " qS_after_w=" << qS_after.w
                  << " qT_world_w=" << qT_world.w
                  << "\n";
    }

    // Residual ankle translate (world -> model) for exact floor contact.
    // ~0 when the target is reachable (the knee bend places the foot). When the
    // floor target is UNREACHABLE (foot yanked down past the leg's length), an
    // unbounded ankleOffset here would drag the ankle past the joint limit and
    // STRETCH the fully-extended leg. Clamp the ankle travel to maxIKDistance
    // (the per-leg "max distance the foot can reach" budget) and let the
    // two-bone knee bend carry the rest - the leg extends to its max reach
    // instead of elastically stretching. This is the knob `maxIKDistance` was
    // added for and was never applied.
    glm::vec3 ankleWorldDelta = targetWorld - A_final;
    const float dd = glm::length(ankleWorldDelta);
    // FIX (refreshed todo Fix 2): During idle / stationary periods, shrink the
    // ankle translation budget by 75% so the two-bone solver handles the pose
    // structurally (through knee / hip rotation) rather than structural
    // translation offsets. This kills the micro-sliding that occurs when
    // sub-frame terrain-normal fluctuations produce small, persistent ankle
    // offsets while the character is physically static — the residual gets
    // rate-limited and never settles, dragging the foot across the floor.
    float horizontalSpeed = glm::length(glm::vec2(characterVelocity.x,
                                                  characterVelocity.z));
    float effectiveMaxIKDistance = footIKSettings.maxIKDistance;
    if (horizontalSpeed < 0.05f) {
        effectiveMaxIKDistance *= 0.25f;
    }
    if (dd > effectiveMaxIKDistance) {
        ankleWorldDelta *= (effectiveMaxIKDistance / dd);
    }
    outAnkleOffsetModel = glm::vec3(glm::inverse(modelMatrix) * glm::vec4(ankleWorldDelta, 0.0f));
    outAnkleWorldEnd = A_final;
    outKneeWorldEnd = Ktarget;

    // REST-STATE GROUND-ALIGNMENT DAMPING (Fix 3 from updated todo): at an
    // absolute standstill the two-bone solver and the micro-velocity noise in
    // the ground normal drive sub-degree per-frame chatter in the thigh/shin
    // rotation matrices. When the leg is at full extension (knee ~174-176°)
    // this chatter is amplified by the trig compression near 180° and vibrates
    // the entire foot mesh. Apply a 10 Hz low-pass envelope on the output
    // rotations to kill the high-frequency torque noise so the standing pose is
    // glass-smooth. During locomotion the filter is bypassed (no lag /
    // foot-dragging).
    static glm::quat dampedThighQuatL(1.0f, 0.0f, 0.0f, 0.0f);
    static glm::quat dampedShinQuatL(1.0f, 0.0f, 0.0f, 0.0f);
    static glm::quat dampedThighQuatR(1.0f, 0.0f, 0.0f, 0.0f);
    static glm::quat dampedShinQuatR(1.0f, 0.0f, 0.0f, 0.0f);
    if (footIKSettings.restDampingEnabled) {
        float dampingFactor = 10.0f * lastDeltaTime;
        if (isLeftLeg) {
            dampedThighQuatL = glm::slerp(dampedThighQuatL,
                                           glm::quat_cast(outThighRot), dampingFactor);
            dampedShinQuatL  = glm::slerp(dampedShinQuatL,
                                           glm::quat_cast(outShinRot),  dampingFactor);
            outThighRot = glm::mat4_cast(dampedThighQuatL);
            outShinRot  = glm::mat4_cast(dampedShinQuatL);
        } else {
            dampedThighQuatR = glm::slerp(dampedThighQuatR,
                                           glm::quat_cast(outThighRot), dampingFactor);
            dampedShinQuatR  = glm::slerp(dampedShinQuatR,
                                           glm::quat_cast(outShinRot),  dampingFactor);
            outThighRot = glm::mat4_cast(dampedThighQuatR);
            outShinRot  = glm::mat4_cast(dampedShinQuatR);
        }
    }

    return true;
}

// ---- Debug: check for extreme knee bends ----
bool Animator::DebugSolveLegIK(int thighBone, int shinBone, int ankleBone,
                               const glm::mat4& modelMatrix, const glm::vec3& targetWorld,
                               glm::mat4& outThighRot, glm::mat4& outShinRot,
                               glm::vec3& outAnkleOffsetModel,
                               glm::vec3& outAnkleWorldEnd, glm::vec3& outKneeWorldEnd) const
{
    bool result = SolveLegIK(thighBone, shinBone, ankleBone, modelMatrix, targetWorld,
                             outThighRot, outShinRot, outAnkleOffsetModel,
                             outAnkleWorldEnd, outKneeWorldEnd);
    if (!result) return false;

    // Use the SAME H as SolveLegIK (currBoneWorldPos, old-IK) for consistency.
    // The kneeDeg here measures the desired knee at Ktarget/A_final, matching
    // what the iterative clamp sees.
    glm::vec3 H = glm::vec3(modelMatrix * glm::vec4(currBoneWorldPos[thighBone], 1.0f));
    glm::vec3 K = outKneeWorldEnd;
    glm::vec3 A = outAnkleWorldEnd;
    glm::vec3 thighD = H - K;
    glm::vec3 shinD  = A - K;
    float Lth = glm::length(thighD), Lsh = glm::length(shinD);
    if (Lth > 1e-4f && Lsh > 1e-4f) {
        float kneeDeg = glm::degrees(acosf(glm::clamp(glm::dot(thighD, shinD) / (Lth * Lsh), -1.0f, 1.0f)));
        if (kneeDeg < 170.0f) {
            glm::vec3 toTarget = targetWorld - H;
            float d = glm::length(toTarget);
            std::cout << "[SOLVE_DEBUG] kneeDeg=" << kneeDeg << " d=" << d
                      << " H=(" << H.x << "," << H.y << "," << H.z << ")"
                      << " tgt=(" << targetWorld.x << "," << targetWorld.y << "," << targetWorld.z << ")"
                      << " Ktarget=(" << K.x << "," << K.y << "," << K.z << ")"
                      << " A_final=(" << A.x << "," << A.y << "," << A.z << ")"
                      << " L1=" << Lth << " L2=" << Lsh << "\n";
        }
    }
    return true;
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
