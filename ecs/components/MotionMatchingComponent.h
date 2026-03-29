#pragma once

#include "../ECS.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>

namespace ecs {

/**
 * Motion Matching Component - For entities using motion matching
 */
struct MotionMatchingComponent : public Component {
    // Current state
    glm::vec3 rootVelocity{0.0f};
    glm::vec3 rootAcceleration{0.0f};
    glm::vec2 trajectoryDirection{0.0f, 1.0f};
    float trajectorySpeed = 0.0f;
    
    // Pose matching
    float poseMatchThreshold = 0.1f;
    float velocityMatchThreshold = 0.5f;
    
    // Database
    int motionDatabaseID = -1;
    int currentClipID = -1;
    float currentClipTime = 0.0f;
    
    // Blending
    bool isBlending = false;
    float blendTime = 0.0f;
    float blendDuration = 0.3f;
    int blendFromClip = -1;
    int blendToClip = -1;
    
    // Settings
    bool enabled = true;
    bool useTrajectoryPrediction = true;
    bool useFootPlanting = true;
    
    MotionMatchingComponent() = default;
};

/**
 * Motion Matching Database Component - References a motion database
 */
struct MotionDatabaseComponent : public Component {
    std::string databasePath;
    int databaseID = -1;
    bool isLoaded = false;
    
    MotionDatabaseComponent() = default;
    MotionDatabaseComponent(const std::string& path) : databasePath(path) {}
};

} // namespace ecs
