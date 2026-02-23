#pragma once
#include "MotionMatchingTypes.h"
#include <glm/glm.hpp>

// ============================================================================
// TRAJECTORY PREDICTOR
// ============================================================================
// 
// Predicts where the character WILL BE in the future.
// This is CRITICAL for motion matching - we don't just match
// current state, we match where we're GOING.
// 
// Example:
// - Character running forward at 5 m/s
// - In 0.3s, they'll be 1.5m ahead
// - We need a pose that moves them 1.5m in 0.3s
// ============================================================================

class TrajectoryPredictor {
public:
    TrajectoryPredictor();
    ~TrajectoryPredictor() = default;
    
    // =========================================================================
    // TRAJECTORY PREDICTION
    // =========================================================================
    
    /**
     * Predict future trajectory from current state
     * 
     * @param currentPosition Current character position
     * @param currentVelocity Current character velocity
     * @param currentRotation Current character rotation (yaw in radians)
     * @param moveDirection Input movement direction (-1 to 1)
     * @param config Motion matching configuration
     * @return Predicted trajectory for next N frames
     */
    Trajectory Predict(const glm::vec3& currentPosition,
                      const glm::vec3& currentVelocity,
                      float currentRotation,
                      const glm::vec2& moveDirection,
                      const MotionMatchingConfig& config) const;
    
    /**
     * Predict trajectory with acceleration
     * 
     * Accounts for character speeding up or slowing down.
     * 
     * @param currentPosition Current position
     * @param currentVelocity Current velocity
     * @param targetVelocity Target velocity (where input wants to go)
     * @param acceleration How fast character can accelerate
     * @param config Configuration
     * @return Predicted trajectory with acceleration
     */
    Trajectory PredictWithAcceleration(
        const glm::vec3& currentPosition,
        const glm::vec3& currentVelocity,
        const glm::vec3& targetVelocity,
        float acceleration,
        const MotionMatchingConfig& config) const;
    
    /**
     * Predict trajectory along a curve (for parkour, etc.)
     * 
     * @param startPosition Start of curve
     * @param endPosition End of curve
     * @param curveHeight How high the curve arcs (for jumps)
     * @param duration How long to traverse curve
     * @return Curved trajectory
     */
    Trajectory PredictCurve(const glm::vec3& startPosition,
                           const glm::vec3& endPosition,
                           float curveHeight,
                           float duration,
                           const MotionMatchingConfig& config) const;
    
    // =========================================================================
    // TRAJECTORY ANALYSIS
    // =========================================================================
    
    /**
     * Calculate trajectory difference
     * 
     * How different are two trajectories?
     * Used for scoring pose matches.
     */
    float CalculateTrajectoryDifference(const Trajectory& a,
                                        const Trajectory& b) const;
    
    /**
     * Get trajectory curvature
     * 
     * How much is the character turning?
     * 0 = straight line, >0 = turning
     */
    float CalculateCurvature(const Trajectory& trajectory) const;
    
    /**
     * Get trajectory speed profile
     * 
     * Is character accelerating, decelerating, or constant?
     */
    enum class SpeedProfile {
        ACCELERATING,     // Speeding up
        DECELERATING,     // Slowing down
        CONSTANT,         // Same speed
        STOPPED           // Not moving
    };
    
    SpeedProfile GetSpeedProfile(const Trajectory& trajectory) const;
    
    // =========================================================================
    // DEBUG VISUALIZATION
    // =========================================================================
    
    /**
     * Get trajectory as debug line points
     */
    void GetDebugPoints(const Trajectory& trajectory,
                       std::vector<glm::vec3>& outPoints) const;
    
    /**
     * Draw trajectory (requires debug renderer)
     */
    void DrawTrajectory(const Trajectory& trajectory,
                       const glm::vec3& color,
                       float duration = 2.0f) const;
    
private:
    /**
     * Internal prediction helper
     */
    void PredictInternal(
        const glm::vec3& startPos,
        const glm::vec3& startVel,
        const glm::vec3& accel,
        int numPoints,
        float pointSpacing,
        Trajectory& outTrajectory) const;
};
