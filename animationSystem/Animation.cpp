#include "Animation.h"
#include <algorithm>
#include <cmath>
#include <iostream>

// ---------------- Animation ----------------

Animation::Animation(const std::string& name, float duration, float tps)
    : name(name), duration(duration), ticksPerSecond(tps) {}

void Animation::AddBoneAnimation(const BoneAnimation& boneAnim) {
    boneAnimations[boneAnim.boneName] = boneAnim;
}

void Animation::DebugPrintBoneNames() const {
    std::cout << "==== Animation Bone Channels ====\n";
    for (const auto& [name, boneAnim] : boneAnimations) {
        std::cout << name << " | ";
        if (boneAnim.HasPositionAnimation()) std::cout << "POS (" << boneAnim.positionTimes.size() << ") ";
        if (boneAnim.HasRotationAnimation()) std::cout << "ROT (" << boneAnim.rotationTimes.size() << ") ";
        if (boneAnim.HasScaleAnimation()) std::cout << "SCALE (" << boneAnim.scaleTimes.size() << ") ";
        std::cout << "\n";
    }
}

const BoneAnimation* Animation::GetBoneAnimation(const std::string& boneName) const {
    auto it = boneAnimations.find(boneName);
    if (it == boneAnimations.end())
        return nullptr;
    return &it->second;
}

float Animation::GetDuration() const {
    return duration;
}

float Animation::GetTicksPerSecond() const {
    return ticksPerSecond;
}

size_t Animation::GetTotalKeyframeCount() const {
    size_t count = 0;
    for (const auto& [_, bone] : boneAnimations) {
        count += bone.positionTimes.size();
        count += bone.rotationTimes.size();
        count += bone.scaleTimes.size();
    }
    return count;
}

// Helper: find insertion point in sorted arrays
static int FindKeyIndex(const std::vector<double>& times, double time) {
    if (times.size() < 2) return 0;
    
    for (int i = 0; i < (int)times.size() - 1; ++i) {
        if (time < times[i + 1])
            return i;
    }
    return (int)times.size() - 2;
}

glm::vec3 BoneAnimation::InterpolatePosition(float time) const {
    if (positionTimes.size() == 0)
        return glm::vec3(0.0f);
    if (positionTimes.size() == 1)
        return positionValues[0];

    int i = FindKeyIndex(positionTimes, (double)time);
    double t1 = positionTimes[i];
    double t2 = positionTimes[i + 1];
    double delta = t2 - t1;
    float blend = delta > 0.0001 ? (float)((time - t1) / delta) : 0.0f;

    return glm::mix(positionValues[i], positionValues[i + 1], blend);
}

glm::quat BoneAnimation::InterpolateRotation(float time) const {
    if (rotationTimes.size() == 0)
        return glm::quat(1, 0, 0, 0);
    if (rotationTimes.size() == 1)
        return glm::normalize(rotationValues[0]);

    int i = FindKeyIndex(rotationTimes, (double)time);
    double t1 = rotationTimes[i];
    double t2 = rotationTimes[i + 1];
    double delta = t2 - t1;
    float blend = delta > 0.0001 ? (float)((time - t1) / delta) : 0.0f;

    glm::quat result = glm::slerp(rotationValues[i], rotationValues[i + 1], blend);
    return glm::normalize(result);  // CRITICAL: always normalize after interpolation
}

glm::vec3 BoneAnimation::InterpolateScale(float time) const {
    if (scaleTimes.size() == 0)
        return glm::vec3(1.0f);
    if (scaleTimes.size() == 1)
        return scaleValues[0];

    int i = FindKeyIndex(scaleTimes, (double)time);
    double t1 = scaleTimes[i];
    double t2 = scaleTimes[i + 1];
    double delta = t2 - t1;
    float blend = delta > 0.0001 ? (float)((time - t1) / delta) : 0.0f;

    return glm::mix(scaleValues[i], scaleValues[i + 1], blend);
}

