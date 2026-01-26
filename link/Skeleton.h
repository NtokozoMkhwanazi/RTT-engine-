#pragma once
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>
#include "AnimationTypes.h"

#define MAX_BONES 120

struct BoneInfo {
    glm::vec3 bindTranslation{0.0f};
    glm::mat4 offset{1.0f};
    glm::mat4 bindTransform{1.0f};
    int id = -1;
};

struct Skeleton {
    AssimpNodeData rootNode;
    std::map<std::string, int> boneMapping;
    std::vector<BoneInfo> bones;
    glm::mat4 globalInverseTransform{1.0f};
    int rootBoneIndex = -1;

    // ✅ FIXED
    int GetBoneIndex(const std::string& name) const
    {
        auto it = boneMapping.find(name);
        return (it != boneMapping.end()) ? it->second : -1;
    }
};

