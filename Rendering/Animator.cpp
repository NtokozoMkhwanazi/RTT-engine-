#include "Animator.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <iostream>
#include <cmath>

// ---------------- Utility ----------------
std::string Animator::NormalizeName(const std::string& name) {
    std::string n = name;

    // Strip namespaces (Armature|Bone, mixamorig:Bone)
    size_t p = n.find('|');
    if (p != std::string::npos)
        n = n.substr(p + 1);

    p = n.find(':');
    if (p != std::string::npos)
        n = n.substr(p + 1);

    return n;
}

// ---------------- Lifecycle ----------------
Animator::Animator(const Skeleton* skeleton)
    : skeleton(skeleton)
{
    finalBoneMatrices.resize(MAX_BONES, glm::mat4(1.0f));
}

void Animator::Play(Animation* animation) {
    current = animation;
    next = nullptr;
    time = 0.0f;
    blendTime = 0.0f;
    blendDuration = 0.0f;
}

void Animator::BlendTo(Animation* animation, float duration) {
    if (!animation || animation == current)
        return;

    next = animation;
    blendTime = 0.0f;
    blendDuration = glm::max(duration, 0.001f);
}

void Animator::Update(float dt) {
    if (!current || !skeleton)
        return;

    float tps = current->GetTicksPerSecond();
    if (tps <= 0.0f) tps = 25.0f;

    time += dt * tps;
    time = fmod(time, current->GetDuration());

    if (next) {
        blendTime += dt;
        float factor = glm::clamp(blendTime / blendDuration, 0.0f, 1.0f);
        EvaluatePose(next, factor);

        if (factor >= 1.0f) {
            current = next;
            next = nullptr;
        }
    } else {
        EvaluatePose();
    }
}

// ---------------- Pose Evaluation ----------------
void Animator::EvaluatePose() {
    rootMotionDelta = glm::vec3(0.0f);
    EvaluateNode(skeleton->rootNode, glm::mat4(1.0f), nullptr, 0.0f);
}

void Animator::EvaluatePose(Animation* blendAnim, float blendFactor) {
    rootMotionDelta = glm::vec3(0.0f);
    EvaluateNode(skeleton->rootNode, glm::mat4(1.0f), blendAnim, blendFactor);
}

void Animator::EvaluateNode(
    const AssimpNodeData& node,
    const glm::mat4& parent,
    Animation* blendAnim,
    float blendFactor)
{
    std::string key = NormalizeName(node.name);

    glm::vec3 pos(0.0f);
    glm::quat rot(1,0,0,0);
    glm::vec3 scl(1.0f);

    // ---- Base animation ----
    if (current) {
        if (auto* a = current->GetBoneAnimation(key)) {
            pos = a->InterpolatePosition(time);
            rot = a->InterpolateRotation(time);
            scl = a->InterpolateScale(time);
        }
    }

    // ---- Blending ----
    if (blendAnim && blendFactor > 0.0f) {
        if (auto* a = blendAnim->GetBoneAnimation(key)) {
            pos = glm::mix(pos, a->InterpolatePosition(time), blendFactor);
            rot = glm::slerp(rot, a->InterpolateRotation(time), blendFactor);
            scl = glm::mix(scl, a->InterpolateScale(time), blendFactor);
        }
    }

    // ---- Root motion ----
    if (key == "Hips" || key == "Root") {
        rootMotionDelta += pos;
        pos = glm::vec3(0.0f);
    }

    glm::mat4 local =
        glm::translate(glm::mat4(1.0f), pos) *
        glm::toMat4(rot) *
        glm::scale(glm::mat4(1.0f), scl);

    glm::mat4 global = parent * node.transform * local;

    // ---- Bone output ----
    auto it = skeleton->boneMapping.find(key);
    if (it != skeleton->boneMapping.end()) {
        int idx = it->second;
        finalBoneMatrices[idx] = global * skeleton->bones[idx].offset;
    }

    for (const auto& c : node.children)
        EvaluateNode(c, global, blendAnim, blendFactor);
}

// ---------------- Upload ----------------
void Animator::Upload(Shader& shader) {
    for (size_t i = 0; i < finalBoneMatrices.size(); ++i)
        shader.setMat4("bones[" + std::to_string(i) + "]", finalBoneMatrices[i]);
}

glm::vec3 Animator::ConsumeRootMotion() {
    glm::vec3 d = rootMotionDelta;
    rootMotionDelta = glm::vec3(0.0f);
    return d;
}

const std::vector<glm::mat4>& Animator::GetFinalBoneMatrices() const {
    return finalBoneMatrices;
}

