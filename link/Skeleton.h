#pragma once
#include <glm/glm.hpp>
#include <map>
#include <string>
#include "AnimationTypes.h"

#define MAX_BONES 120

struct BoneInfo {
    glm::mat4 offset;       // inverse bind pose
    glm::vec3 bindTranslation;
  glm::mat4 bindTransform; 

};



struct Skeleton {
    AssimpNodeData rootNode;
    std::map<std::string, int> boneMapping;
    std::vector<BoneInfo> bones;
    glm::mat4 globalInverseTransform;

   int rootBoneIndex = -1; 

};

