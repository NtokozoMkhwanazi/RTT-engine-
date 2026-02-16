#pragma once
#include <string>
#include <map>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "AnimationConfig.h"

struct BoneAnimation {
    std::string boneName;
    
    // Separate arrays for each animation channel
    // Position channel
    std::vector<double> positionTimes;
    std::vector<glm::vec3> positionValues;
    
    // Rotation channel
    std::vector<double> rotationTimes;
    std::vector<glm::quat> rotationValues;
    
    // Scale channel
    std::vector<double> scaleTimes;
    std::vector<glm::vec3> scaleValues;

    glm::vec3 InterpolatePosition(float time) const;
    glm::quat InterpolateRotation(float time) const;
    glm::vec3 InterpolateScale(float time) const;
    
    // Check which channels have animation data
    bool HasPositionAnimation() const { return positionTimes.size() > 1; }
    bool HasRotationAnimation() const { return rotationTimes.size() > 1; }
    bool HasScaleAnimation() const { return scaleTimes.size() > 1; }
};

class Animation {
public:
    Animation(const std::string& name, float duration, float ticksPerSecond);

    void AddBoneAnimation(const BoneAnimation& boneAnim);
    const BoneAnimation* GetBoneAnimation(const std::string& boneName) const;
    void DebugPrintBoneNames() const;

    float GetDuration() const;
    float GetTicksPerSecond() const;
    size_t GetTotalKeyframeCount() const;
    
    // Animation compression methods
    void Compress(float positionTolerance = 0.01f, float rotationTolerance = 0.01f, float scaleTolerance = 0.01f);
    void ReduceKeyframes(float tolerance = 0.01f);
    size_t GetCompressedSize() const;

//temp
public:
    std::string name;
    float duration;
    float ticksPerSecond;
    std::map<std::string, BoneAnimation> boneAnimations;
    
    // Compression data
    bool isCompressed = false;
    float compressionRatio = 1.0f;
};

