#include "DebugSkeleton.h"
#include "BoneName.h"

// REMOVED 'inline' here so it links correctly from test.cpp
void BuildSkeletonLines(
    const AssimpNodeData& node,
    const glm::mat4& parentTransform,
    int parentBoneIndex,
    const Skeleton& skeleton,
    const std::vector<glm::mat4>& finalBones,
    std::vector<DebugLine>& outLines)
{

    glm::mat4 global = parentTransform * node.transform;

    int currentBoneIndex = node.boneIndex;
    std::string cleanName = NormalizeBoneName(node.name);

    auto it = skeleton.boneMapping.find(cleanName);

    if(it != skeleton.boneMapping.end()){
        currentBoneIndex = it->second;
        // Only draw a line if we have a valid parent bone to connect to
        if(parentBoneIndex != -1 && currentBoneIndex != -1 ){
            glm::vec3 parentPos = glm::vec3(finalBones[parentBoneIndex][3]);
            glm::vec3 nodePos   = glm::vec3(finalBones[currentBoneIndex][3]);

            outLines.push_back({ parentPos, nodePos });
        }
    }

    // Recursion: If this node wasn't a bone, pass the parentBoneIndex down
    // so children can still connect to the last valid ancestor bone.
    for (const auto& child : node.children) {
        BuildSkeletonLines(
            child,
            global,
            (currentBoneIndex == -1) ? parentBoneIndex : currentBoneIndex,
            skeleton,
            finalBones,
            outLines
        );
    }
}



