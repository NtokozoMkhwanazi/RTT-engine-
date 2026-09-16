#include "AnimationLayerSystem.h"
#include <iostream>
#include <algorithm>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

// ============================================================================
// ANIMATION LAYER SYSTEM IMPLEMENTATION
// ============================================================================

AnimationLayerSystem::AnimationLayerSystem() {
    // Default configuration
    config.maxLayers = 8;
    config.maxBones = 120;
    config.defaultBlendDuration = 0.15f;
    config.enablePerBoneBlending = true;
    config.enablePriorityBlending = true;
    config.debugMode = false;
}

AnimationLayerSystem::~AnimationLayerSystem() {
    layers.clear();
    blendedMatrices.clear();
    boneWeights.clear();
}

void AnimationLayerSystem::Initialize(const Skeleton* skeleton, const LayerSystemConfig& config) {
    if (!skeleton) {
        std::cerr << "[AnimationLayerSystem] ERROR: Null skeleton!\n";
        return;
    }

    this->skeleton = skeleton;
    this->config = config;

    // Initialize storage
    blendedMatrices.resize(skeleton->bones.size(), glm::mat4(1.0f));
    boneWeights.resize(skeleton->bones.size(), 0.0f);

    initialized = true;

    std::cout << "[AnimationLayerSystem] Initialized with " << skeleton->bones.size()
              << " bones, max " << config.maxLayers << " layers\n";
}

bool AnimationLayerSystem::AddLayer(const std::string& name, std::shared_ptr<Animation> anim,
                                     LayerBlendMode blendMode, BoneMaskPreset maskPreset,
                                     float initialWeight) {
    if (!initialized) {
        std::cerr << "[AnimationLayerSystem] ERROR: Not initialized!\n";
        return false;
    }

    if (!anim) {
        std::cerr << "[AnimationLayerSystem] ERROR: Null animation for layer: " << name << "\n";
        return false;
    }

    if (layers.size() >= config.maxLayers) {
        std::cerr << "[AnimationLayerSystem] ERROR: Max layers (" << config.maxLayers << ") reached!\n";
        return false;
    }

    // Check if layer already exists
    if (HasLayer(name)) {
        std::cerr << "[AnimationLayerSystem] WARNING: Layer already exists: " << name << "\n";
        return false;
    }

    AnimationLayer layer;
    layer.name = name;
    layer.animation = anim;
    layer.blendMode = blendMode;
    layer.maskPreset = maskPreset;
    layer.weight = initialWeight;
    layer.targetWeight = initialWeight;
    layer.enabled = true;
    layer.loop = true;  // Default to looping (Animation class doesn't have looping member)

    std::cout << "[AnimationLayerSystem] Added layer: " << name
              << " (blend=" << (int)blendMode << ", mask=" << (int)maskPreset
              << ", weight=" << initialWeight << ")\n";

    layers.push_back(layer);

    // Sort by priority
    if (config.enablePriorityBlending) {
        SortLayersByPriority();
    }

    return true;
}

bool AnimationLayerSystem::AddLayerWithCustomMask(const std::string& name, std::shared_ptr<Animation> anim,
                                                   LayerBlendMode blendMode,
                                                   const std::vector<bool>& boneMask,
                                                   float initialWeight) {
    if (!initialized) {
        std::cerr << "[AnimationLayerSystem] ERROR: Not initialized!\n";
        return false;
    }

    if (!anim) {
        std::cerr << "[AnimationLayerSystem] ERROR: Null animation for layer: " << name << "\n";
        return false;
    }

    if (boneMask.size() != skeleton->bones.size()) {
        std::cerr << "[AnimationLayerSystem] ERROR: Bone mask size mismatch! Expected "
                  << skeleton->bones.size() << ", got " << boneMask.size() << "\n";
        return false;
    }

    if (layers.size() >= config.maxLayers) {
        std::cerr << "[AnimationLayerSystem] ERROR: Max layers reached!\n";
        return false;
    }

    if (HasLayer(name)) {
        std::cerr << "[AnimationLayerSystem] WARNING: Layer already exists: " << name << "\n";
        return false;
    }

    AnimationLayer layer;
    layer.name = name;
    layer.animation = anim;
    layer.blendMode = blendMode;
    layer.maskPreset = BoneMaskPreset::CUSTOM;
    layer.customBoneMask = boneMask;
    layer.weight = initialWeight;
    layer.targetWeight = initialWeight;
    layer.enabled = true;
    layer.loop = true;  // Default to looping

    std::cout << "[AnimationLayerSystem] Added custom layer: " << name
              << " (" << std::count(boneMask.begin(), boneMask.end(), true)
              << " bones)\n";

    layers.push_back(layer);

    if (config.enablePriorityBlending) {
        SortLayersByPriority();
    }

    return true;
}

void AnimationLayerSystem::RemoveLayer(const std::string& name, float fadeOutDuration) {
    auto it = std::find_if(layers.begin(), layers.end(),
        [&name](const AnimationLayer& layer) { return layer.name == name; });

    if (it == layers.end()) {
        std::cerr << "[AnimationLayerSystem] WARNING: Layer not found: " << name << "\n";
        return;
    }

    if (fadeOutDuration > 0.0f) {
        // Fade out before removing
        it->targetWeight = 0.0f;
        it->blendOutDuration = fadeOutDuration;

        // Remove when fade completes
        // This is handled in Update()
    } else {
        // Instant removal
        layers.erase(it);
        std::cout << "[AnimationLayerSystem] Removed layer: " << name << "\n";
    }
}

AnimationLayer* AnimationLayerSystem::GetLayer(const std::string& name) {
    auto it = std::find_if(layers.begin(), layers.end(),
        [&name](const AnimationLayer& layer) { return layer.name == name; });

    return (it != layers.end()) ? &(*it) : nullptr;
}

const AnimationLayer* AnimationLayerSystem::GetLayer(const std::string& name) const {
    auto it = std::find_if(layers.begin(), layers.end(),
        [&name](const AnimationLayer& layer) { return layer.name == name; });

    return (it != layers.end()) ? &(*it) : nullptr;
}

bool AnimationLayerSystem::HasLayer(const std::string& name) const {
    return std::any_of(layers.begin(), layers.end(),
        [&name](const AnimationLayer& layer) { return layer.name == name; });
}

void AnimationLayerSystem::SetLayerWeight(const std::string& name, float weight, float duration) {
    AnimationLayer* layer = GetLayer(name);
    if (!layer) {
        std::cerr << "[AnimationLayerSystem] WARNING: Layer not found: " << name << "\n";
        return;
    }

    layer->targetWeight = glm::clamp(weight, 0.0f, 1.0f);

    if (duration > 0.0f) {
        if (weight > layer->weight) {
            layer->blendInDuration = duration;
        } else {
            layer->blendOutDuration = duration;
        }
    } else {
        layer->weight = layer->targetWeight;
    }

    if (config.debugMode) {
        std::cout << "[LayerSystem] Set weight: " << name << " = " << weight << "\n";
    }
}

void AnimationLayerSystem::SetAllLayerWeights(const std::unordered_map<std::string, float>& weights) {
    for (const auto& [name, weight] : weights) {
        SetLayerWeight(name, weight, config.defaultBlendDuration);
    }
}

void AnimationLayerSystem::FadeInLayer(const std::string& name, float duration) {
    SetLayerWeight(name, 1.0f, duration);
}

void AnimationLayerSystem::FadeOutLayer(const std::string& name, float duration) {
    SetLayerWeight(name, 0.0f, duration);
}

void AnimationLayerSystem::SetLayerTime(const std::string& name, float time) {
    AnimationLayer* layer = GetLayer(name);
    if (!layer) return;

    layer->time = fmod(time, layer->animation->duration);
}

void AnimationLayerSystem::SetLayerSpeed(const std::string& name, float speedMultiplier) {
    AnimationLayer* layer = GetLayer(name);
    if (!layer) return;

    layer->speedMultiplier = glm::max(speedMultiplier, 0.0f);
}

void AnimationLayerSystem::SetLayerLooping(const std::string& name, bool loop) {
    AnimationLayer* layer = GetLayer(name);
    if (!layer) return;

    layer->loop = loop;
}

void AnimationLayerSystem::Update(float dt) {
    if (!initialized) return;

    // Update all layers
    for (auto& layer : layers) {
        if (!layer.enabled) continue;

        UpdateLayer(layer, dt);
    }

    // Remove finished layers that are fading out
    layers.erase(
        std::remove_if(layers.begin(), layers.end(),
            [](const AnimationLayer& layer) {
                return layer.targetWeight <= 0.0f && layer.weight <= 0.01f;
            }),
        layers.end());

    // Calculate per-bone weights
    if (config.enablePerBoneBlending) {
        CalculateBoneWeights();
    }
}

void AnimationLayerSystem::UpdateLayer(AnimationLayer& layer, float dt) {
    // Blend weight toward target
    if (layer.weight < layer.targetWeight) {
        layer.weight += dt / layer.blendInDuration;
        if (layer.weight >= layer.targetWeight) {
            layer.weight = layer.targetWeight;
        }
    } else if (layer.weight > layer.targetWeight) {
        layer.weight -= dt / layer.blendOutDuration;
        if (layer.weight <= layer.targetWeight) {
            layer.weight = layer.targetWeight;
        }
    }

    // Update animation time
    if (layer.animation && layer.weight > 0.01f) {
        float animSpeed = layer.speedMultiplier > 0 ? layer.speedMultiplier : 1.0f;
        layer.time += dt * animSpeed;

        // Handle looping
        if (layer.loop) {
            layer.time = fmod(layer.time, layer.animation->duration);
        } else if (layer.time >= layer.animation->duration) {
            layer.time = layer.animation->duration;

            // Call completion callback
            if (layer.onLayerComplete) {
                layer.onLayerComplete();
            }
        }

        // Call update callback
        if (layer.onLayerUpdate) {
            layer.onLayerUpdate(layer.weight);
        }
    }
}

void AnimationLayerSystem::ApplyToAnimator(Animator* animator, float baseWeight) {
    if (!initialized || !animator) return;

    // Sort layers by priority
    if (config.enablePriorityBlending) {
        SortLayersByPriority();
    }

    // Apply layers in priority order
    int layerIndex = 0;
    for (const auto& layer : layers) {
        if (!layer.enabled || layer.weight <= 0.01f || !layer.animation) continue;

        switch (layer.blendMode) {
            case LayerBlendMode::LINEAR:
                ApplyLayerToAnimator(animator, layer, layer.weight * baseWeight);
                break;

            case LayerBlendMode::ADDITIVE:
                ApplyAdditiveLayer(animator, layer, layer.weight * baseWeight);
                break;

            case LayerBlendMode::MULTIPLICATIVE:
                ApplyMultiplicativeLayer(animator, layer, layer.weight * baseWeight);
                break;

            case LayerBlendMode::PROJECTION:
                ApplyProjectionLayer(animator, layer, layer.weight * baseWeight);
                break;
        }

        layerIndex++;
    }
}

void AnimationLayerSystem::ApplyLayerToAnimator(Animator* animator, const AnimationLayer& layer, float weight) {
    if (!animator || !layer.animation) return;

    // Get bone mask for this layer
    std::vector<bool> boneMask = GetLayerBoneMask(layer);

    // Sample the animation at the current layer time
    std::vector<glm::mat4> sampledPose(skeleton->bones.size(), glm::mat4(1.0f));
    
    // Create reverse bone mapping (index -> name)
    std::vector<std::string> boneNames(skeleton->bones.size());
    for (const auto& [name, index] : skeleton->boneMapping) {
        if (index >= 0 && static_cast<size_t>(index) < skeleton->bones.size()) {
            boneNames[index] = name;
        }
    }
    
    // Sample each bone's transform from the animation
    for (size_t boneIndex = 0; boneIndex < skeleton->bones.size(); boneIndex++) {
        if (!boneMask[boneIndex]) {
            // Skip bones not in the mask
            sampledPose[boneIndex] = skeleton->bones[boneIndex].bindTransform;
            continue;
        }

        const std::string& boneName = boneNames[boneIndex];
        
        // Find bone animation track
        auto boneAnimIt = layer.animation->boneAnimations.find(boneName);
        if (boneAnimIt == layer.animation->boneAnimations.end()) {
            // No animation for this bone, use bind pose
            sampledPose[boneIndex] = skeleton->bones[boneIndex].bindTransform;
            continue;
        }

        const BoneAnimation& boneAnim = boneAnimIt->second;
        
        // Interpolate transforms at current layer time
        float timeToUse = fmod(layer.time, layer.animation->duration);
        if (timeToUse < 0) timeToUse += layer.animation->duration;

        glm::vec3 position = boneAnim.InterpolatePosition(timeToUse);
        glm::quat rotation = boneAnim.InterpolateRotation(timeToUse);
        glm::vec3 scale = boneAnim.InterpolateScale(timeToUse);

        // Build transformation matrix
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), position);
        transform *= glm::mat4_cast(rotation);
        transform *= glm::scale(glm::mat4(1.0f), scale);

        sampledPose[boneIndex] = transform;
    }

    // Get the current pose from the animator (base pose).
    // FIX: Pull from globalBoneMatrices (un-skinned) instead of
    // GetFinalBoneMatrices() (post-skinning). The post-skinned matrices
    // have globalInverseTransform and bone.offset baked in — decomposing
    // them destroys IK offsets (root snap/skate/floating on state transitions).
    const std::vector<glm::mat4>& basePose = animator->globalBoneMatrices;

    // Blend sampled pose with base pose using bone mask and weight
    // For proper skeletal blending, blend TRS components separately
    for (size_t i = 0; i < skeleton->bones.size(); i++) {
        if (!boneMask[i]) {
            // Bone not affected by this layer - use base pose
            blendedMatrices[i] = basePose[i];
            continue;
        }

        // Decompose both matrices to TRS for proper blending
        glm::vec3 baseScale, baseTranslation;
        glm::quat baseRotation;
        glm::vec3 baseSkew;
        glm::vec4 basePerspective;
        glm::decompose(basePose[i], baseScale, baseRotation, baseTranslation, baseSkew, basePerspective);

        glm::vec3 sampledScale, sampledTranslation;
        glm::quat sampledRotation;
        glm::vec3 sampledSkew;
        glm::vec4 sampledPerspective;
        glm::decompose(sampledPose[i], sampledScale, sampledRotation, sampledTranslation, sampledSkew, sampledPerspective);

        // Blend TRS components
        glm::vec3 finalTranslation = glm::mix(baseTranslation, sampledTranslation, weight);
        glm::quat finalRotation = glm::slerp(baseRotation, sampledRotation, weight);
        glm::vec3 finalScale = glm::mix(baseScale, sampledScale, weight);

        // Rebuild final matrix in un-skinned global space
        glm::mat4 finalMatrix = glm::translate(glm::mat4(1.0f), finalTranslation);
        finalMatrix *= glm::mat4_cast(finalRotation);
        finalMatrix *= glm::scale(glm::mat4(1.0f), finalScale);

        // Write clean un-skinned matrix back to the animator and keep
        // finalBoneMatrices in sync so skinning doesn't use stale data.
        animator->globalBoneMatrices[i] = finalMatrix;
        auto& finalMats = animator->GetFinalBoneMatricesMutable();
        if (i < finalMats.size() && skeleton) {
            finalMats[i] = skeleton->globalInverseTransform * finalMatrix * skeleton->bones[i].offset;
        }
        blendedMatrices[i] = finalMatrix;
    }

    if (config.debugMode) {
        std::cout << "[LayerSystem] Applied layer: " << layer.name
                  << " weight=" << weight << "\n";
    }
}

void AnimationLayerSystem::ApplyAdditiveLayer(Animator* animator, const AnimationLayer& layer, float weight) {
    if (!animator || !layer.animation) return;

    // Get bone mask for this layer
    std::vector<bool> boneMask = GetLayerBoneMask(layer);

    // Get the current base pose from animator
    const std::vector<glm::mat4>& basePose = animator->GetFinalBoneMatrices();

    // Create reverse bone mapping (index -> name)
    std::vector<std::string> boneNames(skeleton->bones.size());
    for (const auto& [name, index] : skeleton->boneMapping) {
        if (index >= 0 && static_cast<size_t>(index) < skeleton->bones.size()) {
            boneNames[index] = name;
        }
    }

    // Sample the additive animation at current layer time
    std::vector<glm::mat4> additivePose(skeleton->bones.size(), glm::mat4(1.0f));

    for (size_t boneIndex = 0; boneIndex < skeleton->bones.size(); boneIndex++) {
        if (!boneMask[boneIndex]) {
            additivePose[boneIndex] = skeleton->bones[boneIndex].bindTransform;
            continue;
        }

        const std::string& boneName = boneNames[boneIndex];

        // Find bone animation track
        auto boneAnimIt = layer.animation->boneAnimations.find(boneName);
        if (boneAnimIt == layer.animation->boneAnimations.end()) {
            // No animation for this bone, use bind pose
            additivePose[boneIndex] = skeleton->bones[boneIndex].bindTransform;
            continue;
        }

        const BoneAnimation& boneAnim = boneAnimIt->second;

        // Interpolate transforms at current layer time
        float timeToUse = fmod(layer.time, layer.animation->duration);
        if (timeToUse < 0) timeToUse += layer.animation->duration;

        glm::vec3 position = boneAnim.InterpolatePosition(timeToUse);
        glm::quat rotation = boneAnim.InterpolateRotation(timeToUse);
        glm::vec3 scale = boneAnim.InterpolateScale(timeToUse);

        // Build transformation matrix
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), position);
        transform *= glm::mat4_cast(rotation);
        transform *= glm::scale(glm::mat4(1.0f), scale);

        additivePose[boneIndex] = transform;
    }

    // Apply additive blending:
    // FinalPose = BasePose + (AdditivePose - ReferencePose) * weight
    // Reference pose is typically the bind pose (T-pose)

    for (size_t i = 0; i < skeleton->bones.size(); i++) {
        if (!boneMask[i]) {
            blendedMatrices[i] = basePose[i];
            continue;
        }

        // Get reference pose (bind pose)
        glm::mat4 referencePose = skeleton->bones[i].bindTransform;

        // Decompose matrices to TRS for proper additive blending
        glm::vec3 baseScale, baseTranslation;
        glm::quat baseRotation;
        glm::vec3 baseSkew;
        glm::vec4 basePerspective;
        glm::decompose(basePose[i], baseScale, baseRotation, baseTranslation, baseSkew, basePerspective);

        glm::vec3 additiveScale, additiveTranslation;
        glm::quat additiveRotation;
        glm::vec3 additiveSkew;
        glm::vec4 additivePerspective;
        glm::decompose(additivePose[i], additiveScale, additiveRotation, additiveTranslation, additiveSkew, additivePerspective);

        glm::vec3 refScale, refTranslation;
        glm::quat refRotation;
        glm::vec3 refSkew;
        glm::vec4 refPerspective;
        glm::decompose(referencePose, refScale, refRotation, refTranslation, refSkew, refPerspective);

        // Calculate additive offset (additive - reference)
        glm::vec3 positionOffset = additiveTranslation - refTranslation;

        // For rotation, use quaternion difference
        glm::quat rotationDiff = additiveRotation * glm::inverse(refRotation);

        // For scale, use ratio
        glm::vec3 scaleRatio;
        for (int j = 0; j < 3; j++) {
            scaleRatio[j] = (refScale[j] != 0.0f) ? (additiveScale[j] / refScale[j]) : 1.0f;
        }

        // Apply weighted additive offset to base pose
        glm::vec3 finalTranslation = baseTranslation + positionOffset * weight;
        glm::quat finalRotation = glm::slerp(baseRotation, baseRotation * rotationDiff, weight);
        glm::vec3 finalScale = baseScale * glm::mix(glm::vec3(1.0f), scaleRatio, weight);

        // Rebuild final matrix
        glm::mat4 finalMatrix = glm::translate(glm::mat4(1.0f), finalTranslation);
        finalMatrix *= glm::mat4_cast(finalRotation);
        finalMatrix *= glm::scale(glm::mat4(1.0f), finalScale);

        blendedMatrices[i] = finalMatrix;
    }

    if (config.debugMode) {
        std::cout << "[LayerSystem] Applied additive layer: " << layer.name
                  << " weight=" << weight << "\n";
    }
}

// ============================================================================
// MULTIPLICATIVE BLENDING
// ============================================================================
// FinalPose = BasePose * (MultiplicativePose / ReferencePose) ^ weight
// Used for: Scaling animations, intensity control
void AnimationLayerSystem::ApplyMultiplicativeLayer(Animator* animator, const AnimationLayer& layer, float weight) {
    if (!animator || !layer.animation) return;

    // Get bone mask for this layer
    std::vector<bool> boneMask = GetLayerBoneMask(layer);

    // Get the current base pose from animator
    const std::vector<glm::mat4>& basePose = animator->GetFinalBoneMatrices();

    // Create reverse bone mapping (index -> name)
    std::vector<std::string> boneNames(skeleton->bones.size());
    for (const auto& [name, index] : skeleton->boneMapping) {
        if (index >= 0 && static_cast<size_t>(index) < skeleton->bones.size()) {
            boneNames[index] = name;
        }
    }

    // Sample the multiplicative animation at current layer time
    std::vector<glm::mat4> multPose(skeleton->bones.size(), glm::mat4(1.0f));

    for (size_t boneIndex = 0; boneIndex < skeleton->bones.size(); boneIndex++) {
        if (!boneMask[boneIndex]) {
            multPose[boneIndex] = glm::mat4(1.0f);  // Identity for multiplication
            continue;
        }

        const std::string& boneName = boneNames[boneIndex];

        // Find bone animation track
        auto boneAnimIt = layer.animation->boneAnimations.find(boneName);
        if (boneAnimIt == layer.animation->boneAnimations.end()) {
            // No animation for this bone, use identity
            multPose[boneIndex] = glm::mat4(1.0f);
            continue;
        }

        const BoneAnimation& boneAnim = boneAnimIt->second;

        // Interpolate transforms at current layer time
        float timeToUse = fmod(layer.time, layer.animation->duration);
        if (timeToUse < 0) timeToUse += layer.animation->duration;

        glm::vec3 position = boneAnim.InterpolatePosition(timeToUse);
        glm::quat rotation = boneAnim.InterpolateRotation(timeToUse);
        glm::vec3 scale = boneAnim.InterpolateScale(timeToUse);

        // Build transformation matrix
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), position);
        transform *= glm::mat4_cast(rotation);
        transform *= glm::scale(glm::mat4(1.0f), scale);

        multPose[boneIndex] = transform;
    }

    // Apply multiplicative blending per bone
    for (size_t i = 0; i < skeleton->bones.size(); i++) {
        if (!boneMask[i]) {
            blendedMatrices[i] = basePose[i];
            continue;
        }

        // Decompose matrices to TRS
        glm::vec3 baseScale, baseTranslation;
        glm::quat baseRotation;
        glm::vec3 baseSkew;
        glm::vec4 basePerspective;
        glm::decompose(basePose[i], baseScale, baseRotation, baseTranslation, baseSkew, basePerspective);

        glm::vec3 multScale, multTranslation;
        glm::quat multRotation;
        glm::vec3 multSkew;
        glm::vec4 multPerspective;
        glm::decompose(multPose[i], multScale, multRotation, multTranslation, multSkew, multPerspective);

        // Apply multiplicative blending with weight
        // Scale: baseScale * (multScale ^ weight)
        // Position: basePosition * (multPosition ^ weight)  
        // Rotation: baseRotation * (multRotation ^ weight)
        
        glm::vec3 finalScale;
        for (int j = 0; j < 3; j++) {
            finalScale[j] = baseScale[j] * std::pow(multScale[j], weight);
        }

        glm::vec3 finalTranslation;
        for (int j = 0; j < 3; j++) {
            finalTranslation[j] = baseTranslation[j] * std::pow(std::abs(multTranslation[j]), weight) * 
                                  (multTranslation[j] >= 0 ? 1 : -1);
        }

        // For rotation, use power of quaternion
        // q^t = exp(t * log(q)), but we'll use slerp for stability
        // glm::pow for quaternions requires GTC/quaternion header
        glm::quat identityQuat = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::quat poweredRot = glm::slerp(identityQuat, multRotation, weight);
        glm::quat finalRotation = baseRotation * poweredRot;

        // Rebuild final matrix
        glm::mat4 finalMatrix = glm::translate(glm::mat4(1.0f), finalTranslation);
        finalMatrix *= glm::mat4_cast(finalRotation);
        finalMatrix *= glm::scale(glm::mat4(1.0f), finalScale);

        blendedMatrices[i] = finalMatrix;
    }

    if (config.debugMode) {
        std::cout << "[LayerSystem] Applied multiplicative layer: " << layer.name
                  << " weight=" << weight << "\n";
    }
}

// ============================================================================
// PROJECTION BLENDING
// ============================================================================
// FinalPose = BasePose + ProjectedOffset * weight
// Used for: Aim offsets, look-at adjustments, localized pose modifications
void AnimationLayerSystem::ApplyProjectionLayer(Animator* animator, const AnimationLayer& layer, float weight) {
    if (!animator || !layer.animation) return;

    // Get bone mask for this layer
    std::vector<bool> boneMask = GetLayerBoneMask(layer);

    // Get the current base pose from animator
    const std::vector<glm::mat4>& basePose = animator->GetFinalBoneMatrices();

    // Create reverse bone mapping (index -> name)
    std::vector<std::string> boneNames(skeleton->bones.size());
    for (const auto& [name, index] : skeleton->boneMapping) {
        if (index >= 0 && static_cast<size_t>(index) < skeleton->bones.size()) {
            boneNames[index] = name;
        }
    }

    // Sample the projection animation at current layer time
    std::vector<glm::mat4> projPose(skeleton->bones.size(), glm::mat4(1.0f));

    for (size_t boneIndex = 0; boneIndex < skeleton->bones.size(); boneIndex++) {
        if (!boneMask[boneIndex]) {
            projPose[boneIndex] = skeleton->bones[boneIndex].bindTransform;
            continue;
        }

        const std::string& boneName = boneNames[boneIndex];

        // Find bone animation track
        auto boneAnimIt = layer.animation->boneAnimations.find(boneName);
        if (boneAnimIt == layer.animation->boneAnimations.end()) {
            // No animation for this bone, use bind pose
            projPose[boneIndex] = skeleton->bones[boneIndex].bindTransform;
            continue;
        }

        const BoneAnimation& boneAnim = boneAnimIt->second;

        // Interpolate transforms at current layer time
        float timeToUse = fmod(layer.time, layer.animation->duration);
        if (timeToUse < 0) timeToUse += layer.animation->duration;

        glm::vec3 position = boneAnim.InterpolatePosition(timeToUse);
        glm::quat rotation = boneAnim.InterpolateRotation(timeToUse);
        glm::vec3 scale = boneAnim.InterpolateScale(timeToUse);

        // Build transformation matrix
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), position);
        transform *= glm::mat4_cast(rotation);
        transform *= glm::scale(glm::mat4(1.0f), scale);

        projPose[boneIndex] = transform;
    }

    // Apply projection blending
    // This blends only the "offset" from the reference pose
    for (size_t i = 0; i < skeleton->bones.size(); i++) {
        if (!boneMask[i]) {
            blendedMatrices[i] = basePose[i];
            continue;
        }

        // Get reference pose (bind pose)
        glm::mat4 referencePose = skeleton->bones[i].bindTransform;

        // Decompose matrices to TRS
        glm::vec3 baseScale, baseTranslation;
        glm::quat baseRotation;
        glm::vec3 baseSkew;
        glm::vec4 basePerspective;
        glm::decompose(basePose[i], baseScale, baseRotation, baseTranslation, baseSkew, basePerspective);

        glm::vec3 projScale, projTranslation;
        glm::quat projRotation;
        glm::vec3 projSkew;
        glm::vec4 projPerspective;
        glm::decompose(projPose[i], projScale, projRotation, projTranslation, projSkew, projPerspective);

        glm::vec3 refScale, refTranslation;
        glm::quat refRotation;
        glm::vec3 refSkew;
        glm::vec4 refPerspective;
        glm::decompose(referencePose, refScale, refRotation, refTranslation, refSkew, refPerspective);

        // Calculate projection offset (difference from reference)
        glm::vec3 translationOffset = projTranslation - refTranslation;
        glm::quat rotationOffset = projRotation * glm::inverse(refRotation);
        
        // For scale, calculate ratio
        glm::vec3 scaleOffset;
        for (int j = 0; j < 3; j++) {
            scaleOffset[j] = (refScale[j] != 0.0f) ? (projScale[j] / refScale[j] - 1.0f) : 0.0f;
        }

        // Apply weighted projection offset
        // The key difference from additive: projection uses the base pose direction
        glm::vec3 finalTranslation = baseTranslation + translationOffset * weight;
        glm::quat finalRotation = glm::slerp(baseRotation, baseRotation * rotationOffset, weight);
        glm::vec3 finalScale = baseScale * (glm::vec3(1.0f) + scaleOffset * weight);

        // Rebuild final matrix
        glm::mat4 finalMatrix = glm::translate(glm::mat4(1.0f), finalTranslation);
        finalMatrix *= glm::mat4_cast(finalRotation);
        finalMatrix *= glm::scale(glm::mat4(1.0f), finalScale);

        blendedMatrices[i] = finalMatrix;
    }

    if (config.debugMode) {
        std::cout << "[LayerSystem] Applied projection layer: " << layer.name
                  << " weight=" << weight << "\n";
    }
}

void AnimationLayerSystem::CalculateBoneWeights() {
    // Reset bone weights
    std::fill(boneWeights.begin(), boneWeights.end(), 0.0f);

    // Accumulate weights from all layers
    for (const auto& layer : layers) {
        if (!layer.enabled || layer.weight <= 0.01f) continue;

        std::vector<bool> boneMask = GetLayerBoneMask(layer);

        for (size_t i = 0; i < boneMask.size() && i < boneWeights.size(); i++) {
            if (boneMask[i]) {
                boneWeights[i] = glm::max(boneWeights[i], layer.weight);
            }
        }
    }
}

void AnimationLayerSystem::SortLayersByPriority() {
    std::sort(layers.begin(), layers.end(),
        [](const AnimationLayer& a, const AnimationLayer& b) {
            return a.priority > b.priority;  // Higher priority first
        });
}

std::vector<bool> AnimationLayerSystem::CreateBoneMask(BoneMaskPreset preset) const {
    std::vector<bool> mask;
    if (!skeleton) return mask;

    mask.resize(skeleton->bones.size(), false);

    // Helper lambda to check if bone index matches a pattern
    auto boneMatchesPattern = [this](size_t boneIndex, const std::string& pattern) -> bool {
        // Search boneMapping for this bone index
        for (const auto& [name, index] : skeleton->boneMapping) {
            if (static_cast<size_t>(index) == boneIndex) {
                return BoneNameMatches(name, pattern);
            }
        }
        return false;
    };

    switch (preset) {
        case BoneMaskPreset::FULL_BODY:
            std::fill(mask.begin(), mask.end(), true);
            break;

        case BoneMaskPreset::UPPER_BODY:
            for (size_t i = 0; i < skeleton->bones.size(); i++) {
                if (boneMatchesPattern(i, "arm") ||
                    boneMatchesPattern(i, "hand") ||
                    boneMatchesPattern(i, "shoulder") ||
                    boneMatchesPattern(i, "clavicle") ||
                    boneMatchesPattern(i, "neck") ||
                    boneMatchesPattern(i, "head") ||
                    boneMatchesPattern(i, "spine")) {
                    mask[i] = true;
                }
            }
            break;

        case BoneMaskPreset::LOWER_BODY:
            for (size_t i = 0; i < skeleton->bones.size(); i++) {
                if (boneMatchesPattern(i, "leg") ||
                    boneMatchesPattern(i, "foot") ||
                    boneMatchesPattern(i, "toe") ||
                    boneMatchesPattern(i, "hip")) {
                    mask[i] = true;
                }
            }
            break;

        case BoneMaskPreset::LEFT_ARM:
            for (size_t i = 0; i < skeleton->bones.size(); i++) {
                if (boneMatchesPattern(i, "larm") ||
                    boneMatchesPattern(i, "lshoulder") ||
                    boneMatchesPattern(i, "lhand") ||
                    boneMatchesPattern(i, "lforearm")) {
                    mask[i] = true;
                }
            }
            break;

        case BoneMaskPreset::RIGHT_ARM:
            for (size_t i = 0; i < skeleton->bones.size(); i++) {
                if (boneMatchesPattern(i, "rarm") ||
                    boneMatchesPattern(i, "rshoulder") ||
                    boneMatchesPattern(i, "rhand") ||
                    boneMatchesPattern(i, "rforearm")) {
                    mask[i] = true;
                }
            }
            break;

        case BoneMaskPreset::HEAD_ONLY:
            for (size_t i = 0; i < skeleton->bones.size(); i++) {
                if (boneMatchesPattern(i, "head") ||
                    boneMatchesPattern(i, "neck")) {
                    mask[i] = true;
                }
            }
            break;

        case BoneMaskPreset::ARMS_ONLY:
            for (size_t i = 0; i < skeleton->bones.size(); i++) {
                if (boneMatchesPattern(i, "arm") ||
                    boneMatchesPattern(i, "hand") ||
                    boneMatchesPattern(i, "shoulder") ||
                    boneMatchesPattern(i, "clavicle") ||
                    boneMatchesPattern(i, "forearm")) {
                    mask[i] = true;
                }
            }
            break;

        case BoneMaskPreset::CUSTOM:
            // Should use CreateCustomMask instead
            break;
    }

    return mask;
}

std::vector<bool> AnimationLayerSystem::CreateCustomMask(const std::vector<std::string>& boneNames) const {
    std::vector<bool> mask;
    if (!skeleton) return mask;

    mask.resize(skeleton->bones.size(), false);

    for (const auto& name : boneNames) {
        // Look up bone index by name using skeleton's boneMapping
        int boneIndex = skeleton->GetBoneIndex(name);
        if (boneIndex >= 0 && static_cast<size_t>(boneIndex) < skeleton->bones.size()) {
            mask[boneIndex] = true;
        }
    }

    return mask;
}

std::vector<bool> AnimationLayerSystem::CreateCustomMask(const std::vector<int>& boneIndices) const {
    std::vector<bool> mask;
    if (!skeleton) return mask;

    mask.resize(skeleton->bones.size(), false);

    for (int index : boneIndices) {
        if (index >= 0 && index < static_cast<int>(skeleton->bones.size())) {
            mask[index] = true;
        }
    }

    return mask;
}

std::vector<bool> AnimationLayerSystem::GetLayerBoneMask(const AnimationLayer& layer) const {
    switch (layer.maskPreset) {
        case BoneMaskPreset::FULL_BODY:
            return CreateBoneMask(BoneMaskPreset::FULL_BODY);
        case BoneMaskPreset::UPPER_BODY:
            return CreateBoneMask(BoneMaskPreset::UPPER_BODY);
        case BoneMaskPreset::LOWER_BODY:
            return CreateBoneMask(BoneMaskPreset::LOWER_BODY);
        case BoneMaskPreset::LEFT_ARM:
            return CreateBoneMask(BoneMaskPreset::LEFT_ARM);
        case BoneMaskPreset::RIGHT_ARM:
            return CreateBoneMask(BoneMaskPreset::RIGHT_ARM);
        case BoneMaskPreset::HEAD_ONLY:
            return CreateBoneMask(BoneMaskPreset::HEAD_ONLY);
        case BoneMaskPreset::ARMS_ONLY:
            return CreateBoneMask(BoneMaskPreset::ARMS_ONLY);
        case BoneMaskPreset::CUSTOM:
            return layer.customBoneMask;
        default:
            return CreateBoneMask(BoneMaskPreset::FULL_BODY);
    }
}

std::string AnimationLayerSystem::GetDebugInfo() const {
    std::string info = "Animation Layer System\n";
    info += "========================\n";
    info += "Initialized: " + std::string(initialized ? "YES" : "NO") + "\n";
    info += "Layers: " + std::to_string(layers.size()) + " / " + std::to_string(config.maxLayers) + "\n";
    info += "Bones: " + std::to_string(skeleton ? skeleton->bones.size() : 0) + "\n";
    info += "Per-Bone Blending: " + std::string(config.enablePerBoneBlending ? "YES" : "NO") + "\n";
    info += "Debug Mode: " + std::string(config.debugMode ? "YES" : "NO") + "\n";
    info += "\nActive Layers:\n";

    for (const auto& layer : layers) {
        info += "  - " + layer.name + "\n";
        info += "    Weight: " + std::to_string(layer.weight) + " (target: " + std::to_string(layer.targetWeight) + ")\n";
        info += "    Time: " + std::to_string(layer.time) + " / " + std::to_string(layer.animation ? layer.animation->duration : 0) + "\n";
        info += "    Blend Mode: " + std::to_string((int)layer.blendMode) + "\n";
        info += "    Priority: " + std::to_string(layer.priority) + "\n";
    }

    return info;
}

void AnimationLayerSystem::PrintDebugInfo() const {
    std::cout << "\n" << GetDebugInfo() << std::endl;
}

std::string AnimationLayerSystem::NormalizeBoneName(const std::string& name) {
    std::string result = name;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return result;
}

bool AnimationLayerSystem::BoneNameMatches(const std::string& boneName, const std::string& pattern) {
    std::string lowerBone = NormalizeBoneName(boneName);
    std::string lowerPattern = NormalizeBoneName(pattern);

    return lowerBone.find(lowerPattern) != std::string::npos;
}
