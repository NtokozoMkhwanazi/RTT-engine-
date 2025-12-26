#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <map>

#include "Animation.h"
#include "Shader.h"
#include "Skeleton.h"

class Animator {
public:
    Animator(const Skeleton* skeleton);

    void Play(Animation* animation);
    void BlendTo(Animation* animation, float duration);
    void Update(float dt);

    void Upload(Shader& shader);
    glm::vec3 ConsumeRootMotion();

    const std::vector<glm::mat4>& GetFinalBoneMatrices() const;

private:
    // ---- Core ----
    const Skeleton* skeleton = nullptr;
    Animation* current = nullptr;
    Animation* next = nullptr;

    float time = 0.0f;
    float blendTime = 0.0f;
    float blendDuration = 0.0f;

    glm::vec3 rootMotionDelta{0.0f};
    std::vector<glm::mat4> finalBoneMatrices;

    // ---- Internal ----
    void EvaluatePose();
    void EvaluatePose(Animation* blendAnim, float blendFactor);

    void EvaluateNode(
        const AssimpNodeData& node,
        const glm::mat4& parent,
        Animation* blendAnim,
        float blendFactor
    );

    // ---- Utilities ----
    static std::string NormalizeName(const std::string& name);
};

