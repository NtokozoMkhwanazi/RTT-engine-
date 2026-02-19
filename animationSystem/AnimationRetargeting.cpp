#include "AnimationRetargeting.h"
#include "../boneSystem/BoneName.h"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <algorithm>
#include <cctype>

Animation AnimationRetargeting::RetargetAnimation(
    const Animation &sourceAnim,
    const Skeleton &sourceSkel,
    const Skeleton &targetSkel,
    float scale)
{
    // Create a new animation with the same properties as the source
    Animation retargetedAnim(sourceAnim.name, sourceAnim.duration, sourceAnim.ticksPerSecond);

    // Detect rig types for both skeletons
    RigType sourceRigType = DetectRigType(sourceSkel);
    RigType targetRigType = DetectRigType(targetSkel);

    // Generate bone mapping based on detected rig types
    std::map<std::string, std::string> boneMap;

    if (sourceRigType == targetRigType && sourceRigType != RigType::UNKNOWN)
    {
        // If both skeletons are the same known rig type, use standard mapping
        boneMap = GetStandardRigMapping(sourceRigType);
    }
    else
    {
        // Otherwise, use enhanced fuzzy mapping
        boneMap = GenerateEnhancedBoneMapping(sourceSkel, targetSkel);
    }

    // Process each bone animation in the source
    for (const auto &[sourceBoneName, sourceBoneAnim] : sourceAnim.boneAnimations)
    {
        // Find corresponding bone in target skeleton
        std::string targetBoneName = FindBestBoneMatch(sourceBoneName, targetSkel);

        // If no match found, try the original mapping
        if (targetBoneName.empty())
        {
            // Look up in the bone mapping
            auto it = boneMap.find(NormalizeBoneName(sourceBoneName));
            if (it != boneMap.end())
            {
                targetBoneName = it->second;
            }
            else
            {
                // If no direct mapping found, try to find by normalized name
                std::string normSource = NormalizeBoneName(sourceBoneName);
                if (targetSkel.GetBoneIndex(normSource) != -1)
                {
                    targetBoneName = normSource;
                }
                else
                {
                    continue; // Skip if target bone doesn't exist
                }
            }
        }

        // Create new bone animation for target skeleton
        BoneAnimation targetBoneAnim = sourceBoneAnim; // Copy original data
        targetBoneAnim.boneName = targetBoneName;

        // Scale position data
        for (auto &pos : targetBoneAnim.positionValues)
        {
            pos *= scale;
        }

        // Add to retargeted animation
        retargetedAnim.AddBoneAnimation(targetBoneAnim);
    }

    return retargetedAnim;
}

std::map<std::string, std::string> AnimationRetargeting::GenerateBoneMapping(
    const Skeleton &sourceSkel,
    const Skeleton &targetSkel)
{
    std::map<std::string, std::string> boneMap;

    // Common bone name mappings (handles different naming conventions)
    std::vector<std::pair<std::string, std::string>> commonMappings = {
        {"hips", "pelvis"},
        {"pelvis", "hips"},
        {"spine_01", "spine1"},
        {"spine_02", "spine2"},
        {"spine_03", "spine3"},
        {"neck_01", "neck1"},
        {"head", "head"},
        {"leftarm", "left_arm"},
        {"rightarm", "right_arm"},
        {"leftforearm", "left_forearm"},
        {"rightforearm", "right_forearm"},
        {"lefthand", "left_hand"},
        {"righthand", "right_hand"},
        {"leftupleg", "left_upleg"},
        {"rightupleg", "right_upleg"},
        {"leftleg", "left_leg"},
        {"rightleg", "right_leg"},
        {"leftfoot", "left_foot"},
        {"rightfoot", "right_foot"},
        {"lefttoe", "left_toe"},
        {"righttoe", "right_toe"}};

    // Add common mappings
    for (const auto &[source, target] : commonMappings)
    {
        if (sourceSkel.GetBoneIndex(source) != -1 && targetSkel.GetBoneIndex(target) != -1)
        {
            boneMap[NormalizeBoneName(source)] = NormalizeBoneName(target);
        }
    }

    // Also add direct name matches
    for (const auto &[name, idx] : sourceSkel.boneMapping)
    {
        if (targetSkel.GetBoneIndex(name) != -1)
        {
            boneMap[name] = name;
        }
    }

    return boneMap;
}

std::map<std::string, std::string> AnimationRetargeting::GenerateMixamoBoneMapping(
    const Skeleton &sourceSkel,
    const Skeleton &targetSkel)
{
    std::map<std::string, std::string> boneMap;

    // Mixamo-specific bone name mappings
    std::vector<std::pair<std::string, std::string>> mixamoMappings = {
        // Standard Mixamo to common names
        {"mixamorig:Hips", "hips"},
        {"mixamorig:Spine", "spine"},
        {"mixamorig:Spine1", "spine1"},
        {"mixamorig:Spine2", "spine2"},
        {"mixamorig:Neck", "neck"},
        {"mixamorig:Head", "head"},
        {"mixamorig:LeftShoulder", "left_shoulder"},
        {"mixamorig:LeftArm", "left_arm"},
        {"mixamorig:LeftForeArm", "left_forearm"},
        {"mixamorig:LeftHand", "left_hand"},
        {"mixamorig:RightShoulder", "right_shoulder"},
        {"mixamorig:RightArm", "right_arm"},
        {"mixamorig:RightForeArm", "right_forearm"},
        {"mixamorig:RightHand", "right_hand"},
        {"mixamorig:LeftUpLeg", "left_upleg"},
        {"mixamorig:LeftLeg", "left_leg"},
        {"mixamorig:LeftFoot", "left_foot"},
        {"mixamorig:LeftToeBase", "left_toe"},
        {"mixamorig:RightUpLeg", "right_upleg"},
        {"mixamorig:RightLeg", "right_leg"},
        {"mixamorig:RightFoot", "right_foot"},
        {"mixamorig:RightToeBase", "right_toe"},

        // Variations
        {"Hips", "hips"},
        {"Spine", "spine"},
        {"Spine1", "spine1"},
        {"Spine2", "spine2"},
        {"Neck", "neck"},
        {"Head", "head"},
        {"LeftShoulder", "left_shoulder"},
        {"LeftArm", "left_arm"},
        {"LeftForeArm", "left_forearm"},
        {"LeftHand", "left_hand"},
        {"RightShoulder", "right_shoulder"},
        {"RightArm", "right_arm"},
        {"RightForeArm", "right_forearm"},
        {"RightHand", "right_hand"},
        {"LeftUpLeg", "left_upleg"},
        {"LeftLeg", "left_leg"},
        {"LeftFoot", "left_foot"},
        {"LeftToe_End", "left_toe"},
        {"RightUpLeg", "right_upleg"},
        {"RightLeg", "right_leg"},
        {"RightFoot", "right_foot"},
        {"RightToe_End", "right_toe"},
    };

    // Add Mixamo mappings
    for (const auto &[source, target] : mixamoMappings)
    {
        if (sourceSkel.GetBoneIndex(source) != -1 && targetSkel.GetBoneIndex(target) != -1)
        {
            boneMap[NormalizeBoneName(source)] = NormalizeBoneName(target);
        }
    }

    // Fallback to generic mapping if Mixamo-specific mapping fails
    auto genericMap = GenerateBoneMapping(sourceSkel, targetSkel);
    for (const auto &[source, target] : genericMap)
    {
        if (boneMap.find(source) == boneMap.end())
        {
            boneMap[source] = target;
        }
    }

    return boneMap;
}

void AnimationRetargeting::ScaleAnimation(Animation &anim, float scaleFactor)
{
    for (auto &[boneName, boneAnim] : anim.boneAnimations)
    {
        // Scale position keyframes
        for (auto &pos : boneAnim.positionValues)
        {
            pos *= scaleFactor;
        }
    }
}

void AnimationRetargeting::AdjustTiming(Animation &anim, float timeMultiplier)
{
    // Adjust duration and ticks per second
    anim.duration *= timeMultiplier;
    anim.ticksPerSecond /= timeMultiplier;

    // Adjust all time values in keyframes
    for (auto &[boneName, boneAnim] : anim.boneAnimations)
    {
        // Scale position times
        for (auto &time : boneAnim.positionTimes)
        {
            time *= timeMultiplier;
        }

        // Scale rotation times
        for (auto &time : boneAnim.rotationTimes)
        {
            time *= timeMultiplier;
        }

        // Scale scale times
        for (auto &time : boneAnim.scaleTimes)
        {
            time *= timeMultiplier;
        }
    }
}

Animation AnimationRetargeting::FixTPoseToAPose(const Animation &sourceAnim, const Skeleton &skeleton)
{
    Animation fixedAnim = sourceAnim; // Copy the original animation

    // T-pose to A-pose corrections for arms
    for (auto &[boneName, boneAnim] : fixedAnim.boneAnimations)
    {
        std::string normName = NormalizeBoneName(boneName);

        // Correct arm positions for T-pose to A-pose
        if (normName.find("upperarm") != std::string::npos ||
            normName.find("arm") != std::string::npos)
        {

            // For left arm, rotate slightly inward toward body
            if (normName.find("left") != std::string::npos)
            {
                for (auto &rot : boneAnim.rotationValues)
                {
                    // Apply corrective rotation to bring arms from T-pose to A-pose
                    glm::vec3 euler = glm::eulerAngles(rot);
                    euler.y -= glm::radians(20.0f); // Rotate inward
                    euler.z -= glm::radians(10.0f); // Slight forward rotation
                    rot = glm::quat(euler);
                }
            }
            // For right arm, rotate slightly inward toward body (opposite direction)
            else if (normName.find("right") != std::string::npos)
            {
                for (auto &rot : boneAnim.rotationValues)
                {
                    // Apply corrective rotation to bring arms from T-pose to A-pose
                    glm::vec3 euler = glm::eulerAngles(rot);
                    euler.y += glm::radians(20.0f); // Rotate inward
                    euler.z -= glm::radians(10.0f); // Slight forward rotation
                    rot = glm::quat(euler);
                }
            }
        }
    }

    return fixedAnim;
}

void AnimationRetargeting::ApplyCorrectiveTransforms(Animation &anim, const Skeleton &skeleton)
{
    // Apply corrective transforms to fix common Mixamo issues
    for (auto &[boneName, boneAnim] : anim.boneAnimations)
    {
        std::string normName = NormalizeBoneName(boneName);

        // Apply corrective rotations for common Mixamo issues
        if (normName.find("hand") != std::string::npos)
        {
            // Correct hand rotations that are often off in Mixamo exports
            for (auto &rot : boneAnim.rotationValues)
            {
                glm::vec3 euler = glm::eulerAngles(rot);
                // Adjust hand orientation to be more natural
                euler.x += glm::radians(5.0f);
                euler.y += glm::radians(5.0f);
                rot = glm::quat(euler);
            }
        }

        if (normName.find("foot") != std::string::npos)
        {
            // Correct foot rotations for better ground contact
            for (auto &rot : boneAnim.rotationValues)
            {
                glm::vec3 euler = glm::eulerAngles(rot);
                // Adjust foot orientation to be flatter on ground
                euler.x -= glm::radians(10.0f);
                rot = glm::quat(euler);
            }
        }
    }
}

Animation AnimationRetargeting::NormalizeToStandardPose(const Animation &sourceAnim, const Skeleton &skeleton)
{
    // Start with the source animation
    Animation normalizedAnim = sourceAnim;

    // Apply T-pose to A-pose correction
    normalizedAnim = FixTPoseToAPose(normalizedAnim, skeleton);

    // Apply corrective transforms
    ApplyCorrectiveTransforms(normalizedAnim, skeleton);

    return normalizedAnim;
}

glm::mat4 AnimationRetargeting::MapBoneTransform(
    const glm::mat4 &sourceTransform,
    const std::string &sourceBoneName,
    const std::string &targetBoneName,
    const Skeleton &sourceSkel,
    const Skeleton &targetSkel,
    float scale)
{
    // This is a simplified implementation
    // In a full implementation, this would handle complex transformations
    // between different skeleton structures

    // For now, just apply scaling to the translation component
    glm::mat4 result = sourceTransform;
    glm::vec3 translation = glm::vec3(result[3]);
    translation *= scale;
    result[3][0] = translation.x;
    result[3][1] = translation.y;
    result[3][2] = translation.z;

    return result;
}

std::vector<std::string> AnimationRetargeting::GetMixamoBoneVariants(const std::string &boneName)
{
    std::vector<std::string> variants;
    std::string lowerName = boneName;

    // Convert to lowercase for comparison
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

    // Add the original name
    variants.push_back(boneName);

    // Add common Mixamo prefixes/suffixes
    if (lowerName.find("mixamorig:") == 0)
    {
        // Remove prefix
        std::string withoutPrefix = boneName.substr(12); // length of "mixamorig:"
        variants.push_back(withoutPrefix);
    }
    else
    {
        // Add with prefix
        variants.push_back("mixamorig:" + boneName);
    }

    // Add other common variations
    if (lowerName.find("left") != std::string::npos)
    {
        std::string rightVersion = boneName;
        size_t pos = rightVersion.find("Left");
        if (pos != std::string::npos)
        {
            rightVersion.replace(pos, 4, "Right");
        }
        else
        {
            pos = rightVersion.find("left");
            if (pos != std::string::npos)
            {
                rightVersion.replace(pos, 4, "right");
            }
        }
        variants.push_back(rightVersion);
    }
    else if (lowerName.find("right") != std::string::npos)
    {
        std::string leftVersion = boneName;
        size_t pos = leftVersion.find("Right");
        if (pos != std::string::npos)
        {
            leftVersion.replace(pos, 5, "Left");
        }
        else
        {
            pos = leftVersion.find("right");
            if (pos != std::string::npos)
            {
                leftVersion.replace(pos, 5, "left");
            }
        }
        variants.push_back(leftVersion);
    }

    return variants;
}

std::string AnimationRetargeting::FindBestBoneMatch(const std::string &boneName, const Skeleton &skeleton)
{
    // First try exact match
    if (skeleton.GetBoneIndex(boneName) != -1)
    {
        return boneName;
    }

    // Try normalized match
    std::string normName = NormalizeBoneName(boneName);
    if (skeleton.GetBoneIndex(normName) != -1)
    {
        return normName;
    }

    // Try Mixamo variants
    auto variants = GetMixamoBoneVariants(boneName);
    for (const auto &variant : variants)
    {
        if (skeleton.GetBoneIndex(variant) != -1)
        {
            return variant;
        }

        std::string normVariant = NormalizeBoneName(variant);
        if (skeleton.GetBoneIndex(normVariant) != -1)
        {
            return normVariant;
        }
    }

    // Try partial matches
    std::string lowerBoneName = boneName;
    std::transform(lowerBoneName.begin(), lowerBoneName.end(), lowerBoneName.begin(), ::tolower);

    for (const auto &[skeletonBoneName, idx] : skeleton.boneMapping)
    {
        std::string lowerSkeletonName = skeletonBoneName;
        std::transform(lowerSkeletonName.begin(), lowerSkeletonName.end(), lowerSkeletonName.begin(), ::tolower);

        if (lowerSkeletonName.find(lowerBoneName) != std::string::npos ||
            lowerBoneName.find(lowerSkeletonName) != std::string::npos)
        {
            return skeletonBoneName;
        }
    }

    // No match found
    return "";
}

std::map<std::string, std::string> AnimationRetargeting::GenerateEnhancedBoneMapping(
    const Skeleton &sourceSkel,
    const Skeleton &targetSkel)
{
    std::map<std::string, std::string> boneMap;

    // First, try exact matches
    for (const auto &[sourceName, sourceIdx] : sourceSkel.boneMapping)
    {
        if (targetSkel.GetBoneIndex(sourceName) != -1)
        {
            boneMap[NormalizeBoneName(sourceName)] = NormalizeBoneName(sourceName);
        }
    }

    // Then try Mixamo-specific mappings
    auto mixamoMap = GenerateMixamoBoneMapping(sourceSkel, targetSkel);
    for (const auto &[source, target] : mixamoMap)
    {
        if (boneMap.find(source) == boneMap.end())
        {
            boneMap[source] = target;
        }
    }

    // Finally, try fuzzy matching for any remaining bones
    for (const auto &[sourceName, sourceIdx] : sourceSkel.boneMapping)
    {
        std::string normSource = NormalizeBoneName(sourceName);
        if (boneMap.find(normSource) == boneMap.end())
        {
            // Find the best match in the target skeleton
            std::string bestMatch;
            float bestScore = 0.0f;

            for (const auto &[targetName, targetIdx] : targetSkel.boneMapping)
            {
                float score = CalculateBoneSimilarity(normSource, NormalizeBoneName(targetName));
                if (score > bestScore && score > 0.6f)
                { // Require at least 60% similarity
                    bestScore = score;
                    bestMatch = NormalizeBoneName(targetName);
                }
            }

            if (!bestMatch.empty())
            {
                boneMap[normSource] = bestMatch;
            }
        }
    }

    return boneMap;
}

float AnimationRetargeting::CalculateBoneSimilarity(const std::string &bone1, const std::string &bone2)
{
    std::string b1 = bone1;
    std::string b2 = bone2;

    // Convert to lowercase for comparison
    std::transform(b1.begin(), b1.end(), b1.begin(), ::tolower);
    std::transform(b2.begin(), b2.end(), b2.begin(), ::tolower);

    // Check for exact match
    if (b1 == b2)
        return 1.0f;

    // Check for substring match
    if (b1.find(b2) != std::string::npos || b2.find(b1) != std::string::npos)
    {
        // Return similarity based on the length of the shorter string
        float minLength = std::min(b1.length(), b2.length());
        float maxLength = std::max(b1.length(), b2.length());
        return minLength / maxLength;
    }

    // Check for common patterns
    if ((b1.find("left") != std::string::npos && b2.find("left") != std::string::npos) ||
        (b1.find("right") != std::string::npos && b2.find("right") != std::string::npos))
    {
        // Same side, check for similar bone type
        std::string b1Type = b1;
        std::string b2Type = b2;

        // Remove side indicators
        size_t pos;
        if ((pos = b1Type.find("left")) != std::string::npos)
            b1Type.erase(pos, 4);
        if ((pos = b1Type.find("right")) != std::string::npos)
            b1Type.erase(pos, 5);
        if ((pos = b2Type.find("left")) != std::string::npos)
            b2Type.erase(pos, 4);
        if ((pos = b2Type.find("right")) != std::string::npos)
            b2Type.erase(pos, 5);

        // Calculate similarity of the bone type
        if (b1Type == b2Type)
            return 0.8f;

        // Partial match
        if (b1Type.find(b2Type) != std::string::npos || b2Type.find(b1Type) != std::string::npos)
        {
            float minLength = std::min(b1Type.length(), b2Type.length());
            float maxLength = std::max(b1Type.length(), b2Type.length());
            return 0.6f + (0.2f * minLength / maxLength);
        }
    }

    // No significant similarity
    return 0.0f;
}

std::vector<std::string> AnimationRetargeting::GetAllBoneNameVariations(const std::string &boneName)
{
    std::vector<std::string> variations;
    variations.push_back(boneName);
    variations.push_back(NormalizeBoneName(boneName));

    // Add common prefixes/suffixes
    variations.push_back("mixamorig:" + boneName);
    variations.push_back(boneName.substr(12)); // Remove mixamorig: prefix if present

    // Add variations with underscores/dots replaced
    std::string temp = boneName;
    std::replace(temp.begin(), temp.end(), '.', '_');
    variations.push_back(temp);

    temp = boneName;
    std::replace(temp.begin(), temp.end(), '_', '.');
    variations.push_back(temp);

    return variations;
}

AnimationRetargeting::RigType AnimationRetargeting::DetectRigType(const Skeleton &skeleton)
{
    // Count bones with specific naming patterns to determine rig type
    int mixamoBones = 0;
    int mannequinBones = 0;
    int genericBones = 0;

    for (const auto &[boneName, boneIdx] : skeleton.boneMapping)
    {
        std::string normName = NormalizeBoneName(boneName);

        // Check for Mixamo-specific naming
        if (boneName.find("mixamorig:") != std::string::npos)
        {
            mixamoBones++;
        }
        // Check for Epic Mannequin naming
        else if (normName.find("root") != std::string::npos ||
                 normName.find("pelvis") != std::string::npos ||
                 normName.find("spine") != std::string::npos ||
                 normName.find("clavicle") != std::string::npos)
        {
            mannequinBones++;
        }
        // Check for generic human naming
        else if (normName.find("hip") != std::string::npos ||
                 normName.find("spine") != std::string::npos ||
                 normName.find("neck") != std::string::npos ||
                 normName.find("head") != std::string::npos ||
                 normName.find("arm") != std::string::npos ||
                 normName.find("leg") != std::string::npos ||
                 normName.find("foot") != std::string::npos)
        {
            genericBones++;
        }
    }

    // Determine the most likely rig type based on bone counts
    if (mixamoBones > mannequinBones && mixamoBones > genericBones)
    {
        return RigType::MIXAMO_RIG;
    }
    else if (mannequinBones > mixamoBones && mannequinBones > genericBones)
    {
        return RigType::EPIC_MANNEQUIN;
    }
    else if (genericBones > 0)
    {
        return RigType::GENERIC_HUMAN;
    }

    return RigType::UNKNOWN;
}

AnimationRetargeting::RigType AnimationRetargeting::DetectRigTypeFromAnimation(const Animation &animation)
{
    // Count bones with specific naming patterns to determine rig type
    int mixamoBones = 0;
    int mannequinBones = 0;
    int genericBones = 0;

    for (const auto &[boneName, boneAnim] : animation.boneAnimations)
    {
        std::string normName = NormalizeBoneName(boneName);

        // Check for Mixamo-specific naming
        if (boneName.find("mixamorig:") != std::string::npos)
        {
            mixamoBones++;
        }
        // Check for Epic Mannequin naming
        else if (normName.find("root") != std::string::npos ||
                 normName.find("pelvis") != std::string::npos ||
                 normName.find("spine") != std::string::npos ||
                 normName.find("clavicle") != std::string::npos)
        {
            mannequinBones++;
        }
        // Check for generic human naming
        else if (normName.find("hip") != std::string::npos ||
                 normName.find("spine") != std::string::npos ||
                 normName.find("neck") != std::string::npos ||
                 normName.find("head") != std::string::npos ||
                 normName.find("arm") != std::string::npos ||
                 normName.find("leg") != std::string::npos ||
                 normName.find("foot") != std::string::npos)
        {
            genericBones++;
        }
    }

    // Determine the most likely rig type based on bone counts
    if (mixamoBones > mannequinBones && mixamoBones > genericBones)
    {
        return RigType::MIXAMO_RIG;
    }
    else if (mannequinBones > mixamoBones && mannequinBones > genericBones)
    {
        return RigType::EPIC_MANNEQUIN;
    }
    else if (genericBones > 0)
    {
        return RigType::GENERIC_HUMAN;
    }

    return RigType::UNKNOWN;
}

std::map<std::string, std::string> AnimationRetargeting::GetStandardRigMapping(RigType rigType)
{
    std::map<std::string, std::string> mapping;

    switch (rigType)
    {
    case RigType::MIXAMO_RIG:
        mapping = {
            {"mixamorig:Hips", "hips"},
            {"mixamorig:Spine", "spine"},
            {"mixamorig:Spine1", "spine1"},
            {"mixamorig:Spine2", "spine2"},
            {"mixamorig:Neck", "neck"},
            {"mixamorig:Head", "head"},
            {"mixamorig:LeftShoulder", "left_shoulder"},
            {"mixamorig:LeftArm", "left_arm"},
            {"mixamorig:LeftForeArm", "left_forearm"},
            {"mixamorig:LeftHand", "left_hand"},
            {"mixamorig:RightShoulder", "right_shoulder"},
            {"mixamorig:RightArm", "right_arm"},
            {"mixamorig:RightForeArm", "right_forearm"},
            {"mixamorig:RightHand", "right_hand"},
            {"mixamorig:LeftUpLeg", "left_upleg"},
            {"mixamorig:LeftLeg", "left_leg"},
            {"mixamorig:LeftFoot", "left_foot"},
            {"mixamorig:LeftToeBase", "left_toe"},
            {"mixamorig:RightUpLeg", "right_upleg"},
            {"mixamorig:RightLeg", "right_leg"},
            {"mixamorig:RightFoot", "right_foot"},
            {"mixamorig:RightToeBase", "right_toe"}};
        break;

    case RigType::EPIC_MANNEQUIN:
        mapping = {
            {"root", "hips"},
            {"pelvis", "pelvis"},
            {"spine_01", "spine"},
            {"spine_02", "spine1"},
            {"spine_03", "spine2"},
            {"neck_01", "neck"},
            {"head", "head"},
            {"clavicle_l", "left_shoulder"},
            {"upperarm_l", "left_arm"},
            {"lowerarm_l", "left_forearm"},
            {"hand_l", "left_hand"},
            {"clavicle_r", "right_shoulder"},
            {"upperarm_r", "right_arm"},
            {"lowerarm_r", "right_forearm"},
            {"hand_r", "right_hand"},
            {"thigh_l", "left_upleg"},
            {"calf_l", "left_leg"},
            {"foot_l", "left_foot"},
            {"ball_l", "left_toe"},
            {"thigh_r", "right_upleg"},
            {"calf_r", "right_leg"},
            {"foot_r", "right_foot"},
            {"ball_r", "right_toe"}};
        break;

    case RigType::GENERIC_HUMAN:
        mapping = {
            {"hips", "hips"},
            {"pelvis", "pelvis"},
            {"spine", "spine"},
            {"spine1", "spine1"},
            {"spine2", "spine2"},
            {"neck", "neck"},
            {"head", "head"},
            {"left_shoulder", "left_shoulder"},
            {"left_arm", "left_arm"},
            {"left_forearm", "left_forearm"},
            {"left_hand", "left_hand"},
            {"right_shoulder", "right_shoulder"},
            {"right_arm", "right_arm"},
            {"right_forearm", "right_forearm"},
            {"right_hand", "right_hand"},
            {"left_upleg", "left_upleg"},
            {"left_leg", "left_leg"},
            {"left_foot", "left_foot"},
            {"left_toe", "left_toe"},
            {"right_upleg", "right_upleg"},
            {"right_leg", "right_leg"},
            {"right_foot", "right_foot"},
            {"right_toe", "right_toe"}};
        break;

    default:
        break;
    }

    return mapping;
}

bool AnimationRetargeting::ValidateSkeletonStructure(const Skeleton &skeleton, RigType expectedType)
{
    // Define required bones for each rig type
    std::vector<std::string> requiredBones;

    switch (expectedType)
    {
    case RigType::MIXAMO_RIG:
        requiredBones = {"mixamorig:Hips", "mixamorig:Spine", "mixamorig:Head",
                         "mixamorig:LeftUpLeg", "mixamorig:RightUpLeg",
                         "mixamorig:LeftArm", "mixamorig:RightArm"};
        break;

    case RigType::EPIC_MANNEQUIN:
        requiredBones = {"root", "pelvis", "spine_01", "head",
                         "thigh_l", "thigh_r", "upperarm_l", "upperarm_r"};
        break;

    case RigType::GENERIC_HUMAN:
        requiredBones = {"hips", "spine", "head", "left_upleg", "right_upleg",
                         "left_arm", "right_arm"};
        break;

    default:
        return false;
    }

    // Check if all required bones are present
    for (const auto &bone : requiredBones)
    {
        if (skeleton.GetBoneIndex(bone) == -1)
        {
            return false;
        }
    }

    return true;
}