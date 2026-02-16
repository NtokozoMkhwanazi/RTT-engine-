#include "AssimpAnimationLoader.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <algorithm>
#include <iostream>

#include "boneSystem/BoneName.h"

static glm::vec3 ToVec3(const aiVector3D &v)
{
    // Direct conversion: use Assimp positions as-is
    return glm::vec3(v.x, v.y, v.z);
}

static glm::quat ToQuat(const aiQuaternion &q)
{
    // Direct conversion from Assimp (w, x, y, z) to glm (w, x, y, z).
    // Assimp and glm both use the same quaternion component order.
    glm::quat qq(q.w, q.x, q.y, q.z);
    return glm::normalize(qq);
}

Animation AssimpAnimationLoader::LoadAnimation(
    const aiScene *scene,
    const aiAnimation *aiAnim)
{
    float duration = static_cast<float>(aiAnim->mDuration);
    float ticksPerSecond =
        aiAnim->mTicksPerSecond != 0.0
            ? static_cast<float>(aiAnim->mTicksPerSecond)
            : 25.0f;

    Animation animation(
        aiAnim->mName.C_Str(),
        duration,
        ticksPerSecond);

    for (unsigned int i = 0; i < aiAnim->mNumChannels; ++i)
    {
        aiNodeAnim *channel = aiAnim->mChannels[i];

        BoneAnimation boneAnim;
        boneAnim.boneName =
            NormalizeBoneName(channel->mNodeName.C_Str());

        // Populate position channel
        for (unsigned int k = 0; k < channel->mNumPositionKeys; ++k)
        {
            boneAnim.positionTimes.push_back(channel->mPositionKeys[k].mTime);
            boneAnim.positionValues.push_back(ToVec3(channel->mPositionKeys[k].mValue));
        }


        // Populate rotation channel
        boneAnim.rotationTimes.reserve(channel->mNumRotationKeys);
        boneAnim.rotationValues.reserve(channel->mNumRotationKeys);

        glm::quat prevQ(1, 0, 0, 0);

        for (unsigned int k = 0; k < channel->mNumRotationKeys; ++k)
        {
            boneAnim.rotationTimes.push_back(channel->mRotationKeys[k].mTime);

            glm::quat q = ToQuat(channel->mRotationKeys[k].mValue);

            // Fix quaternion sign flip (CRITICAL for Mixamo)
            if (k > 0 && glm::dot(prevQ, q) < 0.0f)
                q = -q;

            boneAnim.rotationValues.push_back(q);
            prevQ = q;
        }

        // Populate scale channel
        for (unsigned int k = 0; k < channel->mNumScalingKeys; ++k)
        {
            boneAnim.scaleTimes.push_back(channel->mScalingKeys[k].mTime);
            boneAnim.scaleValues.push_back(ToVec3(channel->mScalingKeys[k].mValue));
        }

        animation.AddBoneAnimation(boneAnim);
    }

    // DEBUG: Print first 3 rotation key times for "rightupleg"
    const BoneAnimation *debugBone = animation.GetBoneAnimation("rightupleg");
    if (debugBone && debugBone->rotationTimes.size() > 0)
    {
        std::cout << "[ANIMATION DEBUG] rightupleg rotation key times (first 3): ";
        for (int i = 0; i < std::min(3, (int)debugBone->rotationTimes.size()); ++i)
        {
            std::cout << debugBone->rotationTimes[i];
            if (i < 2)
                std::cout << ", ";
        }
        std::cout << "\n";
    }

    return animation;
}

Animation AssimpAnimationLoader::LoadAnimationWithPoseCorrection(const aiScene* scene, const aiAnimation* aiAnim)
{
    // First load the animation normally
    Animation anim = LoadAnimation(scene, aiAnim);
    
    // Apply pose corrections if needed
    if (IsTPoseAnimation(scene, aiAnim)) {
        anim = ApplyPoseCorrections(anim);
    }
    
    return anim;
}

bool AssimpAnimationLoader::IsTPoseAnimation(const aiScene* scene, const aiAnimation* aiAnim)
{
    // Check if the animation appears to be in T-pose by examining the initial keyframes
    // In T-pose, arms are extended horizontally from the body
    
    for (unsigned int i = 0; i < aiAnim->mNumChannels; ++i)
    {
        aiNodeAnim *channel = aiAnim->mChannels[i];
        std::string boneName = NormalizeBoneName(channel->mNodeName.C_Str());
        
        // Check for arm bones
        if (boneName.find("arm") != std::string::npos || 
            boneName.find("forearm") != std::string::npos) {
            
            if (channel->mNumRotationKeys > 0) {
                // Get the first rotation keyframe
                aiQuaterniont<float> firstRot = channel->mRotationKeys[0].mValue;
                glm::quat rotation = ToQuat(firstRot);
                
                // Convert to Euler angles to check if arms are horizontal
                glm::vec3 euler = glm::eulerAngles(rotation);
                
                // In T-pose, arms are typically rotated outward (around Y or Z axis)
                // Check if the rotation suggests a T-pose
                if (std::abs(euler.y) > 0.5f || std::abs(euler.z) > 0.5f) {
                    return true;
                }
            }
        }
    }
    
    return false;
}

Animation AssimpAnimationLoader::ApplyPoseCorrections(const Animation& anim)
{
    // Apply corrections to convert from T-pose to A-pose or fix other common issues
    Animation correctedAnim = anim;
    
    for (auto& [boneName, boneAnim] : correctedAnim.boneAnimations) {
        std::string normName = NormalizeBoneName(boneName);
        
        // Correct arm rotations for T-pose to A-pose conversion
        if (normName.find("arm") != std::string::npos || 
            normName.find("upperarm") != std::string::npos ||
            normName.find("forearm") != std::string::npos) {
            
            // Apply corrective rotation to bring arms from T-pose to A-pose
            for (auto& rot : boneAnim.rotationValues) {
                glm::vec3 euler = glm::eulerAngles(rot);
                
                // For left arm, rotate slightly inward toward body
                if (normName.find("left") != std::string::npos) {
                    euler.y -= glm::radians(20.0f); // Rotate inward
                    euler.z -= glm::radians(10.0f); // Slight forward rotation
                }
                // For right arm, rotate slightly inward toward body (opposite direction)
                else if (normName.find("right") != std::string::npos) {
                    euler.y += glm::radians(20.0f); // Rotate inward
                    euler.z -= glm::radians(10.0f); // Slight forward rotation
                }
                
                rot = glm::quat(euler);
            }
        }
        
        // Correct hand rotations that are often off in Mixamo exports
        if (normName.find("hand") != std::string::npos) {
            for (auto& rot : boneAnim.rotationValues) {
                glm::vec3 euler = glm::eulerAngles(rot);
                // Adjust hand orientation to be more natural
                euler.x += glm::radians(5.0f);
                euler.y += glm::radians(5.0f);
                rot = glm::quat(euler);
            }
        }
        
        // Correct foot rotations for better ground contact
        if (normName.find("foot") != std::string::npos) {
            for (auto& rot : boneAnim.rotationValues) {
                glm::vec3 euler = glm::eulerAngles(rot);
                // Adjust foot orientation to be flatter on ground
                euler.x -= glm::radians(10.0f);
                rot = glm::quat(euler);
            }
        }
    }
    
    return correctedAnim;
}
