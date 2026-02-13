#include "AssimpAnimationLoader.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <algorithm>
#include <iostream>

#include "boneSystem/BoneName.h"

static glm::vec3 ToVec3(const aiVector3D& v) {
    // Direct conversion: use Assimp positions as-is
    return glm::vec3(v.x, v.y, v.z);
}

static glm::quat ToQuat(const aiQuaternion& q) {
    // Direct conversion from Assimp (w, x, y, z) to glm (w, x, y, z).
    // Assimp and glm both use the same quaternion component order.
    glm::quat qq(q.w, q.x, q.y, q.z);
    return glm::normalize(qq);
}

Animation AssimpAnimationLoader::LoadAnimation(
    const aiScene* scene,
    const aiAnimation* aiAnim
) {
    float duration = static_cast<float>(aiAnim->mDuration);
    float ticksPerSecond =
        aiAnim->mTicksPerSecond != 0.0
            ? static_cast<float>(aiAnim->mTicksPerSecond)
            : 25.0f;

    Animation animation(
        aiAnim->mName.C_Str(),
        duration,
        ticksPerSecond
    );

    for (unsigned int i = 0; i < aiAnim->mNumChannels; ++i) {
        aiNodeAnim* channel = aiAnim->mChannels[i];

        BoneAnimation boneAnim;
        boneAnim.boneName =
            NormalizeBoneName(channel->mNodeName.C_Str());

        // Populate position channel
        for (unsigned int k = 0; k < channel->mNumPositionKeys; ++k) {
            boneAnim.positionTimes.push_back(channel->mPositionKeys[k].mTime);
            boneAnim.positionValues.push_back(ToVec3(channel->mPositionKeys[k].mValue));
        }

        // Populate rotation channel
        for (unsigned int k = 0; k < channel->mNumRotationKeys; ++k) {
            boneAnim.rotationTimes.push_back(channel->mRotationKeys[k].mTime);
            // Normalize rotations on load to avoid drift from non-normalized keys
            boneAnim.rotationValues.push_back(ToQuat(channel->mRotationKeys[k].mValue));
        }

        // Populate scale channel
        for (unsigned int k = 0; k < channel->mNumScalingKeys; ++k) {
            boneAnim.scaleTimes.push_back(channel->mScalingKeys[k].mTime);
            boneAnim.scaleValues.push_back(ToVec3(channel->mScalingKeys[k].mValue));
        }

        animation.AddBoneAnimation(boneAnim);
    }

    // DEBUG: Print first 3 rotation key times for "rightupleg"
    const BoneAnimation* debugBone = animation.GetBoneAnimation("rightupleg");
    if (debugBone && debugBone->rotationTimes.size() > 0) {
        std::cout << "[ANIMATION DEBUG] rightupleg rotation key times (first 3): ";
        for (int i = 0; i < std::min(3, (int)debugBone->rotationTimes.size()); ++i) {
            std::cout << debugBone->rotationTimes[i];
            if (i < 2) std::cout << ", ";
        }
        std::cout << "\n";
    }

    return animation;
}


