#include "Animator.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <cmath>
#include <iostream>


// ---------------- Utility ----------------
std::string Animator::NormalizeName(const std::string& name)
{
    std::string n = name;

    size_t p = n.find('|');
    if (p != std::string::npos) n = n.substr(p + 1);

    p = n.find(':');
    if (p != std::string::npos) n = n.substr(p + 1);

    return n;
}

// ---------------- Lifecycle ----------------
Animator::Animator(const Skeleton* skeleton)
    : skeleton(skeleton)
{
    finalBoneMatrices.resize(MAX_BONES, glm::mat4(1.0f));
}

void Animator::Play(Animation* animation)
{
    current = animation;
    next = nullptr;
    time = 0.0f;
    blendTime = 0.0f;
    blendDuration = 0.0f;
}

void Animator::BlendTo(Animation* animation, float duration)
{
    if (!animation || animation == current)
        return;

    next = animation;
    blendTime = 0.0f;
    blendDuration = glm::max(duration, 0.001f);
}

// ---------------- Update ----------------
void Animator::Update(float dt)
{
    if (!current || !skeleton)
        return;

    float tps = current->GetTicksPerSecond();
    if (tps <= 0.0f) tps = 25.0f;

    time = fmod(time + dt * tps, current->GetDuration());

    rootMotionDelta = glm::vec3(0.0f);

    if (next)
    {
        blendTime += dt;
        float factor = glm::clamp(blendTime / blendDuration, 0.0f, 1.0f);
        EvaluateNode(skeleton->rootNode, glm::mat4(1.0f), next, factor);
        finalBoneMatrices[0] = glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), {0,1,0});
        

        if (factor >= 1.0f)
        {
            current = next;
            next = nullptr;
        }
    }
    else
    {
        EvaluateNode(skeleton->rootNode, glm::mat4(1.0f), nullptr, 0.0f);
    }
}

void Animator::EvaluateNode(
    const AssimpNodeData& node,
    const glm::mat4& parentTransform,
    Animation* /*blendAnim*/,
    float /*blendFactor*/)
{
    std::string key = NormalizeName(node.name);

    glm::mat4 localTransform = node.transform;

    if (current)
    {
        if (auto* a = current->GetBoneAnimation(key))
        {
            glm::vec3 pos = a->InterpolatePosition(time);
            glm::quat rot = a->InterpolateRotation(time);
            glm::vec3 scl = a->InterpolateScale(time);

            localTransform =
                glm::translate(glm::mat4(1.0f), pos) *
                glm::toMat4(rot) *
                glm::scale(glm::mat4(1.0f), scl);
        }
    }

    glm::mat4 globalTransform = parentTransform * localTransform;

    auto it = skeleton->boneMapping.find(key);
    if (it != skeleton->boneMapping.end())
    {
        int idx = it->second;
        finalBoneMatrices[idx] =
            skeleton->globalInverseTransform *
            globalTransform *
            skeleton->bones[idx].offset;
    }

    for (const auto& child : node.children)
        EvaluateNode(child, globalTransform, nullptr, 0.0f);
}



// ---------------- Upload to GPU ----------------
void Animator::Upload(Shader& shader)
{
    for (int i = 0; i < skeleton->bones.size(); ++i)
{
    shader.setMat4("bones[" + std::to_string(i) + "]", finalBoneMatrices[i]);
}

}

// ---------------- Root Motion ----------------
glm::vec3 Animator::ConsumeRootMotion()
{
    glm::vec3 d = rootMotionDelta;
    rootMotionDelta = glm::vec3(0.0f);
    return d;
}

// ---------------- Accessor ----------------
const std::vector<glm::mat4>& Animator::GetFinalBoneMatrices() const
{
    return finalBoneMatrices;
}

