#include "FootPlantingSystem.h"
#include "../animationSystem/Animator.h"
#include <iostream>

FootPlantingSystem::FootPlantingSystem() {}

void FootPlantingSystem::Initialize(const Skeleton* skeleton) {
    if (!skeleton) return;
    
    // Find foot and toe bones
    leftFoot.footBone = skeleton->GetBoneIndex("leftfoot");
    leftFoot.toeBone = skeleton->GetBoneIndex("lefttoebase");
    
    rightFoot.footBone = skeleton->GetBoneIndex("rightfoot");
    rightFoot.toeBone = skeleton->GetBoneIndex("righttoebase");
    
    initialized = (leftFoot.footBone >= 0 && rightFoot.footBone >= 0);
    
    if (initialized) {
        std::cout << "[FootPlantingSystem] Initialized: Left=" << leftFoot.footBone
                  << ", Right=" << rightFoot.footBone << "\n";
    } else {
        std::cerr << "[FootPlantingSystem] WARNING: Could not find foot bones!\n";
    }
}

void FootPlantingSystem::Update(const glm::vec3& leftFootPos,
                                 const glm::vec3& rightFootPos,
                                 const glm::vec3& leftFootVel,
                                 const glm::vec3& rightFootVel,
                                 float groundHeight,
                                 const MotionMatchingConfig& config) {
    if (!initialized) return;
    
    // Update foot states (static dt for now)
    float dt = 0.016f;
    
    leftFoot.Update(leftFootPos, leftFootVel, groundHeight, config, dt);
    rightFoot.Update(rightFootPos, rightFootVel, groundHeight, config, dt);
}

bool FootPlantingSystem::ShouldPlant(const glm::vec3& position,
                                      const glm::vec3& velocity,
                                      float groundHeight,
                                      const MotionMatchingConfig& config) const {
    // Foot must be near ground
    float footHeight = position.y - groundHeight;
    if (footHeight > config.footPlantHeightThreshold) {
        return false;  // Too high
    }
    
    // Foot must be moving slowly
    float speed = glm::length(velocity);
    if (speed > config.footPlantThreshold) {
        return false;  // Moving too fast
    }
    
    return true;
}

bool FootPlantingSystem::ShouldRelease(const FootState& foot,
                                        const glm::vec3& newVel,
                                        const MotionMatchingConfig& config) const {
    // Foot must have been planted for minimum time
    if (foot.plantTimer < 0.1f) {
        return false;  // Just planted, don't release yet
    }
    
    // Foot is moving up or fast
    if (newVel.y > 0.5f) {
        return true;  // Lifting
    }
    
    float speed = glm::length(newVel);
    if (speed > config.footPlantThreshold * 2.0f) {
        return true;  // Moving fast
    }
    
    return false;
}

glm::vec3 FootPlantingSystem::GetIKTarget(const glm::vec3& animFootPos,
                                           bool isLeftFoot) const {
    const FootState& foot = isLeftFoot ? leftFoot : rightFoot;
    
    if (foot.planted && foot.plantTimer > 0.05f) {
        // Foot is planted - use locked position
        return foot.plantPosition;
    } else {
        // Foot is free - use animation position
        return animFootPos;
    }
}

void FootPlantingSystem::ApplyFootIK(Animator* animator,
                                      const glm::mat4& modelMatrix,
                                      float dt) const {
    if (!initialized || !animator) return;
    
    // Get current foot positions from animation
    // In a full implementation, we'd sample the skeleton and get foot positions
    // For now, this is a placeholder for the IK application
    
    // Apply IK if feet are planted
    if (leftFoot.planted) {
        // animator->SetBoneIK(leftFoot.footBone, leftFoot.plantPosition);
    }
    if (rightFoot.planted) {
        // animator->SetBoneIK(rightFoot.footBone, rightFoot.plantPosition);
    }
}

std::string FootPlantingSystem::GetDebugInfo() const {
    std::string info = "Foot Planting: ";
    info += "L=" + std::string(leftFoot.planted ? "PLANTED" : "FREE");
    info += " R=" + std::string(rightFoot.planted ? "PLANTED" : "FREE");
    return info;
}

// ============================================================================
// FOOT STATE UPDATE
// ============================================================================

void FootPlantingSystem::FootState::Update(
    const glm::vec3& newPos,
    const glm::vec3& newVel,
    float groundHeight,
    const MotionMatchingConfig& config,
    float dt) {
    
    position = newPos;
    velocity = newVel;
    
    float speed = glm::length(newVel);
    float footHeight = position.y - groundHeight;
    
    if (planted) {
        // Currently planted - check if should release
        if (plantTimer < 0.1f) {
            return;  // Just planted, don't release yet
        }
        
        // Foot is moving up or fast
        if (newVel.y > 0.5f) {
            planted = false;
            plantTimer = 0.0f;
            releaseTimer = 0.1f;  // Cooldown
            return;
        }
        
        float speed = glm::length(newVel);
        if (speed > config.footPlantThreshold * 2.0f) {
            planted = false;
            plantTimer = 0.0f;
            releaseTimer = 0.1f;
            return;
        }
        
        // Stay planted
        plantTimer += dt;
    } else {
        // Currently free - check if should plant
        if (releaseTimer > 0.0f) {
            releaseTimer -= dt;
        } else if (footHeight <= config.footPlantHeightThreshold &&
                   speed <= config.footPlantThreshold) {
            // Plant the foot
            planted = true;
            plantPosition = position;
            plantTimer = 0.0f;
        }
    }
    
    UpdateDebug();
}

void FootPlantingSystem::FootState::UpdateDebug() {
    debug.planted = planted;
    debug.position = position;
    debug.plantPosition = plantPosition;
    debug.velocity = glm::length(velocity);
    debug.height = position.y;
    debug.plantTimer = plantTimer;
}
