#include "Animator.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/string_cast.hpp>
#include "boneSystem/BoneName.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>

// No local normalization - use canonical NormalizeBoneName from BoneName.h

// ------------------------------------------------------------
// Animator
// ------------------------------------------------------------
Animator::Animator(const Skeleton *skel)
    : skeleton(skel),
      current(nullptr),
      next(nullptr),
      time(0.0f),
      blendTime(0.0f),
      blendDuration(0.0f),
      prevRootPos(0.0f),
      rootMotionDelta(0.0f)
{
    size_t boneCount = (skeleton && !skeleton->bones.empty()) ? skeleton->bones.size() : MAX_BONES;
    finalBoneMatrices.resize(boneCount, glm::mat4(1.0f));
    globalBoneMatrices.resize(boneCount, glm::mat4(1.0f));
    prevBoneWorldPos.resize(boneCount, glm::vec3(0.0f));
    currBoneWorldPos.resize(boneCount, glm::vec3(0.0f));
    ikOffsets.resize(boneCount, glm::vec3(0.0f));
    
    std::cout << "[Animator] Created with skeleton containing " << boneCount << " bones\n";
    if (skeleton) {
        std::cout << "[Animator] Skeleton root bone index: " << skeleton->rootBoneIndex << "\n";
        std::cout << "[Animator] Global inverse transform:\n";
        std::cout << "  " << glm::to_string(skeleton->globalInverseTransform[0]) << "\n";
        std::cout << "  " << glm::to_string(skeleton->globalInverseTransform[1]) << "\n";
        std::cout << "  " << glm::to_string(skeleton->globalInverseTransform[2]) << "\n";
        std::cout << "  " << glm::to_string(skeleton->globalInverseTransform[3]) << "\n";
    }
}

void Animator::Play(Animation *anim)
{
    current = anim;
    next = nullptr;
    time = 0.0f;

    // IMPORTANT: reset root motion state when a new animation starts
    prevRootPos = glm::vec3(0.0f);
    rootMotionDelta = glm::vec3(0.0f);
    hasPrevRoot = false;
}

void Animator::BlendTo(Animation *anim, float duration)
{
    if (!anim || anim == current)
        return;

    next = anim;
    blendTime = 0.0f;
    blendDuration = glm::max(duration, 0.001f);
}

void Animator::Update(float dt)
{
    if (!skeleton) {
        std::cout << "[Animator::Update] ERROR: No skeleton!\n";
        return;
    }

    static bool DEBUG_PAUSE_ANIM = false;
    debugForceIdentityScale = false;

    std::cout << "[Animator::Update] dt=" << dt << ", time=" << time << ", current anim=" << (current ? current->name : "NULL") << "\n";

    // Save previous positions BEFORE overwriting
    prevBoneWorldPos = currBoneWorldPos;

    Animation *savedCurrent = current;

    if (!DEBUG_PAUSE_ANIM && current)
    {
        time += dt * current->GetTicksPerSecond();
        time = fmod(time, current->GetDuration());
        std::cout << "[Animator::Update] Animation time updated: " << time << "/" << current->GetDuration() << "\n";
    }
    else
    {
        current = nullptr;
        std::cout << "[Animator::Update] Animation paused or null\n";
    }

    finalBoneMatrices.assign(skeleton->bones.size(), glm::mat4(1.0f));
    globalBoneMatrices.assign(skeleton->bones.size(), glm::mat4(1.0f));

    std::cout << "[Animator::Update] About to evaluate nodes, skeleton root node name: " << skeleton->rootNode.name << "\n";
    std::cout << "[Animator::Update] Number of skeleton bones: " << skeleton->bones.size() << "\n";

    EvaluateNode(skeleton->rootNode, glm::mat4(1.0f), nullptr, time);

    current = savedCurrent;

    // Compute current bone positions
    for (size_t i = 0; i < globalBoneMatrices.size(); ++i)
    {
        currBoneWorldPos[i] =
            glm::vec3(globalBoneMatrices[i] * glm::vec4(0, 0, 0, 1));
    }

    // Root motion
    rootMotionDelta = glm::vec3(0.0f);

    int rootIdx = skeleton->rootBoneIndex;
    if (rootIdx >= 0 && rootIdx < (int)currBoneWorldPos.size())
    {
        glm::vec3 currRoot = currBoneWorldPos[rootIdx];
        std::cout << "[Animator::Update] Root bone position: " << glm::to_string(currRoot) << "\n";

        if (!hasPrevRoot)
        {
            prevRootPos = currRoot;
            hasPrevRoot = true;
            std::cout << "[Animator::Update] Initialized root position\n";
        }
        else
        {
            rootMotionDelta = currRoot - prevRootPos;
            prevRootPos = currRoot;
            std::cout << "[Animator::Update] Root motion delta: " << glm::to_string(rootMotionDelta) << "\n";
        }

        rootMotionDelta.y = 0.0f;
    }
    
    std::cout << "[Animator::Update] Completed update, final matrices size: " << finalBoneMatrices.size() << "\n";
}

void Animator::EvaluateNode(
    const AssimpNodeData& node,
    const glm::mat4& parent,
    Animation* blendAnim,
    float blendFactor)
{
    std::string name = NormalizeBoneName(node.name);

    std::cout << "[EvaluateNode] Processing bone: " << name << ", node.boneIndex: " << node.boneIndex << "\n";

    glm::mat4 bindLocal = node.transform;

    // ---- Decompose bind pose ----
    glm::vec3 bindScale, bindPos, skew;
    glm::quat bindRot;
    glm::vec4 perspective;

    glm::decompose(bindLocal, bindScale, bindRot, bindPos, skew, perspective);
    bindRot = glm::normalize(bindRot);

    std::cout << "[EvaluateNode] Bind pose - Pos: " << glm::to_string(bindPos) 
              << ", Rot: " << glm::to_string(bindRot) 
              << ", Scale: " << glm::to_string(bindScale) << "\n";

    // ---- Start from bind pose ----
    glm::vec3 pos   = bindPos;
    glm::quat rot   = bindRot;
    glm::vec3 scale = bindScale;

    // ---- Apply animation (override bind channels) ----
    if (current)
    {
        std::cout << "[EvaluateNode] Current animation: " << current->name << "\n";
        if (const BoneAnimation* boneAnim = current->GetBoneAnimation(name))
        {
            std::cout << "[EvaluateNode] Found bone animation for: " << name << "\n";
            
            // Animation processing for all bones

            if (boneAnim->HasRotationAnimation()) {
                rot = boneAnim->InterpolateRotation(time);
                std::cout << "[EvaluateNode] Applied rotation animation to: " << name << "\n";
            }

            if (boneAnim->HasScaleAnimation()) {
                scale = boneAnim->InterpolateScale(time);
                std::cout << "[EvaluateNode] Applied scale animation to: " << name << "\n";
            }

            if (debugForceIdentityScale)
                scale = glm::vec3(1.0f);

            if (boneAnim->HasPositionAnimation()) {
                pos = boneAnim->InterpolatePosition(time);
                std::cout << "[EvaluateNode] Applied position animation to bone: " << name << "\n";
            } else {
                std::cout << "[EvaluateNode] Kept bind pose position for bone: " << name << "\n";
            }
        } else {
            std::cout << "[EvaluateNode] No bone animation found for: " << name << "\n";
        }
    } else {
        std::cout << "[EvaluateNode] No current animation\n";
    }

    // ---- Rebuild local transform ----
    glm::mat4 localTransform =
        glm::translate(glm::mat4(1.0f), pos) *
        glm::mat4_cast(rot) *
        glm::scale(glm::mat4(1.0f), scale);

    std::cout << "[EvaluateNode] Local transform built for: " << name << "\n";

    // ---- Global ----
    glm::mat4 globalTransform = parent * localTransform;

    int boneIndex = node.boneIndex;

    if (boneIndex != -1)
    {
        std::cout << "[EvaluateNode] Processing bone index: " << boneIndex << " for: " << name << "\n";

        // Store the global transform for this bone before applying IK offset
        glm::mat4 globalTransformForChildren = globalTransform;

        // Apply IK offset only for the final bone matrix (not for hierarchy)
        glm::mat4 ikOffsetMat =
            glm::translate(glm::mat4(1.0f), ikOffsets[boneIndex]);

        glm::mat4 finalGlobal = globalTransform * ikOffsetMat;

        globalBoneMatrices[boneIndex] = finalGlobal;

        finalBoneMatrices[boneIndex] =
            skeleton->globalInverseTransform *
            finalGlobal *
            skeleton->bones[boneIndex].offset;

        std::cout << "[EvaluateNode] Final bone matrix computed for index: " << boneIndex << "\n";

        // Pass the globalTransform (WITHOUT IK offset) to children to maintain proper hierarchy
        for (const auto& child : node.children) {
            std::cout << "[EvaluateNode] Recursing to child of: " << name << "\n";
            EvaluateNode(child, globalTransformForChildren, blendAnim, blendFactor);
        }
    } else {
        std::cout << "[EvaluateNode] Bone index is -1 for: " << name << ", skipping matrix computation\n";
        
        // Still pass the global transform to children for proper hierarchy
        for (const auto& child : node.children) {
            std::cout << "[EvaluateNode] Recursing to child of: " << name << "\n";
            EvaluateNode(child, globalTransform, blendAnim, blendFactor);
        }
    }
}


glm::vec3 Animator::GetBoneWorldPosition(int bone, const glm::mat4 &modelMat) const
{
    if (bone < 0 || bone >= (int)globalBoneMatrices.size())
        return glm::vec3(0.0f);

    glm::mat4 world = modelMat * globalBoneMatrices[bone];
    return glm::vec3(world * glm::vec4(0, 0, 0, 1));
}

bool Animator::IsFootPlanted(int bone) const
{
    return std::abs(currBoneWorldPos[bone].y - prevBoneWorldPos[bone].y) < 0.001f;
}

void Animator::AddIKOffset(int bone, const glm::vec3 &offset, float weight)
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

const std::vector<glm::mat4> &Animator::GetFinalBoneMatrices() const
{
    std::cout << "[GetFinalBoneMatrices] Returning " << finalBoneMatrices.size() << " matrices\n";
    if (!finalBoneMatrices.empty()) {
        std::cout << "[GetFinalBoneMatrices] First matrix: " << glm::to_string(finalBoneMatrices[0][0]) << "\n";
        std::cout << "[GetFinalBoneMatrices] Last matrix: " << glm::to_string(finalBoneMatrices.back()[0]) << "\n";
    }
    return finalBoneMatrices;
}
