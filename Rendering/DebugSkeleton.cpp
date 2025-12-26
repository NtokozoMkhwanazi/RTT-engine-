#include "DebugSkeleton.h"
#include <iostream>


static std::string NormalizeName(const std::string& name){
    size_t pos = name.find('|');
    return(pos == std::string::npos) ? name : name.substr(pos + 1);    

}
// REMOVED 'inline' here so it links correctly from main.cpp
void BuildSkeletonLines(
    const AssimpNodeData& node,
    const glm::mat4& parentTransform,
    int parentBoneIndex,
    const Skeleton& skeleton,
    const std::vector<glm::mat4>& finalBones,
    std::vector<DebugLine>& outLines)
{
    
    
glm::mat4 global = parentTransform * node.transform;
    
    // FIX 1: Default to -1 (no bone) instead of 1
   
    int currentBoneIndex = node.boneIndex; 
    std::string cleanName = NormalizeName(node.name);
    std::cout<<"Visiting node normalized: "<<cleanName<<std::endl;
    auto it = skeleton.boneMapping.find(cleanName);

    static int debugOnce = 0;
    if(debugOnce < 3){
    std::cout<<"-----NODE NAME RAW------[" <<cleanName<<"]\n";
    std::cout<< "----BoneMapping KEYS ----\n";
      for(auto& [boneName, idx]: skeleton.boneMapping){
        std::cout<<"BoneMap: "<<boneName<<std::endl;
        }
        std::cout<<"-----\n";
        debugOnce++;
    }
  
    if(it != skeleton.boneMapping.end()){
        currentBoneIndex = it->second;
        std::cout<< "Bone Found:"<<cleanName<<"index"<<it->second<<std::endl;
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



