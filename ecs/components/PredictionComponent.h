#pragma once

/**
 * Prediction Component - Stores predicted trajectories (Phase 3)
 *
 * Attached to entities that have predicted future states.
 * Used by the visualization system to render predictions.
 */

#include <vector>
#include "../components/GeospatialComponent.h"
#include "glm/glm.hpp"
#include "../../geospatial/PredictiveModel.h"

namespace ecs {

struct PredictionComponent {
    // Predicted trajectory points
    std::vector<PredictedState> predictions;

    // Prediction metadata
    double predictionHorizon = 60.0;  // seconds
    double lastPredictionTime = 0.0;
    bool isValid = false;
    double confidence = 0.0;

    // Monte Carlo simulation results
    std::vector<std::vector<PredictedState>> monteCarloPaths;
    int numSimulations = 0;

    // Uncertainty visualization
    glm::vec3 uncertaintyEllipseSemiAxes{0.0f};
    float uncertaintyOrientation = 0.0f;

    PredictionComponent() = default;

    /**
     * Clear all predictions
     */
    void clear() {
        predictions.clear();
        monteCarloPaths.clear();
        isValid = false;
    }

    /**
     * Check if predictions are stale
     */
    bool isStale(double currentTime, double maxAge = 5.0) const {
        return (currentTime - lastPredictionTime) > maxAge;
    }

    /**
     * Get the furthest predicted point
     */
    const PredictedState* getFinalPrediction() const {
        if (predictions.empty()) return nullptr;
        return &predictions.back();
    }

    /**
     * Get prediction at specific time offset
     */
    const PredictedState* getPredictionAt(double timeOffset) const {
        for (const auto& pred : predictions) {
            if (pred.timestamp >= timeOffset) {
                return &pred;
            }
        }
        return nullptr;
    }
};

} // namespace ecs
