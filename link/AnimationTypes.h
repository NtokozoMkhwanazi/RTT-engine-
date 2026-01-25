// AnimationTypes.h
#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <map>

// Represents a single keyframe for a bone
struct BoneKey {
    float timeStamp;         // Time in seconds
    glm::vec3 position;      // Translation
    glm::quat rotation;      // Rotation
    glm::vec3 scale;         // Scaling
};

// Represents animation data for a single bone


// Represents a node in the hierarchy of the skeleton
struct AssimpNodeData {
    std::string name;
    glm::mat4 transform;
    std::vector<AssimpNodeData> children;
    int boneIndex = -1;
};

