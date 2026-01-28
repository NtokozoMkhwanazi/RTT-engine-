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

    // lowercase
    std::transform(n.begin(), n.end(), n.begin(),
        [](unsigned char c) { return std::tolower(c); });

    // remove namespace
    size_t colon = n.find(':');
    if (colon != std::string::npos)
        n = n.substr(colon + 1);

    // REMOVE underscores and spaces
    n.erase(std::remove_if(n.begin(), n.end(),
        [](char c) { return c == '_' || c == ' '; }),
        n.end());

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
    if (!current || !skeleton) return;

    time += dt * current->GetTicksPerSecond();
    time = fmod(time, current->GetDuration());

    std::fill(finalBoneMatrices.begin(),
              finalBoneMatrices.end(),
              glm::mat4(1.0f));

    globalBoneMatrices.resize(skeleton->bones.size());

    EvaluateNode(skeleton->rootNode, glm::mat4(1.0f), nullptr, 0.0f);

    prevBoneWorldPos = currBoneWorldPos;

for (size_t i = 0; i < globalBoneMatrices.size(); ++i)
{
    currBoneWorldPos[i] =
        glm::vec3(globalBoneMatrices[i] * glm::vec4(0,0,0,1));
}

}

void Animator::EvaluateNode(
    const AssimpNodeData& node,
    const glm::mat4& parent,
    Animation* blendAnim,
    float blendFactor
) {
    std::string name = NormalizeBone(node.name);
    glm::mat4 localTransform = node.transform;

    // 🎬 APPLY CURRENT ANIMATION
    if (current) {
        if (const BoneAnimation* boneAnim = current->GetBoneAnimation(name)) {
            glm::vec3 pos = boneAnim->InterpolatePosition(time);
            glm::quat rot = boneAnim->InterpolateRotation(time);
            glm::vec3 scale = boneAnim->InterpolateScale(time);

            localTransform =
                glm::translate(glm::mat4(1.0f), pos) *
                glm::mat4_cast(rot) *
                glm::scale(glm::mat4(1.0f), scale);
        }
    }

    glm::mat4 globalTransform = parent * localTransform;

    int boneIndex = skeleton->GetBoneIndex(name);
if (boneIndex != -1) {

    // ✅ STORE REAL WORLD TRANSFORM
    globalBoneMatrices[boneIndex] = globalTransform;

    // skinning matrix (for shader)
    finalBoneMatrices[boneIndex] =
        skeleton->globalInverseTransform *
        globalTransform *
        skeleton->bones[boneIndex].offset;
}

    if (boneIndex != -1)
    {
        std::cout << "Animating bone: " << node.name << "\n";
    }

    for (const auto& child : node.children) {
        EvaluateNode(child, globalTransform, blendAnim, blendFactor);
    }
}


glm::vec3 Animator::GetBoneWorldPosition(
    int bone,
    const glm::mat4& modelMat
) const
{
    if (bone < 0 || bone >= (int)globalBoneMatrices.size())
        return glm::vec3(0.0f);

    glm::mat4 world = modelMat * globalBoneMatrices[bone];

    return glm::vec3(world * glm::vec4(0, 0, 0, 1));
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

