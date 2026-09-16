#pragma once

/**
 * GeoPredictionSystem - Handles trajectory prediction (Phase 3)
 *
 * Dedicated system for predictive modeling with multithreading:
 * - Kalman Filter (2D position/velocity)
 * - TensorFlow Lite ML model inference
 * - Monte Carlo simulation for uncertainty analysis
 * - Runs predictions on separate thread to avoid blocking
 */

#include "../ECS.h"
#include "../components/PredictionComponent.h"
#include "../../geospatial/PredictiveModel.h"
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>

namespace ecs {

struct PredictionRequest {
    double latitude;
    double longitude;
    double timestamp;
    double speed;
    double heading;
    std::string entityId;
};

struct PredictionResult {
    std::vector<PredictedState> predictions;
    std::vector<std::vector<PredictedState>> monteCarloPaths;
    double confidence;
    double uncertaintySemiMajor;
    double uncertaintySemiMinor;
    double uncertaintyOrientation;
    std::string entityId;
    bool isValid;
};

class GeoPredictionSystem : public System {
public:
    GeoPredictionSystem() : running(false), predictionInterval(2.0f), lastPredictionTime(0.0f) {}

    ~GeoPredictionSystem() {
        stopPredictionThread();
    }

    /**
     * Initialize predictive model
     */
    void initialize(double originLat, double originLon, const std::string& modelPath = "") {
        predictiveModel.initialize(originLat, originLon);
        if (!modelPath.empty()) {
            predictiveModel.loadMLModel(modelPath);
        }
        startPredictionThread();
        std::cout << "[GeoPredictionSystem] Initialized\n";
    }

    /**
     * Add observation for prediction model
     */
    void addObservation(double lat, double lon, double timestamp,
                        double speed = 0.0, double heading = 0.0) {
        predictiveModel.addObservation(lat, lon, timestamp, speed, heading);
    }

    /**
     * Request prediction (async - result delivered via callback)
     */
    void requestPrediction(const std::string& entityId, double timestamp,
                           double horizon = 60.0, int points = 50,
                           std::function<void(const PredictionResult&)> callback = nullptr) {
        PredictionRequest req;
        req.latitude = 0.0; // Will use current model state
        req.longitude = 0.0;
        req.timestamp = timestamp;
        req.speed = 0.0;
        req.heading = 0.0;
        req.entityId = entityId;

        std::lock_guard<std::mutex> lock(requestMutex);
        predictionRequests.push({req, callback, horizon, points});
        cv.notify_one();
    }

    /**
     * Generate predictions synchronously
     */
    PredictionResult generatePredictions(double timestamp, double horizon = 60.0, int points = 50) {
        PredictionResult result;
        result.predictions = predictiveModel.predictTrajectory(timestamp, horizon, points);
        result.monteCarloPaths = predictiveModel.monteCarloSimulation(timestamp, 30.0, 20, 20);
        result.isValid = !result.predictions.empty();

        if (!result.predictions.empty()) {
            result.confidence = result.predictions[result.predictions.size() / 2].confidence;
        }

        double semiMajor, semiMinor, orientation;
        predictiveModel.getUncertaintyEllipse(semiMajor, semiMinor, orientation);
        result.uncertaintySemiMajor = semiMajor;
        result.uncertaintySemiMinor = semiMinor;
        result.uncertaintyOrientation = orientation;

        return result;
    }

    /**
     * Update prediction component from result
     */
    void updatePredictionComponent(PredictionComponent& pred, const PredictionResult& result) {
        pred.predictions = result.predictions;
        pred.monteCarloPaths = result.monteCarloPaths;
        pred.lastPredictionTime = result.predictions.empty() ? 0.0 : result.predictions[0].timestamp;
        pred.isValid = result.isValid;
        pred.confidence = result.confidence;
        pred.uncertaintyEllipseSemiAxes = glm::vec3(
            static_cast<float>(result.uncertaintySemiMajor),
            static_cast<float>(result.uncertaintySemiMinor),
            0.0f);
        pred.uncertaintyOrientation = static_cast<float>(result.uncertaintyOrientation);
        pred.numSimulations = static_cast<int>(result.monteCarloPaths.size());
    }

    /**
     * Main update - generates predictions at interval
     */
    void update(float dt) override {
        lastPredictionTime += dt;
        if (lastPredictionTime >= predictionInterval) {
            lastPredictionTime = 0.0f;
            // Trigger async prediction for default entity
            requestPrediction("gps_tracker", 0.0);
        }
    }

    PredictiveModel& getPredictiveModel() { return predictiveModel; }
    const PredictiveModel& getPredictiveModel() const { return predictiveModel; }

    void setPythonEnvPath(const std::string& path) {
        predictiveModel.setPythonEnvPath(path);
    }

    void setPredictionInterval(float interval) { predictionInterval = interval; }
    float getPredictionInterval() const { return predictionInterval; }

    const char* getName() const override { return "GeoPredictionSystem"; }

private:
    struct PredictionRequestCallback {
        PredictionRequest request;
        std::function<void(const PredictionResult&)> callback;
        double horizon;
        int points;
    };

    PredictiveModel predictiveModel;
    std::queue<PredictionRequestCallback> predictionRequests;
    std::mutex requestMutex;
    std::condition_variable cv;
    std::thread predictionThread;
    std::atomic<bool> running;
    float predictionInterval;
    float lastPredictionTime;

    void startPredictionThread() {
        if (running) return;
        running = true;
        predictionThread = std::thread([this]() { predictionLoop(); });
        std::cout << "[GeoPredictionSystem] Started prediction thread\n";
    }

    void stopPredictionThread() {
        running = false;
        cv.notify_all();
        if (predictionThread.joinable()) {
            predictionThread.join();
        }
    }

    void predictionLoop() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            PredictionRequestCallback req;
            {
                std::lock_guard<std::mutex> lock(requestMutex);
                if (predictionRequests.empty()) continue;
                req = predictionRequests.front();
                predictionRequests.pop();
            }

            PredictionResult result;
            result.predictions = predictiveModel.predictTrajectory(
                req.request.timestamp, req.horizon, req.points);
            result.monteCarloPaths = predictiveModel.monteCarloSimulation(
                req.request.timestamp, 30.0, 20, 20);
            result.isValid = !result.predictions.empty();

            if (!result.predictions.empty()) {
                result.confidence = result.predictions[result.predictions.size() / 2].confidence;
            }

            double semiMajor, semiMinor, orientation;
            predictiveModel.getUncertaintyEllipse(semiMajor, semiMinor, orientation);
            result.uncertaintySemiMajor = semiMajor;
            result.uncertaintySemiMinor = semiMinor;
            result.uncertaintyOrientation = orientation;
            result.entityId = req.request.entityId;

            if (req.callback) {
                req.callback(result);
            }
        }
    }
};

} // namespace ecs
