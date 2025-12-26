#pragma once
#include <string>
#include <map>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "AnimationConfig.h"

struct Keyframe {
    float time;
    glm::vec3 position;
    glm::quat rotation;
    glm::vec3 scale;
};

struct BoneAnimation {
    std::string boneName;
    std::vector<Keyframe> keyframes;

    glm::vec3 InterpolatePosition(float time) const;
    glm::quat InterpolateRotation(float time) const;
    glm::vec3 InterpolateScale(float time) const;
};

class Animation {
public:
    Animation(const std::string& name, float duration, float ticksPerSecond);

    void AddBoneAnimation(const BoneAnimation& boneAnim);
    const BoneAnimation* GetBoneAnimation(const std::string& boneName) const;

    float GetDuration() const;
    float GetTicksPerSecond() const;

private:
    std::string name;
    float duration;
    float ticksPerSecond;
    std::map<std::string, BoneAnimation> boneAnimations;
};

