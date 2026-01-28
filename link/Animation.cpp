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
    for (const auto& [name, _] : boneAnimations)
        std::cout << name << "\n";
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
    for (const auto& [_, bone] : boneAnimations)
        count += bone.keyframes.size();
    return count;
}

// ---------------- BoneAnimation ----------------

static int FindKeyframe(const std::vector<Keyframe>& keys, float time) {
    for (int i = 0; i < (int)keys.size() - 1; i++) {
        if (time < keys[i + 1].time)
            return i;
    }
    return (int)keys.size() - 2;
}

glm::vec3 BoneAnimation::InterpolatePosition(float time) const {
    if (keyframes.size() == 1)
        return keyframes[0].position;

    // 🔥 wrap time (prevents freezing)
    float maxTime = keyframes.back().time;
    time = fmod(time, maxTime);

    int i = FindKeyframe(keyframes, time);
    const Keyframe& a = keyframes[i];
    const Keyframe& b = keyframes[i + 1];

    float delta = b.time - a.time;
    float t = delta > 0.0f ? (time - a.time) / delta : 0.0f;

    return glm::mix(a.position, b.position, t);
}

glm::quat BoneAnimation::InterpolateRotation(float time) const {
    if (keyframes.size() == 1)
        return keyframes[0].rotation;

    // 🔥 wrap time
    float maxTime = keyframes.back().time;
    time = fmod(time, maxTime);

    int i = FindKeyframe(keyframes, time);
    const Keyframe& a = keyframes[i];
    const Keyframe& b = keyframes[i + 1];

    float delta = b.time - a.time;
    float t = delta > 0.0f ? (time - a.time) / delta : 0.0f;

    return glm::slerp(a.rotation, b.rotation, t);
}

glm::vec3 BoneAnimation::InterpolateScale(float time) const {
    if (keyframes.size() == 1)
        return keyframes[0].scale;

    // 🔥 wrap time
    float maxTime = keyframes.back().time;
    time = fmod(time, maxTime);

    int i = FindKeyframe(keyframes, time);
    const Keyframe& a = keyframes[i];
    const Keyframe& b = keyframes[i + 1];

    float delta = b.time - a.time;
    float t = delta > 0.0f ? (time - a.time) / delta : 0.0f;

    return glm::mix(a.scale, b.scale, t);
}

