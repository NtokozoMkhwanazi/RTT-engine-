#include "Animator.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/string_cast.hpp>
#include <cmath>
#include <iostream>

static glm::mat4 RemoveScale(const glm::mat4& m)
{
    glm::vec3 t, s, skew;
    glm::quat r;
    glm::vec4 p;
    glm::decompose(m, s, r, t, skew, p);
    return glm::translate(glm::mat4(1.0f), t) * glm::toMat4(r);
}
static std::string Normalize(const std::string& s)
{
    std::string out = s;

    // remove assimp hierarchy
    size_t pipe = out.find('|');
    if (pipe != std::string::npos)
        out = out.substr(pipe + 1);

    // remove mixamo prefix
    const std::string mixamo = "mixamorig:";
    if (out.find(mixamo) == 0)
        out = out.substr(mixamo.size());

    return out;
}

Animator::Animator(const Skeleton* skel)
    : skeleton(skel)
{
    finalBoneMatrices.resize(MAX_BONES, glm::mat4(1.0f));
    prevBoneWorldPos.resize(MAX_BONES, glm::vec3(0.0f));
    currBoneWorldPos.resize(MAX_BONES, glm::vec3(0.0f));
    ikOffsets.resize(MAX_BONES, glm::vec3(0.0f));
}

void Animator::Play(Animation* anim)
{
    current = anim;
    next = nullptr;
    time = 0.0f;
}

void Animator::BlendTo(Animation* anim, float duration)
{
    if (!anim || anim == current) return;
    next = anim;
    blendTime = 0.0f;
    blendDuration = glm::max(duration, 0.001f);
}

void Animator::Update(float dt)
{
    if (!current || !skeleton) return;

    // save previous positions
    prevBoneWorldPos = currBoneWorldPos;

    float tps = current->GetTicksPerSecond();
    if (tps <= 0.0f) tps = 25.0f;

    time = fmod(time + dt * tps, current->GetDuration());

    EvaluateNode(skeleton->rootNode, glm::mat4(1.0f), nullptr, 0.0f);

    // clear IK offsets every frame
    std::fill(ikOffsets.begin(), ikOffsets.end(), glm::vec3(0.0f));
}

void Animator::EvaluateNode(
    const AssimpNodeData& node,
    const glm::mat4& parent,
    Animation*,
    float)
{
    std::string key = Normalize(node.name);
    glm::mat4 local = node.transform;

    auto it = skeleton->boneMapping.find(key);
    if (it != skeleton->boneMapping.end())
    {
        if (auto* a = current->GetBoneAnimation(key))
        {
            local =
                glm::translate(glm::mat4(1.0f), a->InterpolatePosition(time)) *
                glm::toMat4(a->InterpolateRotation(time)) *
                glm::scale(glm::mat4(1.0f), a->InterpolateScale(time));
        }

        int idx = it->second;

        // ✅ ROOT MOTION — STRIP BEFORE GLOBAL
        if (idx == skeleton->rootBoneIndex)
        {
            glm::vec3 curr = glm::vec3(local[3]);
            rootMotionDelta += curr - prevRootPos;
            prevRootPos = curr;

            local[3] = glm::vec4(0,0,0,1); // CRITICAL
        }
    }

    // ✅ ONLY NOW build global
    glm::mat4 global = parent * local;

    if (it != skeleton->boneMapping.end())
    {
        int idx = it->second;

        currBoneWorldPos[idx] = glm::vec3(global[3]);

        // IK in GLOBAL space
        global *= glm::translate(glm::mat4(1.0f), ikOffsets[idx]);

        finalBoneMatrices[idx] =
            global * skeleton->bones[idx].offset;

        std::cout << "Global inverse: "
          << glm::to_string(skeleton->globalInverseTransform) << "\n";

    }

    for (auto& c : node.children)
        EvaluateNode(c, global, nullptr, 0.0f);
}



bool Animator::IsFootPlanted(int bone) const
{
    return std::abs(currBoneWorldPos[bone].y -
                    prevBoneWorldPos[bone].y) < 0.001f;
}

glm::vec3 Animator::GetBoneWorldPosition(int bone,
                                        const glm::mat4& modelMat) const
{
    return glm::vec3(modelMat * glm::vec4(currBoneWorldPos[bone], 1.0f));
}

void Animator::AddIKOffset(int bone, const glm::vec3& offset, float weight)
{
    ikOffsets[bone] += offset * weight;
}

glm::vec3 Animator::ConsumeRootMotion()
{
    glm::vec3 d = rootMotionDelta;
    rootMotionDelta = glm::vec3(0.0f);
    return d;
}

const std::vector<glm::mat4>& Animator::GetFinalBoneMatrices() const
{
    return finalBoneMatrices;
}



