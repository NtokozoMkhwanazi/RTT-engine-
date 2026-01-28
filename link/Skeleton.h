#pragma once
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>
#include "AnimationTypes.h"

#include <algorithm>
#include <cctype>

#define MAX_BONES 120

inline std::string NormalizeBones(const std::string& input)
{
    std::string n = input;
    std::transform(n.begin(), n.end(), n.begin(),
        [](unsigned char c) { return std::tolower(c); });

    size_t colon = n.find(':');
    if (colon != std::string::npos)
        n = n.substr(colon + 1);

    return n;
}

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
        auto it = boneMapping.find(NormalizeBones(name));
        return (it != boneMapping.end()) ? it->second : -1;
    }
};

