#pragma once
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>
#include "../animationSystem/AnimationTypes.h"
#include "BoneName.h"

#define MAX_BONES 120

struct BoneInfo {
    glm::vec3 bindTranslation{0.0f};
    glm::mat4 offset{1.0f};
    glm::mat4 bindTransform{1.0f};
    int id = -1;
};

struct HumanoidBones {
    int hips;
    int leftFoot;
    int rightFoot;
    int leftToe;
    int rightToe;
};

struct Skeleton {
    AssimpNodeData rootNode;
    std::map<std::string, int> boneMapping;
    std::vector<BoneInfo> bones;
    glm::mat4 globalInverseTransform{1.0f};
    int rootBoneIndex = -1;
    

    // Use canonical normalization
    int GetBoneIndex(const std::string& name) const
    {
        auto it = boneMapping.find(NormalizeBoneName(name));
        return (it != boneMapping.end()) ? it->second : -1;
    }
};

