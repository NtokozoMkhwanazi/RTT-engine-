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

    // STATE-SPACE REST ZEROING PASS:
    // If the animation player registers micro-velocity below 0.02 m/s, force-
    // clamp to zero. The old 0.09 threshold masked real micro-velocity during
    // walk contact transitions (0.02-0.09 m/s) that the Schmitt trigger needs
    // to see to decide plant/release — lowering it to match the at-rest
    // threshold (0.02) so only true noise is zeroed.
    float currentSpeed = glm::length(newVel);
    glm::vec3 currentAnkleVel = newVel;
    if (currentSpeed < 0.02f) {
        currentAnkleVel = glm::vec3(0.0f);
        currentSpeed = 0.0f;
    }
    velocity = currentAnkleVel;

    float distanceToFloor = position.y - groundHeight;

    // --- STATISTICAL VARIANCE SCHMITT-TRIGGER ---
    // Online Exponential Moving Statistical aggregation (Welford variant).
    // Gain lowered from 0.15 to 0.08 for more stable variance at low speeds
    // (the 0.15 gain made speedVariance flutter around the 0.002 threshold
    // frame-to-frame, causing rapid plant/release oscillation).
    const float kVarianceGain = 0.08f;
    float previousMean = speedMean;

    speedMean = glm::mix(speedMean, currentSpeed, kVarianceGain);
    speedVariance = glm::mix(speedVariance,
                             (currentSpeed - previousMean) * (currentSpeed - speedMean),
                             kVarianceGain);

    bool isCloseToGround = distanceToFloor <= config.footPlantedHeightThreshold;

    // Schmitt trigger with hysteresis. The old dead zone was 0.02-0.35 m/s —
    // far too wide for walk speeds, where foot contact-transition velocities
    // sit at 0.05-0.15 m/s (squarely in the dead zone, so the foot state
    // never updated → jitter). The at-rest band is tightened (0.01 for speed,
    // 0.001 for variance) and the active band is lowered to 0.08 m/s (just
    // above the velocity-zeroing floor) so a lifting foot releases cleanly
    // instead of sitting in limbo. The variance thresholds scale with the
    // speed thresholds to keep the ratio consistent.
    bool signalIsAtRest  = (speedVariance < 0.001f) && (currentSpeed < 0.01f);
    bool signalIsActive  = (currentSpeed > 0.08f) || (speedVariance > 0.005f);

    if (isLocked) {
        if (signalIsActive || !isCloseToGround) {
            isLocked = false;
            lockWeight = 0.0f;
            releaseTimer = 0.10f; // 100ms blending window
        } else {
            lockWeight = 1.0f;
            releaseTimer = 0.0f;
            plantTimer += dt;  // NOW incremented — was dead code before
        }
    } else {
        if (isCloseToGround && signalIsAtRest) {
            isLocked = true;
            lockWeight = 1.0f;
            plantedWorldPos = position;
            plantedWorldPos.y = groundHeight; // Snap strictly to floor
            releaseTimer = 0.0f;
            plantTimer = 0.0f;  // Reset on plant
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
