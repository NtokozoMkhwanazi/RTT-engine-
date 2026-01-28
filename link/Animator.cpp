#include "Animator.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/string_cast.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>


// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------

static glm::mat4 RemoveScale(const glm::mat4& m)
{
    glm::vec3 t, s, skew;
    glm::quat r;
    glm::vec4 p;
    glm::decompose(m, s, r, t, skew, p);
    return glm::translate(glm::mat4(1.0f), t) * glm::toMat4(r);
}

static std::string NormalizeBone(const std::string& s)
{
    std::string n = s;
    std::transform(n.begin(), n.end(), n.begin(),
        [](unsigned char c) { return std::tolower(c); });

    size_t colon = n.find(':');
    if (colon != std::string::npos)
        n = n.substr(colon + 1);

    return n;
}


// ------------------------------------------------------------
// Animator
// ------------------------------------------------------------

Animator::Animator(const Skeleton* skel)
    : skeleton(skel),
      current(nullptr),
      next(nullptr),
      time(0.0f),
      blendTime(0.0f),
      blendDuration(0.0f),
      prevRootPos(0.0f),
      rootMotionDelta(0.0f)
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
    if (!anim || anim == current)
        return;

    next = anim;
    blendTime = 0.0f;
    blendDuration = glm::max(duration, 0.001f);
}


void Animator::Update(float dt)
{
    if (!current || !skeleton)
        return;

    // save previous positions
    prevBoneWorldPos = currBoneWorldPos;

    float tps = current->GetTicksPerSecond();
    if (tps <= 0.0f)
        tps = 25.0f;

    time = std::fmod(time + dt * tps, current->GetDuration());

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
    std::string key = node.name;
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
         std::cout << "Skeleton bones:\n";
        for(auto& [name, idx] : skeleton->boneMapping)
            std::cout << "  " << name << " -> " << idx << "\n";

        // ROOT MOTION — strip before global
        if (idx == skeleton->rootBoneIndex)
        {
            glm::vec3 curr = glm::vec3(local[3]);
            rootMotionDelta += curr - prevRootPos;
            prevRootPos = curr;

            local[3] = glm::vec4(0, 0, 0, 1); // critical
        }
    }

    glm::mat4 global = parent * local;

    if (it != skeleton->boneMapping.end())
    {
        int idx = it->second;

        currBoneWorldPos[idx] = glm::vec3(global[3]);

        // IK offset in global space
        global *= glm::translate(glm::mat4(1.0f), ikOffsets[idx]);

        finalBoneMatrices[idx] =
            global * skeleton->bones[idx].offset;
    }




    for (const auto& c : node.children)
        EvaluateNode(c, global, nullptr, 0.0f);
}


glm::vec3 Animator::GetBoneWorldPosition(
    int boneIndex,
    const glm::mat4& model
) const
{
    if (boneIndex < 0 ||
        boneIndex >= (int)currBoneWorldPos.size())
    {
        return glm::vec3(model[3]); // safe fallback
    }

    glm::vec4 world =
        model * glm::vec4(currBoneWorldPos[boneIndex], 1.0f);

    return glm::vec3(world);
}


bool Animator::IsFootPlanted(int bone) const
{
    return std::abs(
        currBoneWorldPos[bone].y -
        prevBoneWorldPos[bone].y
    ) < 0.001f;
}


void Animator::AddIKOffset(int bone, const glm::vec3& offset, float weight)
{
    if (bone < 0 || bone >= (int)ikOffsets.size())
        return;

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

