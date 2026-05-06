#pragma once

/**
 * Predictive Model - ML/Prediction Engine (Phase 3)
 *
 * Provides trajectory prediction and simulation capabilities:
 * - Kalman Filter for sensor fusion
 * - Linear/non-linear trajectory prediction
 * - Monte Carlo simulation for uncertainty
 * - TensorFlow Lite integration for ML-based prediction
 */

#include <vector>
#include <deque>
#include <cmath>
#include <random>
#include <iostream>
#include <string>
#include "../ecs/components/GeospatialComponent.h"
#include "TFLitePredictor.h"
#include "TimeSeriesDB.h"

// Predicted state
struct PredictedState {
    double timestamp = 0.0;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double speed = 0.0;
    double heading = 0.0;
    double confidence = 1.0;  // 0-1
    double uncertaintyRadius = 0.0; // meters
};

// Kalman Filter for 2D position/speed tracking
class KalmanFilter2D {
public:
    KalmanFilter2D() {
        state.resize(4, 0.0);
        covariance.resize(4, std::vector<double>(4, 0.0));

        for (int i = 0; i < 4; i++) {
            covariance[i][i] = 100.0;
        }

        processNoise = 0.1;
        measurementNoise = 5.0;
    }

    void init(double x, double y, double vx = 0, double vy = 0) {
        state[0] = x;
        state[1] = y;
        state[2] = vx;
        state[3] = vy;
    }

    void predict(double dt) {
        double x = state[0] + state[2] * dt;
        double y = state[1] + state[3] * dt;

        state[0] = x;
        state[1] = y;

        double dt2 = dt * dt;
        covariance[0][0] += 2 * dt * covariance[0][2] + dt2 * covariance[2][2] + processNoise;
        covariance[0][2] += dt * covariance[2][2];
        covariance[1][1] += 2 * dt * covariance[1][3] + dt2 * covariance[3][3] + processNoise;
        covariance[1][3] += dt * covariance[3][3];
    }

    void update(double measX, double measY) {
        double innovX = measX - state[0];
        double innovY = measY - state[1];

        double sX = covariance[0][0] + measurementNoise;
        double sY = covariance[1][1] + measurementNoise;

        double kX = covariance[0][0] / sX;
        double kY = covariance[1][1] / sY;

        state[0] += kX * innovX;
        state[1] += kY * innovY;

        covariance[0][0] *= (1 - kX);
        covariance[1][1] *= (1 - kY);
    }

    void getState(double& x, double& y, double& vx, double& vy) const {
        x = state[0];
        y = state[1];
        vx = state[2];
        vy = state[3];
    }

    double getPositionUncertainty() const {
        return std::sqrt(covariance[0][0] + covariance[1][1]);
    }

private:
    std::vector<double> state;
    std::vector<std::vector<double>> covariance;
    double processNoise;
    double measurementNoise;
};

class PredictiveModel {
public:
    PredictiveModel() : originLat(0.0), originLon(0.0),
                        lastPredictionTime(0.0), lastVx(0.0), lastVy(0.0),
                        initialized(false), useML(false) {}

    /**
     * Initialize with parameters
     */
    void initialize(double originLat, double originLon) {
        this->originLat = originLat;
        this->originLon = originLon;
        kalman.init(0, 0, 0, 0);
        initialized = true;
    }

    /**
     * Load TensorFlow Lite model for ML-based prediction
     */
    bool loadMLModel(const std::string& modelPath) {
        TFLiteStatus status = tflitePredictor.loadModel(modelPath);
        if (status == TFLiteStatus::Ok) {
            useML = true;
            mlModelLoaded = true;
            mlFrameCounter = 0;
            lastMlPredictions.clear();
            std::cout << "[PredictiveModel] ML model loaded, will use TFLite for prediction\n";
            return true;
        }
        std::cout << "[PredictiveModel] ML model load failed, using Kalman Filter\n";
        return false;
    }

    /**
     * Add a new observation
     */
    void addObservation(double lat, double lon, double timestamp,
                        double speed = 0, double heading = 0) {
        if (!initialized) {
            lastPredictionTime = timestamp;
            initialized = true;
        }

        double x, y;
        geoToLocal(lat, lon, x, y);

        if (lastPredictionTime > 0) {
            double dt = timestamp - lastPredictionTime;
            if (dt > 0) {
                kalman.predict(dt);
            }
        }

        kalman.update(x, y);
        kalman.getState(x, y, lastVx, lastVy);

        history.push_back({timestamp, lat, lon, 0.0, speed, heading, 0.0, ""});
        if (history.size() > 1000) {
            history.pop_front();
        }

        lastPredictionTime = timestamp;
    }

    /**
     * Predict future positions (uses Kalman filter - fast, no process spawning)
     */
    std::vector<PredictedState> predictTrajectory(double currentTime,
                                                     double horizonSeconds,
                                                     int numPoints = 50) {
        if (!initialized || history.empty()) {
            return {};
        }

        // Use fast Kalman filter (ML disabled for performance)
        return predictWithKalman(currentTime, horizonSeconds, numPoints);
    }

    /**
     * Monte Carlo simulation for uncertainty analysis
     */
    std::vector<std::vector<PredictedState>> monteCarloSimulation(
        double currentTime, double horizonSeconds,
        int numSimulations = 100, int pointsPerSim = 50) {

        std::vector<std::vector<PredictedState>> results;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::normal_distribution<double> posNoise(0, kalman.getPositionUncertainty());
        std::normal_distribution<double> velNoise(0, 1.0);

        for (int sim = 0; sim < numSimulations; sim++) {
            double x, y, vx, vy;
            kalman.getState(x, y, vx, vy);

            double simX = x + posNoise(gen);
            double simY = y + posNoise(gen);
            double simVx = vx + velNoise(gen);
            double simVy = vy + velNoise(gen);

            std::vector<PredictedState> trajectory;
            double dt = horizonSeconds / pointsPerSim;

            for (int i = 0; i < pointsPerSim; i++) {
                simX += simVx * dt;
                simY += simVy * dt;

                PredictedState pred;
                pred.timestamp = currentTime + (i + 1) * dt;
                localToGeo(simX, simY, pred.latitude, pred.longitude);
                pred.confidence = 1.0 / (sim + 1);
                trajectory.push_back(pred);
            }
            results.push_back(trajectory);
        }

        return results;
    }

    /**
     * Get uncertainty ellipse at current position
     */
    void getUncertaintyEllipse(double& semiMajor, double& semiMinor, double& orientation) {
        double unc = kalman.getPositionUncertainty();
        semiMajor = unc * 1.5;
        semiMinor = unc * 0.8;
        orientation = std::atan2(lastVy, lastVx);
    }

    bool isUsingML() const { return useML; }

    void setPythonEnvPath(const std::string& path) {
        tflitePredictor.setPythonEnvPath(path);
    }

    KalmanFilter2D kalman;
    TFLitePredictor tflitePredictor;
    bool mlModelLoaded;
    std::vector<PredictedState> lastMlPredictions; // Cache for frame skipping
    int mlFrameCounter; // For frame skipping
    bool initialized;
    bool useML;
    double originLat, originLon;
    double lastPredictionTime;
    double lastVx, lastVy;

    std::deque<TimeSeriesPoint> history;

    std::vector<PredictedState> predictWithKalman(double currentTime,
                                                    double horizonSeconds,
                                                    int numPoints) {
        std::vector<PredictedState> predictions;

        double x, y, vx, vy;
        kalman.getState(x, y, vx, vy);

        double uncertainty = kalman.getPositionUncertainty();
        double dt = horizonSeconds / numPoints;

        for (int i = 0; i < numPoints; i++) {
            double t = currentTime + (i + 1) * dt;

            double futureX = x + vx * (i + 1) * dt;
            double futureY = y + vy * (i + 1) * dt;

            PredictedState pred;
            pred.timestamp = t;
            localToGeo(futureX, futureY, pred.latitude, pred.longitude);
            pred.speed = std::sqrt(vx * vx + vy * vy);
            pred.heading = std::atan2(vy, vx) * 180.0 / M_PI;
            if (pred.heading < 0) pred.heading += 360.0;

            pred.uncertaintyRadius = uncertainty * (1.0 + (i + 1) * 0.1);
            pred.confidence = 1.0 / (1.0 + pred.uncertaintyRadius / 100.0);

            predictions.push_back(pred);
        }

        return predictions;
    }

    std::vector<PredictedState> predictWithML(double currentTime,
                                                 double horizonSeconds,
                                                 int numPoints) {
        std::vector<PredictedState> predictions;

        // Frame skipping: only run ML inference every 10 frames
        if (++mlFrameCounter < 10 && !lastMlPredictions.empty()) {
            return lastMlPredictions; // Return cached predictions
        }
        mlFrameCounter = 0;

        // Build input from recent history
        // Format: [lat0, lon0, speed0, heading0, lat1, lon1, speed1, heading1, ...]
        std::vector<double> mlInput;
        int historyWindow = std::min((int)history.size(), 20);
        auto it = history.end();
        for (int i = 0; i < historyWindow; i++) {
            --it;
            mlInput.push_back(it->latitude);
            mlInput.push_back(it->longitude);
            mlInput.push_back(it->speed);
            mlInput.push_back(it->heading);
        }

        // Run ML prediction
        std::cerr << "[PredictiveModel] Calling runInference()\n" << std::flush;
        std::vector<float> mlInputFloat(mlInput.begin(), mlInput.end());
        std::vector<float> mlOutput;
        auto status = tflitePredictor.runInference(mlInputFloat, mlOutput);

        if (status == TFLiteStatus::Ok && !mlOutput.empty()) {
            double dt = horizonSeconds / numPoints;
            int outputPoints = mlOutput.size() / 2; // Expecting lat/lon pairs
            for (int i = 0; i < outputPoints && i < numPoints; i++) {
                PredictedState pred;
                pred.timestamp = currentTime + (i + 1) * dt;
                pred.latitude = mlOutput[i * 2];
                pred.longitude = mlOutput[i * 2 + 1];
                pred.altitude = 0.0;
                pred.confidence = 0.85; // ML predictions have moderate confidence
                pred.uncertaintyRadius = 10.0 * (1.0 + (i + 1) * 0.05);
                predictions.push_back(pred);
            }
        } else {
            // Fallback to Kalman
            return predictWithKalman(currentTime, horizonSeconds, numPoints);
        }

        lastMlPredictions = predictions;
        return predictions;
    }

    void geoToLocal(double lat, double lon, double& x, double& y) {
        x = (lon - originLon) * 111000.0 * std::cos(lat * M_PI / 180.0);
        y = (lat - originLat) * 111000.0;
    }

    void localToGeo(double x, double y, double& lat, double& lon) {
        lat = originLat + y / 111000.0;
        lon = originLon + x / (111000.0 * std::cos(lat * M_PI / 180.0));
    }
};
