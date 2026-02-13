#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "Skeleton.h"
#include "DebugDraw.h"

void BuildSkeletonLines(
    const AssimpNodeData& node,
    const glm::mat4& parentTransform,
    int parentBoneIndex,
    const Skeleton& skeleton,
    const std::vector<glm::mat4>& finalBones,
    std::vector<DebugLine>& outLines
);


