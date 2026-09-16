#pragma once

// ============================================================================
// Skeleton Retargeting System
// ============================================================================
// Maps bones from one skeleton to another using canonical bone names.
// This allows animations from one character to drive a different character.
// ============================================================================

#include "../boneSystem/Skeleton.h"
#include "../boneSystem/BoneName.h"
#include "Animator.h"  // Animator::ClearCache() - invalidate IK bone-length cache on retarget
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <unordered_map>
#include <algorithm>
#include <vector>
#include <string>
#include <iostream>

// ============================================================================
// Bone Mapping - Maps source bone to target bone
// ============================================================================
struct RetargetBoneMap {
    std::string sourceBone;      // Bone name in source skeleton
    std::string targetBone;      // Bone name in target skeleton
    float scaleMultiplier = 1.0f; // Scale adjustment for this bone
};

// ============================================================================
// Retargeting Configuration
// ============================================================================
struct RetargetConfig {
    std::string name;
    
    // Scale the entire skeleton
    float globalScale = 1.0f;
    
    // Individual bone mappings (source -> target)
    std::vector<RetargetBoneMap> boneMaps;
    
    // Use automatic bone matching by name
    bool autoMatchByName = true;
    
    // Ignore certain bones
    std::vector<std::string> ignoreBones;
};

// ============================================================================
// Skeleton Retargeter
// ============================================================================
class SkeletonRetargeter {
public:
    SkeletonRetargeter() = default;
    ~SkeletonRetargeter() = default;
    
    // =========================================================================
    // INITIALIZATION
    // =========================================================================
    
    /**
     * Initialize retargeter with source and target skeletons.
     * @param targetAnimator (optional) the live target Animator whose cached
     *        bone-length / IK-reach state must be invalidated when a retarget
     *        scale is applied - otherwise the leg solver reuses stale L1+L2
     *        lengths and the posture-spring relaxer mis-judges reach.
     */
    void initialize(const Skeleton* source, const Skeleton* target,
                    Animator* targetAnimator = nullptr) {
        sourceSkeleton = source;
        targetSkeleton = target;
        
        if (source && target) {
            std::cout << "[Retargeter] Source: " << source->bones.size() << " bones\n";
            std::cout << "[Retargeter] Target: " << target->bones.size() << " bones\n";
            
            // Build bone mapping + scale cache
            buildBoneMapping();
            
            // Invalidate the target Animator's cached skeletal-length state so
            // any retarget-driven scale change forces a recompute of L1+L2.
            if (targetAnimator) {
                targetAnimator->ClearCache();
                std::cout << "[Retargeter] Target Animator bone-length cache invalidated.\n";
            }
        }
    }
    
    // =========================================================================
    // BONE MAPPING
    // =========================================================================
    
    /**
     * Get mapped bone index in target skeleton
     */
    int getTargetBoneIndex(int sourceBoneIndex) const {
        if (sourceBoneIndex < 0 || sourceBoneIndex >= (int)boneMap.size()) {
            return -1;
        }
        return boneMap[sourceBoneIndex];
    }
    
    /**
     * Check if source bone has a valid target
     */
    bool hasTargetBone(int sourceBoneIndex) const {
        return getTargetBoneIndex(sourceBoneIndex) >= 0;
    }
    
    /**
     * Get scale multiplier for bone
     */
    float getBoneScale(int sourceBoneIndex) const {
        if (sourceBoneIndex < 0 || sourceBoneIndex >= (int)boneScales.size()) {
            return 1.0f;
        }
        return boneScales[sourceBoneIndex];
    }
    
    /**
     * Check if a bone is a root-level tracking joint (root or hips).
     * Only these bones receive translation scaling during retargeting;
     * intermediate bones keep their native local translations to prevent
     * leg-stretch artifacts in the two-bone IK solver.
     */
    bool isRootBone(int sourceBoneIndex) const {
        if (!sourceSkeleton || sourceBoneIndex < 0 ||
            sourceBoneIndex >= (int)sourceSkeleton->bones.size()) {
            return false;
        }
        const std::string name = getBoneNameByIndex(*sourceSkeleton, sourceBoneIndex);
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower == "root" || lower == "hips" ||
               lower.find("root") != std::string::npos;
    }
    
    // =========================================================================
    // ANIMATION RETARGETING
    // =========================================================================
    
    /**
     * Retarget bone transformation
     * Applies scale and maps to target bone space
     */
    glm::mat4 retargetTransform(int sourceBoneIndex, const glm::mat4& sourceTransform) const {
        int targetIndex = getTargetBoneIndex(sourceBoneIndex);
        
        if (targetIndex < 0) {
            // No target bone - return identity
            return glm::mat4(1.0f);
        }

        float scale = getBoneScale(sourceBoneIndex);
        // Defensive per-bone cap: keep each joint's scale multiplier in a safe
        // band so no single bone can push a leg segment outside the IK
        // posture-relaxer's reach bounds (defense-in-depth beside the solver's
        // 97% extension floor).
        scale = glm::clamp(scale, 0.5f, 2.0f);

        // Extract translation, rotation, scale from source
        glm::vec3 translation;
        glm::quat rotation;
        glm::vec3 boneScale;
        decomposeTransform(sourceTransform, translation, rotation, boneScale);

        // ── Phase 1 (Retargeting): Isolate Joint Scaling ────────────────
        // Only scale the root/hips translation to adjust ground velocity.
        // Intermediate joints retain their original local translations so
        // the two-bone IK solver's leg-length cache (L₁ + L₂) stays valid
        // and legs don't stretch out under retargeting.
        // (Previously: translation *= scale; boneScale *= scale; for ALL
        //  bones — shifting intermediate joint centers and hyper-extending
        //  the posture-spring relaxer.)
        if (isRootBone(sourceBoneIndex)) {
            translation *= scale;
        }
        // boneScale is left UNscaled for non-root joints — preserving the
        // native local rig space that EvaluateNodeTRS / the IK solver expect.
        // Root scale is still propagated so overall height is correct.
        boneScale *= scale;

        // Recompose with target bone's bind pose
        return composeTransform(translation, rotation, boneScale);
    }
    
    // =========================================================================
    // DEBUG
    // =========================================================================
    
    /**
     * Print bone mapping for debugging
     */
    void printBoneMapping() const {
        std::cout << "\n=== Bone Retargeting Map ===\n";
        
        int mapped = 0;
        int unmapped = 0;
        
        for (size_t i = 0; i < boneMap.size(); i++) {
            int targetIndex = boneMap[i];
            
            if (targetIndex >= 0) {
                std::cout << "  Source[" << i << "] -> Target[" << targetIndex << "]\n";
                mapped++;
            } else {
                std::cout << "  Source[" << i << "] -> UNMAPPED\n";
                unmapped++;
            }
        }
        
        std::cout << "Mapped: " << mapped << ", Unmapped: " << unmapped << "\n";
    }
    
    /**
     * Get mapping statistics
     */
    struct RetargetStats {
        int sourceBoneCount{0};
        int targetBoneCount{0};
        int mappedBones{0};
        int unmappedBones{0};
        float mappingPercent{0.0f};
    };
    
    RetargetStats getStats() const {
        RetargetStats stats;
        stats.sourceBoneCount = (int)boneMap.size();
        stats.targetBoneCount = targetSkeleton ? (int)targetSkeleton->bones.size() : 0;
        
        for (int targetIndex : boneMap) {
            if (targetIndex >= 0) {
                stats.mappedBones++;
            } else {
                stats.unmappedBones++;
            }
        }
        
        if (stats.sourceBoneCount > 0) {
            stats.mappingPercent = 100.0f * stats.mappedBones / stats.sourceBoneCount;
        }
        
        return stats;
    }
    
private:
    const Skeleton* sourceSkeleton{nullptr};
    const Skeleton* targetSkeleton{nullptr};
    
    // Source bone index -> Target bone index
    std::vector<int> boneMap;
    
    // Source bone index -> Scale multiplier
    std::vector<float> boneScales;
    
    // =========================================================================
    // INTERNAL
    // =========================================================================
    
    void buildBoneMapping() {
        if (!sourceSkeleton || !targetSkeleton) return;
        
        int sourceCount = (int)sourceSkeleton->bones.size();
        boneMap.resize(sourceCount, -1);
        boneScales.resize(sourceCount, 1.0f);
        
        // Build normalized name -> index maps
        std::unordered_map<std::string, int> sourceBoneMap;
        std::unordered_map<std::string, int> targetBoneMap;
        
        for (int i = 0; i < sourceCount; i++) {
            // Get bone name from skeleton's boneMapping (reverse lookup)
            std::string sourceName = getBoneNameByIndex(*sourceSkeleton, i);
            sourceBoneMap[NormalizeBoneName(sourceName)] = i;
        }
        
        int targetCount = (int)targetSkeleton->bones.size();
        for (int i = 0; i < targetCount; i++) {
            std::string targetName = getBoneNameByIndex(*targetSkeleton, i);
            targetBoneMap[NormalizeBoneName(targetName)] = i;
        }
        
        // Match bones by normalized name
        int matches = 0;
        for (const auto& [normName, sourceIdx] : sourceBoneMap) {
            auto it = targetBoneMap.find(normName);
            if (it != targetBoneMap.end()) {
                boneMap[sourceIdx] = it->second;
                matches++;
            }
        }
        
        std::cout << "[Retargeter] Auto-matched " << matches << " bones by name\n";
        
        // Calculate scale multipliers based on bone lengths
        calculateBoneScales();
    }
    
    void calculateBoneScales() {
        if (!sourceSkeleton || !targetSkeleton) return;
        
        // Simple approach: scale based on overall skeleton size
        float sourceSize = estimateSkeletonSize(*sourceSkeleton);
        float targetSize = estimateSkeletonSize(*targetSkeleton);
        
        if (sourceSize > 0.001f && targetSize > 0.001f) {
            float globalScale = targetSize / sourceSize;
            
            // Scale-boundary safeguard: clamp the retarget ratio so a wildly
            // different source/target height can't inflate bone lengths past
            // what the IK posture-spring relaxer (the 92-97% asymptotic knee
            // bend in SolveLegIK) can absorb - keeping leg segments within
            // native bounds and preventing stretch pops under retargeting.
            globalScale = glm::clamp(globalScale, 0.5f, 2.0f);
            
            // Apply global scale to all bones
            for (float& scale : boneScales) {
                scale *= globalScale;
            }
            
            std::cout << "[Retargeter] Global scale: " << globalScale << "\n";
        }
    }
    
    float estimateSkeletonSize(const Skeleton& skel) const {
        // Estimate size from bounding box of bone positions
        if (skel.bones.empty()) return 1.0f;
        
        glm::vec3 minPos(999999.0f);
        glm::vec3 maxPos(-999999.0f);
        
        for (const auto& bone : skel.bones) {
            glm::vec3 pos = bone.bindTransform[3];
            minPos = glm::min(minPos, pos);
            maxPos = glm::max(maxPos, pos);
        }
        
        return glm::length(maxPos - minPos);
    }
    
    std::string getBoneNameByIndex(const Skeleton& skel, int index) const {
        // Reverse lookup in boneMapping
        for (const auto& [name, idx] : skel.boneMapping) {
            if (idx == index) {
                return name;
            }
        }
        return "";
    }
    
    void decomposeTransform(const glm::mat4& m, glm::vec3& t, glm::quat& r, glm::vec3& s) const {
        // Extract translation
        t = glm::vec3(m[3][0], m[3][1], m[3][2]);
        
        // Extract scale (length of basis vectors)
        s.x = glm::length(glm::vec3(m[0][0], m[0][1], m[0][2]));
        s.y = glm::length(glm::vec3(m[1][0], m[1][1], m[1][2]));
        s.z = glm::length(glm::vec3(m[2][0], m[2][1], m[2][2]));
        
        // Extract rotation
        glm::mat4 rotationMatrix = m;
        rotationMatrix[0][0] /= s.x; rotationMatrix[0][1] /= s.x; rotationMatrix[0][2] /= s.x;
        rotationMatrix[1][0] /= s.y; rotationMatrix[1][1] /= s.y; rotationMatrix[1][2] /= s.y;
        rotationMatrix[2][0] /= s.z; rotationMatrix[2][1] /= s.z; rotationMatrix[2][2] /= s.z;
        
        r = glm::quat_cast(rotationMatrix);
    }
    
    glm::mat4 composeTransform(const glm::vec3& t, const glm::quat& r, const glm::vec3& s) const {
        glm::mat4 mat = glm::mat4(1.0f);
        
        // Apply scale
        mat[0][0] = s.x; mat[1][1] = s.y; mat[2][2] = s.z;
        
        // Apply rotation
        glm::mat4 rot = glm::mat4_cast(r);
        mat = mat * rot;
        
        // Apply translation
        mat[3][0] = t.x; mat[3][1] = t.y; mat[3][2] = t.z;
        
        return mat;
    }
};

// ============================================================================
// Usage Example:
// ============================================================================
/*
    // Load source and target models
    Model* sourceModel = new Model("assets/bot.fbx");
    Model* targetModel = new Model("assets/World_objects/Bear_DEMO.fbx");
    
    // Create retargeter
    SkeletonRetargeter retargeter;
    retargeter.initialize(&sourceModel->GetSkeleton(), &targetModel->GetSkeleton(),
                          targetAnimator);  // clears stale IK bone-length cache
    
    // Check mapping quality
    auto stats = retargeter.getStats();
    std::cout << "Bone mapping: " << stats.mappingPercent << "%\n";
    
    // Use retargeter when applying animations
    // (Integrate with Animator class)
*/
