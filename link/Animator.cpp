#include "Animator.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/string_cast.hpp>
#include "BoneName.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>


// No local normalization - use canonical NormalizeBoneName from BoneName.h

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
    size_t boneCount = (skeleton && !skeleton->bones.empty()) ? skeleton->bones.size() : MAX_BONES;
    finalBoneMatrices.resize(boneCount, glm::mat4(1.0f));
    globalBoneMatrices.resize(boneCount, glm::mat4(1.0f));
    prevBoneWorldPos.resize(boneCount, glm::vec3(0.0f));
    currBoneWorldPos.resize(boneCount, glm::vec3(0.0f));
    ikOffsets.resize(boneCount, glm::vec3(0.0f));
}

void Animator::Play(Animation* anim)
{
    current = anim;
    next = nullptr;
    time = 0.0f;

    // IMPORTANT: reset root motion state when a new animation starts
    prevRootPos = glm::vec3(0.0f);
    rootMotionDelta = glm::vec3(0.0f);
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
    if (!skeleton) return;

        debugForceIdentityScale = false;  // set true to test
    // DEBUG: pause animation and evaluate bind-pose only
    // Toggle this flag to false when re-enabling animations.
    static const bool DEBUG_PAUSE_ANIM = false;
    
    // DEBUG: Force scale to identity (test if Mixamo scale noise is corrupting hierarchy)
    // Toggle to true if legs suddenly improve with scale = (1,1,1)
        debugForceIdentityScale = false;  // set true to test

    Animation* savedCurrent = current;
    if (!DEBUG_PAUSE_ANIM && current)
    {
        time += dt * current->GetTicksPerSecond();
        time = fmod(time, current->GetDuration());
    }
    else
    {
        // Temporarily disable animation lookup so EvaluateNode uses
        // the bind-pose transforms stored in the node hierarchy.
        current = nullptr;
    }

    std::fill(finalBoneMatrices.begin(),
              finalBoneMatrices.end(),
              glm::mat4(1.0f));

    globalBoneMatrices.resize(skeleton->bones.size(), glm::mat4(1.0f));

    // Evaluate pose (bind-pose when paused)
    EvaluateNode(skeleton->rootNode, glm::mat4(1.0f), nullptr, 0.0f);

    // Restore animation pointer if we temporarily disabled it
    current = savedCurrent;

    // Save old bone world positions
    prevBoneWorldPos = currBoneWorldPos;

    // Update bone world positions
    for (size_t i = 0; i < globalBoneMatrices.size(); ++i)
    {
        currBoneWorldPos[i] =
            glm::vec3(globalBoneMatrices[i] * glm::vec4(0, 0, 0, 1));
    }

    // DEBUG: Print first few bone matrices for diagnosis
    static int frameCount = 0;
    if (++frameCount % 120 == 0) {
        std::cout << "\n[BONE DEBUG] Frame " << frameCount << ":\n";
        for (int i = 0; i < std::min(3, (int)finalBoneMatrices.size()); ++i) {
            glm::vec3 pos = glm::vec3(finalBoneMatrices[i][3]);
            
            // Extract scale
            float sx = glm::length(glm::vec3(finalBoneMatrices[i][0]));
            float sy = glm::length(glm::vec3(finalBoneMatrices[i][1]));
            float sz = glm::length(glm::vec3(finalBoneMatrices[i][2]));
            
            std::cout << "  B" << i << " pos=" << glm::to_string(pos) 
                      << " scale=(" << sx << ", " << sy << ", " << sz << ")\n";
        }
        // Debug: specific leg bone world positions (if available)
        auto printBoneWorld = [&](int idx){
            if (idx >= 0 && idx < (int)currBoneWorldPos.size())
                std::cout << "  W B"<<idx<<"="<<glm::to_string(currBoneWorldPos[idx])<<"\n";
        };
        std::cout << "[LEG WORLD POS]" << std::endl;
        for (int b = 55; b <= 59; ++b) printBoneWorld(b);
        for (int b = 60; b <= 64; ++b) printBoneWorld(b);
    }

    // -------- ROOT MOTION (ONLY HERE) --------
    rootMotionDelta = glm::vec3(0.0f);

    if (skeleton->rootBoneIndex >= 0 &&
        skeleton->rootBoneIndex < (int)currBoneWorldPos.size())
    {
        glm::vec3 currRoot = currBoneWorldPos[skeleton->rootBoneIndex];

        // First frame: initialize
        if (prevRootPos == glm::vec3(0.0f))
        {
            prevRootPos = currRoot;
            rootMotionDelta = glm::vec3(0.0f);
        }
        else
        {
            rootMotionDelta = currRoot - prevRootPos;
            prevRootPos = currRoot;
        }

        // OPTIONAL: remove vertical root motion
         //rootMotionDelta.y = 0.0f;
    }
}

void Animator::EvaluateNode(
    const AssimpNodeData& node,
    const glm::mat4& parent,
    Animation* blendAnim,
    float blendFactor)
{
    std::string name = NormalizeBoneName(node.name);

    glm::mat4 bindLocal = node.transform;
    glm::mat4 localTransform = bindLocal;  // Default to bind pose

    // Apply animation: use animated rotation/scale, NEVER use position keys
    // Position comes from hierarchy and offset matrix only
    if (current)
    {
        if (const BoneAnimation* boneAnim = current->GetBoneAnimation(name))
        {
            // Only animate rotation and scale - NOT position
            // Mixamo position keys are in world-space/accumulated space, not local
            glm::quat rot = boneAnim->InterpolateRotation(time);
            glm::vec3 scale = boneAnim->InterpolateScale(time);
            
            if (debugForceIdentityScale) {
                scale = glm::vec3(1.0f);
            }

            // Extract bind pose position (from local hierarchy)
            glm::vec3 pos = glm::vec3(bindLocal[3]);

            // Rebuild local transform: bind position + animated rotation/scale
            localTransform =
                glm::translate(glm::mat4(1.0f), pos) *
                glm::mat4_cast(rot) *
                glm::scale(glm::mat4(1.0f), scale);
        }
    }

    // ✅ CORRECT for GLM column-major: parent * local
    glm::mat4 globalTransform = parent * localTransform;

    int boneIndex = node.boneIndex;
    if (boneIndex != -1)
    {
        // Apply IK offsets AFTER animation
        glm::mat4 ikOffsetMat =
            glm::translate(glm::mat4(1.0f), ikOffsets[boneIndex]);

        glm::mat4 finalGlobal = globalTransform * ikOffsetMat;

        // Store world transform (for root motion + debug)
        globalBoneMatrices[boneIndex] = finalGlobal;

        // CRITICAL FIX: Do NOT multiply by offset for bone transforms!
        // Offset matrices in Assimp are for VERTEX skinning in the shader.
        // The skeleton hierarchy position is already in globalAnimated.
        // Applying offset here causes double-translation.
        finalBoneMatrices[boneIndex] = skeleton->globalInverseTransform * finalGlobal;
        
        // DEBUG: Print hierarchy chain
        static bool debugPrinted = false;
        if (!debugPrinted && time < 0.05f)
        {
            if (name.find("hips") != std::string::npos ||
                name.find("rightupleg") != std::string::npos ||
                name.find("rightleg") != std::string::npos ||
                name.find("rightfoot") != std::string::npos)
            {
                glm::vec3 localPos = glm::vec3(bindLocal[3]);
                glm::vec3 parentPos = glm::vec3(parent[3]);
                glm::vec3 globalPos = glm::vec3(globalTransform[3]);
                glm::vec3 offsetPos = glm::vec3(skeleton->bones[boneIndex].offset[3]);
                glm::vec3 finalPos = glm::vec3(finalBoneMatrices[boneIndex][3]);
                
                std::cout << "[CHAIN] '" << name << "' (B" << boneIndex << ")\n"
                          << "  local_Y  = " << localPos.y << ",  parentGlobal_Y=" << parentPos.y 
                          << ",  global_Y=" << globalPos.y << ",  offset_Y=" << offsetPos.y 
                          << ",  final_Y=" << finalPos.y << "\n\n";
            }
        }
    }

    for (const auto& child : node.children)
        EvaluateNode(child, globalTransform, blendAnim, blendFactor);
}

glm::vec3 Animator::GetBoneWorldPosition(int bone, const glm::mat4& modelMat) const
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

void Animator::AddIKOffset(int bone, const glm::vec3& offset, float weight)
{
    if (bone < 0 || bone >= (int)ikOffsets.size()) return;
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

