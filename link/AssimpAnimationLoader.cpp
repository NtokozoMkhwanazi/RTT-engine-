#include "AssimpAnimationLoader.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>


#include "BoneName.h"

static glm::vec3 ToVec3(const aiVector3D& v) {
    return glm::vec3(v.x, v.y, v.z);
}

static glm::quat ToQuat(const aiQuaternion& q) {
    return glm::quat(q.w, q.x, q.y, q.z);
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

        unsigned int posCount = channel->mNumPositionKeys;
        unsigned int rotCount = channel->mNumRotationKeys;
        unsigned int sclCount = channel->mNumScalingKeys;

        unsigned int keyCount =
            std::max({ posCount, rotCount, sclCount });

        for (unsigned int k = 0; k < keyCount; ++k) {
            Keyframe key{};

            // --- TIME ---
            if (k < posCount)
                key.time = (float)channel->mPositionKeys[k].mTime;
            else if (k < rotCount)
                key.time = (float)channel->mRotationKeys[k].mTime;
            else if (k < sclCount)
                key.time = (float)channel->mScalingKeys[k].mTime;
            else
                key.time = 0.0f;

            // --- POSITION ---
            if (k < posCount)
                key.position =
                    ToVec3(channel->mPositionKeys[k].mValue);
            else
                key.position = glm::vec3(0.0f);

            // --- ROTATION ---
            if (k < rotCount)
                key.rotation =
                    ToQuat(channel->mRotationKeys[k].mValue);
            else
                key.rotation = glm::quat(1, 0, 0, 0);

            // --- SCALE ---
            if (k < sclCount)
                key.scale =
                    ToVec3(channel->mScalingKeys[k].mValue);
            else
                key.scale = glm::vec3(1.0f);

            boneAnim.keyframes.push_back(key);
        }

        animation.AddBoneAnimation(boneAnim);
    }

    return animation;
}


