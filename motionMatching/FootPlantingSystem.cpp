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
                                 const MotionMatchingConfig& config,
                                 float dt) {
    if (!initialized) return;
    
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
    
    if (foot.isLocked && foot.plantTimer > 0.05f) {
        // Foot is planted - use locked position
        return foot.plantedWorldPos;
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
    if (leftFoot.isLocked) {
       // animator->SetBoneIK(leftFoot.footBone, leftFoot.plantedWorldPos);
    }
    if (rightFoot.isLocked) {
        //animator->SetBoneIK(rightFoot.footBone, rightFoot.plantedWorldPos);
    }
}

std::string FootPlantingSystem::GetDebugInfo() const {
    std::string info = "Foot Planting: ";
    info += "L=" + std::string(leftFoot.isLocked ? "LOCK" : "free");
    info += " R=" + std::string(rightFoot.isLocked ? "LOCK" : "free");
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
    float dt)
{
    position = newPos;

    // STATE-SPACE REST ZEROING PASS (Fix 1 from updated todo):
    // If the animation player registers micro-velocity below 0.09 m/s, force-
    // clamp the velocity vector to pure zero. This stops the feedback shockwave
    // from the over-reach solver (d > maxReach) from generating false velocity
    // spikes that bypass the Schmitt-Trigger gates and re-break the lock.
    float currentSpeed = glm::length(newVel);
    glm::vec3 currentAnkleVel = newVel;
    // FIX (forAnime.txt Fix 2): Raise the deadband from 0.09 to 0.25 so
    // sub-pixel compression/idle noise is flattened before it can trip the
    // variance Schmitt-Trigger and cause foot lock jitter.
    if (currentSpeed < 0.25f) {
        currentAnkleVel = glm::vec3(0.0f);
        currentSpeed = 0.0f;
    }
    velocity = currentAnkleVel;

    float distanceToFloor = position.y - groundHeight;

    // --- STATISTICAL VARIANCE SCHMITT-TRIGGER ---
    // Online Exponential Moving Statistical aggregation (Welford variant).
    const float kVarianceGain = 0.15f;
    float previousMean = speedMean;

    speedMean = glm::mix(speedMean, currentSpeed, kVarianceGain);
    speedVariance = glm::mix(speedVariance,
                             (currentSpeed - previousMean) * (currentSpeed - speedMean),
                             kVarianceGain);

    bool isCloseToGround = distanceToFloor <= config.footPlantedHeightThreshold;

    // Separate boundary thresholds strictly to ignore floating point log flutter
    bool signalIsAtRest  = (speedVariance < 0.002f) && (currentSpeed < 0.02f);
    bool signalIsActive  = (currentSpeed > 0.35f) || (speedVariance > 0.01f);

    if (isLocked) {
        if (signalIsActive || !isCloseToGround) {
            isLocked = false;
            releaseTimer = 0.10f; // Let the cooldown timer handle decay smoothly
        } else {
            lockWeight = 1.0f;
            releaseTimer = 0.0f;

            // FIX (forAnime.txt Fix 3): Standing Anchor Lock — when the
            // character is completely stationary (zero velocity, negligible
            // variance), zero out the ankle velocity entirely. This cuts off
            // sub-pixel FBX keyframe interpolation chatter before it can
            // propagate into the sensitive cosA analytical leg solver curve,
            // where a 0.0001 change causes multi-degree knee angle swings.
            if (currentSpeed == 0.0f && speedVariance < 0.0001f) {
                velocity = glm::vec3(0.0f);
            }
        }
    } else {
        if (isCloseToGround && signalIsAtRest) {
            isLocked = true;
            lockWeight = 1.0f;
            plantedWorldPos = position;
            plantedWorldPos.y = groundHeight; // Snap strictly to floor
            releaseTimer = 0.0f;
        } else {
            if (releaseTimer > 0.0f) {
                releaseTimer -= dt;
                lockWeight = glm::clamp(releaseTimer / 0.10f, 0.0f, 1.0f);
            } else {
                lockWeight = 0.0f;
            }
        }
    }

    UpdateDebug();
}

void FootPlantingSystem::FootState::UpdateDebug() {
    debug.isLocked = isLocked;
    debug.position = position;
    debug.plantedWorldPos = plantedWorldPos;
    debug.velocity = glm::length(velocity);
    debug.height = position.y;
    debug.plantTimer = plantTimer;
}
