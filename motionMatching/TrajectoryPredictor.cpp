#include "TrajectoryPredictor.h"
#include <algorithm>
#include <cmath>

TrajectoryPredictor::TrajectoryPredictor() {}

Trajectory TrajectoryPredictor::Predict(
    const glm::vec3& currentPosition,
    const glm::vec3& currentVelocity,
    float currentRotation,
    const glm::vec2& moveDirection,
    const MotionMatchingConfig& config) const {
    
    Trajectory trajectory;
    trajectory.numPoints = config.trajectoryPoints;
    
    float dt = config.trajectoryDuration / config.trajectoryPoints;
    
    // Convert input direction to world space
    glm::vec3 inputDir;
    inputDir.x = moveDirection.x;
    inputDir.y = 0.0f;
    inputDir.z = moveDirection.y;
    const float inputLen = glm::length(inputDir);
    // CRITICAL: normalizing a zero vector yields NaN, which then poisons every
    // KD-tree distance (dist = sqrt(NaN) = NaN) and silently breaks the pose
    // search whenever the player lets go of the stick (moveDirection == 0).
    // Guard it: no input -> no intended direction.
    if (inputLen > 1e-6f) inputDir /= inputLen;
    else inputDir = glm::vec3(0.0f);
    
    // Rotate input direction by character rotation
    float cosRot = cos(currentRotation);
    float sinRot = sin(currentRotation);
    glm::vec3 worldInputDir;
    worldInputDir.x = inputDir.x * cosRot - inputDir.z * sinRot;
    worldInputDir.z = inputDir.x * sinRot + inputDir.z * cosRot;
    worldInputDir.y = 0.0f;
    
    // Predict future positions assuming constant velocity + input
    glm::vec3 predictedVel = currentVelocity;
    // FIX (refreshed todo Fix 2): When the stick is released AND the character
    // is below the foot-lift velocity threshold, zero the initial prediction
    // velocity immediately instead of letting residual momentum project a ghost
    // trajectory 0.5 s into the future. The loop's 0.25-per-step decay only
    // shrinks velocity exponentially, so at low but non-zero speeds (e.g. 0.3
    // m/s coasting to a stop) the future path still stretches forward and lures
    // the KD-tree toward slow walk clips instead of static Idle — producing
    // root jitter and ankle drift on return to rest. Collapsing the velocity
    // up-front makes the trajectory converge to a point so the matcher cleanly
    // selects idle poses.
    if (glm::length(moveDirection) < 1e-5f &&
        glm::length(currentVelocity) < config.footLiftVelocityThreshold) {
        predictedVel = glm::vec3(0.0f);
    }
    glm::vec3 predictedPos = currentPosition;
    
    for (int i = 0; i < config.trajectoryPoints; i++) {
        // Turn toward the input direction at the CURRENT speed (simplified
        // model). The old code mixed the velocity toward a hardcoded
        // "worldInputDir * 6.0f units/s" - meaningless in the source-unit
        // feature space the matcher searches - so the predicted path collapsed
        // toward a standstill at locomotion speeds and every fast pose was
        // mis-ranked by its trajectory feature. Blending the direction at the
        // current speed keeps the magnitude (turning, not braking).
        const float inputMagnitude = glm::length(moveDirection);
        if (inputMagnitude > 1e-6f) {
            const float curSpeed = glm::length(predictedVel);
            predictedVel = glm::mix(predictedVel, worldInputDir * curSpeed, 0.15f);
        } else {
            // Replace the frame-rate-dependent glm::mix (0.25f/step) with a
            // physical, dt-normalized exponential decay. The fixed 0.25
            // coefficient assumed a hardcoded frame cadence; at 120 Hz it
            // collapsed the future path far too fast, decoupling the predicted
            // trajectory from the capsule's real momentum and triggering the
            // inertialization tug-of-war (hips yanked backward, feet locked)
            // that hyper-extended the legs. exp(-rate*dt) is frame-rate
            // invariant — the decay over a fixed time window is constant
            // regardless of refresh rate. The explicit floor at 0.01 m/s snaps
            // to true zero, killing sub-threshold micro-drift residual.
            const float decelerationRate = 12.0f;
            predictedVel *= std::exp(-decelerationRate * dt);
            if (glm::length(predictedVel) < 0.01f) {
                predictedVel = glm::vec3(0.0f);
            }
        }
        
        // Integrate position
        predictedPos += predictedVel * dt;
        
        // Store trajectory point
        trajectory.positions[i] = predictedPos;
        trajectory.velocities[i] = predictedVel;
        trajectory.directions[i] = atan2(predictedVel.x, predictedVel.z);
    }
    
    return trajectory;
}

Trajectory TrajectoryPredictor::PredictWithAcceleration(
    const glm::vec3& currentPosition,
    const glm::vec3& currentVelocity,
    const glm::vec3& targetVelocity,
    float acceleration,
    const MotionMatchingConfig& config) const {
    
    Trajectory trajectory;
    trajectory.numPoints = config.trajectoryPoints;
    
    float dt = config.trajectoryDuration / config.trajectoryPoints;
    
    glm::vec3 predictedVel = currentVelocity;
    glm::vec3 predictedPos = currentPosition;
    
    for (int i = 0; i < config.trajectoryPoints; i++) {
        // Accelerate toward target velocity
        glm::vec3 velDiff = targetVelocity - predictedVel;
        float accelMagnitude = glm::length(velDiff);
        
        if (accelMagnitude > 0.01f) {
            glm::vec3 accelDir = velDiff / accelMagnitude;
            float accelAmount = std::min(accelMagnitude, acceleration * dt);
            predictedVel += accelDir * accelAmount;
        }
        
        // Integrate position
        predictedPos += predictedVel * dt;
        
        // Store trajectory point
        trajectory.positions[i] = predictedPos;
        trajectory.velocities[i] = predictedVel;
        trajectory.directions[i] = atan2(predictedVel.x, predictedVel.z);
    }
    
    return trajectory;
}

Trajectory TrajectoryPredictor::PredictCurve(
    const glm::vec3& startPosition,
    const glm::vec3& endPosition,
    float curveHeight,
    float duration,
    const MotionMatchingConfig& config) const {
    
    Trajectory trajectory;
    trajectory.numPoints = config.trajectoryPoints;
    
    glm::vec3 horizontalDir = endPosition - startPosition;
    horizontalDir.y = 0.0f;
    float horizontalDist = glm::length(horizontalDir);
    // Guard against the same NaN normalize-on-zero as Predict(): a zero-length
    // arc would otherwise poison every trajectory feature.
    if (horizontalDist > 1e-6f) horizontalDir /= horizontalDist;
    else horizontalDir = glm::vec3(0.0f);
    
    for (int i = 0; i < config.trajectoryPoints; i++) {
        float t = static_cast<float>(i) / (config.trajectoryPoints - 1);
        
        // Horizontal movement (linear)
        glm::vec3 pos = startPosition + horizontalDir * horizontalDist * t;
        
        // Vertical movement (parabolic arc)
        float parabola = 4.0f * t * (1.0f - t);  // 0 at start/end, 1 at middle
        pos.y += curveHeight * parabola;
        
        // Calculate velocity (derivative of position)
        glm::vec3 vel = horizontalDir * (horizontalDist / duration);
        vel.y = curveHeight * 4.0f * (1.0f - 2.0f * t) / duration;
        
        // Store trajectory point
        trajectory.positions[i] = pos;
        trajectory.velocities[i] = vel;
        trajectory.directions[i] = atan2(vel.x, vel.z);
    }
    
    return trajectory;
}

float TrajectoryPredictor::CalculateTrajectoryDifference(
    const Trajectory& a, const Trajectory& b) const {
    return a.getDifference(b);
}

float TrajectoryPredictor::CalculateCurvature(const Trajectory& trajectory) const {
    if (trajectory.numPoints < 3) return 0.0f;
    
    // Calculate angle between first and last velocity vectors
    glm::vec3 firstVel = glm::normalize(trajectory.velocities[0]);
    glm::vec3 lastVel = glm::normalize(trajectory.velocities[trajectory.numPoints - 1]);
    
    float dot = glm::clamp(glm::dot(firstVel, lastVel), -1.0f, 1.0f);
    float angle = std::acos(dot);
    
    return angle;
}

TrajectoryPredictor::SpeedProfile TrajectoryPredictor::GetSpeedProfile(
    const Trajectory& trajectory) const {
    if (trajectory.numPoints < 2) return SpeedProfile::STOPPED;
    
    float firstSpeed = glm::length(trajectory.velocities[0]);
    float lastSpeed = glm::length(trajectory.velocities[trajectory.numPoints - 1]);
    
    if (firstSpeed < 0.1f && lastSpeed < 0.1f) {
        return SpeedProfile::STOPPED;
    }
    
    float speedChange = lastSpeed - firstSpeed;
    
    if (speedChange > 0.5f) {
        return SpeedProfile::ACCELERATING;
    } else if (speedChange < -0.5f) {
        return SpeedProfile::DECELERATING;
    } else {
        return SpeedProfile::CONSTANT;
    }
}

void TrajectoryPredictor::GetDebugPoints(const Trajectory& trajectory,
                                          std::vector<glm::vec3>& outPoints) const {
    outPoints.clear();
    for (int i = 0; i < trajectory.numPoints; i++) {
        outPoints.push_back(trajectory.positions[i]);
    }
}

void TrajectoryPredictor::DrawTrajectory(const Trajectory& trajectory,
                                          const glm::vec3& color,
                                          float duration) const {
    // Would integrate with debug renderer here
    // For now, just a placeholder
    (void)trajectory;
    (void)color;
    (void)duration;
}

void TrajectoryPredictor::PredictInternal(
    const glm::vec3& startPos,
    const glm::vec3& startVel,
    const glm::vec3& accel,
    int numPoints,
    float pointSpacing,
    Trajectory& outTrajectory) const {
    
    outTrajectory.numPoints = numPoints;
    
    glm::vec3 vel = startVel;
    glm::vec3 pos = startPos;
    
    for (int i = 0; i < numPoints; i++) {
        vel += accel * pointSpacing;
        pos += vel * pointSpacing;
        
        outTrajectory.positions[i] = pos;
        outTrajectory.velocities[i] = vel;
        outTrajectory.directions[i] = atan2(vel.x, vel.z);
    }
}
