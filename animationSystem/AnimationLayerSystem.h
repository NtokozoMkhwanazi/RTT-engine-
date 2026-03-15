#pragma once
#include "Animation.h"
#include "Animator.h"
#include "../boneSystem/Skeleton.h"
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <glm/glm.hpp>

// ============================================================================
// ANIMATION LAYER SYSTEM
// ============================================================================
//
// Advanced layered animation blending for hybrid MM+FSM system.
// Supports:
// - Multiple animation layers with independent weights
// - Bone masks for partial-body blending
// - Additive and linear blending modes
// - Layer priorities and fading
//
// Use Cases:
// - Upper-body attacks over MM locomotion
// - Aim offsets (additive)
// - Reload gestures over walking
// - Head tracking while moving
// ============================================================================

/**
 * Layer Blend Mode
 */
enum class LayerBlendMode {
    LINEAR,         // Standard linear blending (A * weight + B * (1-weight))
    ADDITIVE,       // Additive blending (Base + Additive * weight)
    MULTIPLICATIVE, // Multiplicative blending (Base * Additive * weight)
    PROJECTION      // Projection blending (for aim offsets)
};

/**
 * Layer Bone Mask Presets
 */
enum class BoneMaskPreset {
    FULL_BODY,
    UPPER_BODY,     // Arms, shoulders, head, spine (above waist)
    LOWER_BODY,     // Legs, hips, spine (below waist)
    LEFT_ARM,
    RIGHT_ARM,
    HEAD_ONLY,
    ARMS_ONLY,
    CUSTOM
};

/**
 * Animation Layer Configuration
 */
struct AnimationLayer {
    std::string name;
    std::shared_ptr<Animation> animation;
    LayerBlendMode blendMode = LayerBlendMode::LINEAR;
    BoneMaskPreset maskPreset = BoneMaskPreset::FULL_BODY;
    std::vector<bool> customBoneMask;  // For CUSTOM preset
    float weight = 1.0f;
    float targetWeight = 1.0f;
    float blendInDuration = 0.1f;
    float blendOutDuration = 0.1f;
    float time = 0.0f;
    float speedMultiplier = 1.0f;
    bool loop = true;
    bool enabled = true;
    int priority = 0;  // Higher priority layers override lower ones

    // For additive layers
    glm::vec3 additiveOffset{0.0f};
    float additiveScale = 1.0f;

    // Callbacks
    std::function<void()> onLayerStart;
    std::function<void()> onLayerComplete;
    std::function<void(float)> onLayerUpdate;  // Called each frame with weight

    bool IsValid() const { return animation != nullptr; }
    bool IsFinished() const {
        if (!animation) return true;
        if (loop) return false;
        return time >= animation->duration;
    }
};

/**
 * Layer Blending Result
 */
struct LayerBlendResult {
    std::vector<glm::mat4> blendedBoneMatrices;
    std::vector<float> boneWeights;  // Per-bone blend weight
    bool success = false;
};

/**
 * Animation Layer System Configuration
 */
struct LayerSystemConfig {
    size_t maxLayers = 8;
    size_t maxBones = 120;
    float defaultBlendDuration = 0.15f;
    bool enablePerBoneBlending = true;
    bool enablePriorityBlending = true;
    bool debugMode = false;
};

/**
 * Animation Layer System - Main Class
 *
 * Usage:
 *   LayerSystem layerSystem;
 *   layerSystem.Initialize(skeleton, config);
 *
 *   // Add layers
 *   layerSystem.AddLayer("Attack", attackAnim, LayerBlendMode::LINEAR, BoneMaskPreset::UPPER_BODY);
 *   layerSystem.AddLayer("AimOffset", aimAnim, LayerBlendMode::ADDITIVE, BoneMaskPreset::ARMS_ONLY);
 *
 *   // Update weights
 *   layerSystem.SetLayerWeight("Attack", 1.0f, 0.1f);
 *
 *   // Each frame:
 *   layerSystem.Update(dt);
 *   layerSystem.ApplyToAnimator(animator);
 */
class AnimationLayerSystem {
public:
    AnimationLayerSystem();
    ~AnimationLayerSystem();

    // =========================================================================
    // INITIALIZATION
    // =========================================================================

    /**
     * Initialize layer system
     *
     * @param skeleton Character skeleton
     * @param config System configuration
     */
    void Initialize(const Skeleton* skeleton, const LayerSystemConfig& config = LayerSystemConfig());

    /**
     * Check if initialized
     */
    bool IsInitialized() const { return initialized; }

    // =========================================================================
    // LAYER MANAGEMENT
    // =========================================================================

    /**
     * Add animation layer
     *
     * @param name Layer name (unique identifier)
     * @param anim Animation to play
     * @param blendMode How to blend (Linear, Additive, etc.)
     * @param maskPreset Which bones to affect
     * @param initialWeight Starting weight (0-1)
     * @return true if successful
     */
    bool AddLayer(const std::string& name, std::shared_ptr<Animation> anim,
                  LayerBlendMode blendMode = LayerBlendMode::LINEAR,
                  BoneMaskPreset maskPreset = BoneMaskPreset::FULL_BODY,
                  float initialWeight = 1.0f);

    /**
     * Add layer with custom bone mask
     *
     * @param name Layer name
     * @param anim Animation to play
     * @param blendMode Blend mode
     * @param boneMask Per-bone mask (true = affected)
     * @param initialWeight Starting weight
     * @return true if successful
     */
    bool AddLayerWithCustomMask(const std::string& name, std::shared_ptr<Animation> anim,
                                LayerBlendMode blendMode,
                                const std::vector<bool>& boneMask,
                                float initialWeight = 1.0f);

    /**
     * Remove layer
     *
     * @param name Layer name
     * @param fadeOutDuration Fade out time (0 = instant)
     */
    void RemoveLayer(const std::string& name, float fadeOutDuration = 0.2f);

    /**
     * Get layer by name
     */
    AnimationLayer* GetLayer(const std::string& name);
    const AnimationLayer* GetLayer(const std::string& name) const;

    /**
     * Check if layer exists
     */
    bool HasLayer(const std::string& name) const;

    /**
     * Get number of active layers
     */
    size_t GetLayerCount() const { return layers.size(); }

    // =========================================================================
    // WEIGHT CONTROL
    // =========================================================================

    /**
     * Set layer weight
     *
     * @param name Layer name
     * @param weight Target weight (0-1)
     * @param duration Blend duration (0 = instant)
     */
    void SetLayerWeight(const std::string& name, float weight, float duration = 0.1f);

    /**
     * Set all layer weights
     *
     * @param weights Map of layer name -> weight
     */
    void SetAllLayerWeights(const std::unordered_map<std::string, float>& weights);

    /**
     * Fade layer in
     */
    void FadeInLayer(const std::string& name, float duration = 0.2f);

    /**
     * Fade layer out
     */
    void FadeOutLayer(const std::string& name, float duration = 0.2f);

    // =========================================================================
    // TIME CONTROL
    // =========================================================================

    /**
     * Set layer time
     *
     * @param name Layer name
     * @param time Time in animation
     */
    void SetLayerTime(const std::string& name, float time);

    /**
     * Set layer speed
     */
    void SetLayerSpeed(const std::string& name, float speedMultiplier);

    /**
     * Set layer looping
     */
    void SetLayerLooping(const std::string& name, bool loop);

    // =========================================================================
    // UPDATE & APPLY
    // =========================================================================

    /**
     * Update all layers
     *
     * Call every frame.
     *
     * @param dt Delta time
     */
    void Update(float dt);

    /**
     * Apply layers to animator
     *
     * Call after Update() to apply blended result.
     *
     * @param animator Target animator
     * @param baseWeight Base layer weight (usually 1.0)
     */
    void ApplyToAnimator(Animator* animator, float baseWeight = 1.0f);

    /**
     * Get blended bone matrices
     *
     * For manual blending without animator.
     */
    const std::vector<glm::mat4>& GetBlendedMatrices() const { return blendedMatrices; }

    // =========================================================================
    // BONE MASK HELPERS
    // =========================================================================

    /**
     * Create bone mask from preset
     */
    std::vector<bool> CreateBoneMask(BoneMaskPreset preset) const;

    /**
     * Create custom bone mask from bone names
     *
     * @param boneNames Names of bones to include
     */
    std::vector<bool> CreateCustomMask(const std::vector<std::string>& boneNames) const;

    /**
     * Create custom bone mask from bone indices
     */
    std::vector<bool> CreateCustomMask(const std::vector<int>& boneIndices) const;

    /**
     * Get bone mask for layer
     */
    std::vector<bool> GetLayerBoneMask(const AnimationLayer& layer) const;

    // =========================================================================
    // DEBUG
    // =========================================================================

    /**
     * Get debug info string
     */
    std::string GetDebugInfo() const;

    /**
     * Print debug info
     */
    void PrintDebugInfo() const;

    /**
     * Enable/disable debug mode
     */
    void SetDebugEnabled(bool enabled) { config.debugMode = enabled; }

private:
    const Skeleton* skeleton{nullptr};
    LayerSystemConfig config;
    bool initialized{false};

    std::vector<AnimationLayer> layers;
    std::vector<glm::mat4> blendedMatrices;
    std::vector<float> boneWeights;

    // Internal helpers
    void UpdateLayer(AnimationLayer& layer, float dt);
    void ApplyLayerToAnimator(Animator* animator, const AnimationLayer& layer, float weight);
    void ApplyAdditiveLayer(Animator* animator, const AnimationLayer& layer, float weight);
    void ApplyMultiplicativeLayer(Animator* animator, const AnimationLayer& layer, float weight);
    void ApplyProjectionLayer(Animator* animator, const AnimationLayer& layer, float weight);
    void CalculateBoneWeights();
    void SortLayersByPriority();

    /**
     * Normalize bone name for matching
     */
    static std::string NormalizeBoneName(const std::string& name);

    /**
     * Check if bone name matches pattern
     */
    static bool BoneNameMatches(const std::string& boneName, const std::string& pattern);
};
