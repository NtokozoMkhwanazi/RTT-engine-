#pragma once
#include <glad/glad.h>

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

    void UploadToTexture(GLuint texID);
    void Upload(Shader& shader);

    // -------- FOOT LOCKING / IK --------
    void AddIKOffset(int bone, const glm::vec3& offset, float weight);
    bool IsFootPlanted(int bone) const;
    glm::vec3 GetBoneWorldPosition(int bone, const glm::mat4& modelMat) const;

    glm::vec3 ConsumeRootMotion();

    const std::vector<glm::mat4>& GetFinalBoneMatrices() const;

private:
    const Skeleton* skeleton = nullptr;
    Animation* current = nullptr;
    Animation* next = nullptr;

    std::vector<glm::vec3> m_BoneWorldPositions;

    float time = 0.0f;
    float blendTime = 0.0f;
    float blendDuration = 0.0f;

    glm::vec3 rootMotionDelta{0.0f};
    glm::vec3 prevRootPos{0.0f};

    std::vector<glm::mat4> finalBoneMatrices;

    // -------- FOOT STATE --------
    std::vector<glm::vec3> prevBoneWorldPos;
    std::vector<glm::vec3> currBoneWorldPos;
    std::vector<glm::vec3> ikOffsets;

    void EvaluateNode(
        const AssimpNodeData& node,
        const glm::mat4& parent,
        Animation* blendAnim,
        float blendFactor
    );

    //static std::string NormalizeName(const std::string& name);
};

