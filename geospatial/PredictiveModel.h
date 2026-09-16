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
#include <algorithm>
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
     * Predict future positions. Uses the trained ML model when loaded (cached
     * with frame skipping), otherwise the fast Kalman filter.
     */
    std::vector<PredictedState> predictTrajectory(double currentTime,
                                                     double horizonSeconds,
                                                     int numPoints = 50) {
        if (!initialized || history.empty()) {
            return {};
        }

        if (useML) {
            std::vector<PredictedState> ml = predictWithML(currentTime, horizonSeconds, numPoints);
            if (!ml.empty()) return ml;
        }
        return predictWithKalman(currentTime, horizonSeconds, numPoints);
    }

    /**
     * Monte Carlo simulation for uncertainty analysis.
     * Uses a persistent (per-model) RNG - constructing std::random_device per
     * call was a per-call stall on some systems.
     */
    std::vector<std::vector<PredictedState>> monteCarloSimulation(
        double currentTime, double horizonSeconds,
        int numSimulations = 100, int pointsPerSim = 50) {

        std::vector<std::vector<PredictedState>> results;
        std::normal_distribution<double> posNoise(0, kalman.getPositionUncertainty());
        std::normal_distribution<double> velNoise(0, 1.0);

        for (int sim = 0; sim < numSimulations; sim++) {
            double x, y, vx, vy;
            kalman.getState(x, y, vx, vy);

            double simX = x + posNoise(m_rng);
            double simY = y + posNoise(m_rng);
            double simVx = vx + velNoise(m_rng);
            double simVy = vy + velNoise(m_rng);

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
    std::mt19937 m_rng{12345}; // Persistent RNG for Monte Carlo (deterministic)
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
        // Frame skipping: only run ML inference every 10 calls; the result is
        // cached so the (microsecond-scale) forward pass is not a hot path.
        if (++mlFrameCounter < 10 && !lastMlPredictions.empty()) {
            return lastMlPredictions;
        }
        mlFrameCounter = 0;

        if (history.size() < 2) return {};

        // Input: 20 history samples of (dx/100, dy/100, speed/10, heading/pi)
        // where dx/dy are meter offsets (East/North) from the CURRENT position.
        const auto& last = history.back();
        double curX = 0.0, curY = 0.0;
        geoToLocal(last.latitude, last.longitude, curX, curY);

        std::vector<float> mlInput(80, 0.0f);
        const size_t n = std::min<size_t>(20, history.size());
        auto it = history.end();
        for (size_t i = 0; i < n; ++i) --it;  // walk back to the window start

        size_t s = 0;
        for (auto p = it; p != history.end() && s < 20; ++p, ++s) {
            double x = 0.0, y = 0.0;
            geoToLocal(p->latitude, p->longitude, x, y);
            const double dx = x - curX;
            const double dy = y - curY;
            mlInput[s * 4 + 0] = static_cast<float>(dx / 100.0);
            mlInput[s * 4 + 1] = static_cast<float>(dy / 100.0);
            mlInput[s * 4 + 2] = static_cast<float>(p->speed / 10.0);
            mlInput[s * 4 + 3] = static_cast<float>((p->heading * M_PI / 180.0) / M_PI);
        }

        std::vector<float> mlOutput;
        const TFLiteStatus status = tflitePredictor.runInference(mlInput, mlOutput);
        if (status != TFLiteStatus::Ok || mlOutput.size() < 2) return {};

        // Output: 50 future (dx/100, dy/100) meter offsets at 1 s steps.
        std::vector<PredictedState> predictions;
        const size_t outputPoints = mlOutput.size() / 2;
        predictions.reserve(outputPoints);
        for (size_t i = 0; i < outputPoints; ++i) {
            const double dx = mlOutput[i * 2] * 100.0;
            const double dy = mlOutput[i * 2 + 1] * 100.0;
            PredictedState pred;
            pred.timestamp = currentTime + (double)(i + 1) * 1.0;  // 1 s grid
            localToGeo(curX + dx, curY + dy, pred.latitude, pred.longitude);
            pred.altitude = last.altitude;
            pred.confidence = 0.7;
            pred.uncertaintyRadius = 8.0 + (double)i * 1.5;
            predictions.push_back(pred);
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
