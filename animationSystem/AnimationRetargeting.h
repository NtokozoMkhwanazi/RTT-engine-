#pragma once
#include "Animation.h"
#include "../boneSystem/Skeleton.h"
#include <glm/glm.hpp>
#include <vector>
#include <map>
#include <string>

class AnimationRetargeting {
public:
    // Retarget animation from source skeleton to target skeleton
    static Animation RetargetAnimation(
        const Animation& sourceAnim,
        const Skeleton& sourceSkel,
        const Skeleton& targetSkel,
        float scale = 1.0f
    );

    // Calculate bone mapping between different skeletons
    static std::map<std::string, std::string> GenerateBoneMapping(
        const Skeleton& sourceSkel,
        const Skeleton& targetSkel
    );

    // Enhanced bone mapping for Mixamo compatibility
    static std::map<std::string, std::string> GenerateMixamoBoneMapping(
        const Skeleton& sourceSkel,
        const Skeleton& targetSkel
    );

    // Adjust animation scale for different character sizes
    static void ScaleAnimation(Animation& anim, float scaleFactor);

    // Adjust animation timing to match different character speeds
    static void AdjustTiming(Animation& anim, float timeMultiplier);

    // Fix T-pose to A-pose conversion for Mixamo animations
    static Animation FixTPoseToAPose(const Animation& sourceAnim, const Skeleton& skeleton);

    // Apply corrective transforms for common Mixamo issues
    static void ApplyCorrectiveTransforms(Animation& anim, const Skeleton& skeleton);

    // Normalize animation to standard pose
    static Animation NormalizeToStandardPose(const Animation& sourceAnim, const Skeleton& skeleton);

    // Automatic rig detection
    enum class RigType {
        MIXAMO_RIG,
        EPIC_MANNEQUIN,
        GENERIC_HUMAN,
        UNKNOWN
    };

    // Detect the type of rig based on bone structure
    static RigType DetectRigType(const Skeleton& skeleton);

    // Detect rig type from animation
    static RigType DetectRigTypeFromAnimation(const Animation& animation);

    // Get standard bone mapping for a specific rig type
    static std::map<std::string, std::string> GetStandardRigMapping(RigType rigType);

    // Validate skeleton structure for known rig types
    static bool ValidateSkeletonStructure(const Skeleton& skeleton, RigType expectedType);

private:
    // Helper function to map a bone transformation from source to target
    static glm::mat4 MapBoneTransform(
        const glm::mat4& sourceTransform,
        const std::string& sourceBoneName,
        const std::string& targetBoneName,
        const Skeleton& sourceSkel,
        const Skeleton& targetSkel,
        float scale
    );

    // Generate common Mixamo bone name variations
    static std::vector<std::string> GetMixamoBoneVariants(const std::string& boneName);

    // Find best matching bone in skeleton
    static std::string FindBestBoneMatch(const std::string& boneName, const Skeleton& skeleton);

    // Enhanced bone mapping with fuzzy matching
    static std::map<std::string, std::string> GenerateEnhancedBoneMapping(
        const Skeleton& sourceSkel,
        const Skeleton& targetSkel
    );

    // Calculate bone similarity score for better matching
    static float CalculateBoneSimilarity(const std::string& bone1, const std::string& bone2);

    // Get all possible bone name variations for matching
    static std::vector<std::string> GetAllBoneNameVariations(const std::string& boneName);
};