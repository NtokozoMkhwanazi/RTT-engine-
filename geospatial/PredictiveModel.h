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

// Kalman Filter for 3D position/velocity tracking (X/Y/Z + Vx/Vy/Vz).
// Tracks vertical climb (Z) so the predictor can follow altitude changes
// (elevators, ramps, multi-level dig sites / terraced benches), not just
// horizontal motion on the 2D plane.
class KalmanFilter3D {
public:
    enum : int { X = 0, Y = 1, Z = 2, VX = 3, VY = 4, VZ = 5 };
    static constexpr int N = 6;

    KalmanFilter3D() {
        state.assign(N, 0.0);
        covariance.assign(N, std::vector<double>(N, 0.0));

        for (int i = 0; i < N; i++) {
            covariance[i][i] = 100.0;
        }

        processNoise = 0.1;       // horizontal (m^2/s)
        processNoiseZ = 0.2;      // vertical climb is noisier than planar motion
        measurementNoise = 5.0;
    }

    void init(double x, double y, double z, double vx = 0, double vy = 0, double vz = 0) {
        state[X] = x;  state[Y] = y;  state[Z] = z;
        state[VX] = vx; state[VY] = vy; state[VZ] = vz;
    }

    // Constant-velocity propagate, including vertical climb (Z += Vz*dt).
    void predict(double dt) {
        state[X]  += state[VX] * dt;
        state[Y]  += state[VY] * dt;
        state[Z]  += state[VZ] * dt;   // <- vertical climb

        double dt2 = dt * dt;
        // Horizontal covariance propagation (original 2D filter, reindexed to 3D).
        covariance[0][0] += 2 * dt * covariance[0][VX] + dt2 * covariance[VX][VX] + processNoise;
        covariance[0][VX] += dt * covariance[VX][VX];
        covariance[1][1] += 2 * dt * covariance[1][VY] + dt2 * covariance[VY][VY] + processNoise;
        covariance[1][VY] += dt * covariance[VY][VY];
        // Vertical (Z) covariance propagation - tracks altitude uncertainty.
        covariance[2][2] += 2 * dt * covariance[2][VZ] + dt2 * covariance[VZ][VZ] + processNoiseZ;
        covariance[2][VZ] += dt * covariance[VZ][VZ];
    }

    // 3D position+altitude observation update.
    void update(double measX, double measY, double measZ) {
        double innovX = measX - state[X];
        double innovY = measY - state[Y];
        double innovZ = measZ - state[Z];

        double sX = covariance[0][0] + measurementNoise;
        double sY = covariance[1][1] + measurementNoise;
        double sZ = covariance[2][2] + measurementNoise;

        double kX = covariance[0][0] / sX;
        double kY = covariance[1][1] / sY;
        double kZ = covariance[2][2] / sZ;

        state[X] += kX * innovX;
        state[Y] += kY * innovY;
        state[Z] += kZ * innovZ;

        covariance[0][0] *= (1 - kX);
        covariance[1][1] *= (1 - kY);
        covariance[2][2] *= (1 - kZ);
    }

    void getState(double& x, double& y, double& z,
                  double& vx, double& vy, double& vz) const {
        x = state[X]; y = state[Y]; z = state[Z];
        vx = state[VX]; vy = state[VY]; vz = state[VZ];
    }

    // Overwrite the full 6-D state. Used by PredictiveModel to inject a
    // velocity estimate derived from consecutive position fixes (the
    // correction update above only nudges position, so without this the
    // velocity state never learns the walker's heading/speed and predictions
    // collapse to a stationary point - i.e. "just a dot").
    void setState(double x, double y, double z,
                  double vx, double vy, double vz) {
        state[X] = x; state[Y] = y; state[Z] = z;
        state[VX] = vx; state[VY] = vy; state[VZ] = vz;
    }

    double getPositionUncertainty() const {
        return std::sqrt(covariance[0][0] + covariance[1][1] + covariance[2][2]);
    }

private:
    std::vector<double> state;
    std::vector<std::vector<double>> covariance;
    double processNoise;
    double processNoiseZ;   // vertical (Z) climb noise
    double measurementNoise;
};

class PredictiveModel {
public:
    // Member-init order MUST match declaration order (gcc/clang -Wreorder).
    // Declaration order of the fields below: initialized, useML, originLat,
    // originLon, lastPredictionTime, lastVx, lastVy, ...
    PredictiveModel() : initialized(false), useML(false),
                        originLat(0.0), originLon(0.0), originAlt(0.0),
                        lastPredictionTime(0.0), lastVx(0.0), lastVy(0.0),
                        lastVz(0.0) {}

    /**
     * Initialize with parameters
     */
    void initialize(double originLat, double originLon, double originAlt = 0.0) {
        this->originLat = originLat;
        this->originLon = originLon;
        this->originAlt = originAlt;
        kalman.init(0, 0, 0, 0, 0, 0);
        lastGeoX = lastGeoY = lastGeoZ = 0.0;
        lastVx = lastVy = lastVz = 0.0;
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
     *
     * The 3D Kalman filter is fed Z (altitude) too, so the predictor can follow
     * vertical climb / descent (Todo 9+10): lat/lon/alt are converted from
     * WGS84 to local metric space (East/North metres + altitude-metres Z) once,
     * then both the position estimate and the trajectory history carry altitude.
     */
    void addObservation(double lat, double lon, double timestamp,
                        double speed = 0, double heading = 0,
                        double altitude = 0.0) {
        if (!initialized) {
            lastPredictionTime = timestamp;
            initialized = true;
        }

        double x, y, z;
        geoToLocal3D(lat, lon, altitude, x, y, z);

        const double dt = timestamp - lastPredictionTime;   // 0 on the 1st obs
        if (dt > 0) {
            kalman.predict(dt);
        }

        kalman.update(x, y, z);   // 3D update: feeds Z/altitude into kalman3D
        kalman.getState(x, y, z, lastVx, lastVy, lastVz);

        // Learn velocity from consecutive smoothed positions. The correction
        // update above only adjusts position, so without this Vx/Vy/Vz stay at
        // 0 and predictWithKalman collapses to a stationary point ("just a dot").
        // Estimating velocity here makes the dashboard prediction cone actually
        // LEAD the walker along its heading/speed (descriptive, not a dot).
        if (dt > 0) {
            const double alpha = 0.5;                       // velocity learn rate
            lastVx = lastVx + alpha * ((x - lastGeoX) / dt - lastVx);
            lastVy = lastVy + alpha * ((y - lastGeoY) / dt - lastVy);
            lastVz = lastVz + alpha * ((z - lastGeoZ) / dt - lastVz);
            kalman.setState(x, y, z, lastVx, lastVy, lastVz);
        }
        lastGeoX = x; lastGeoY = y; lastGeoZ = z;

        history.push_back({timestamp, lat, lon, altitude, speed, heading, 0.0, "", ""});
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
            double x, y, z, vx, vy, vz;
            kalman.getState(x, y, z, vx, vy, vz);

            double simX = x + posNoise(m_rng);
            double simY = y + posNoise(m_rng);
            double simZ = z + posNoise(m_rng);
            double simVx = vx + velNoise(m_rng);
            double simVy = vy + velNoise(m_rng);
            double simVz = vz + velNoise(m_rng);

            std::vector<PredictedState> trajectory;
            double dt = horizonSeconds / pointsPerSim;

            for (int i = 0; i < pointsPerSim; i++) {
                simX += simVx * dt;
                simY += simVy * dt;
                simZ += simVz * dt;   // propagate vertical climb noise

                PredictedState pred;
                pred.timestamp = currentTime + (i + 1) * dt;
                localToGeo3D(simX, simY, simZ, pred.latitude, pred.longitude, pred.altitude);
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

    KalmanFilter3D kalman;
    TFLitePredictor tflitePredictor;
    bool mlModelLoaded;
    std::vector<PredictedState> lastMlPredictions; // Cache for frame skipping
    int mlFrameCounter; // For frame skipping
    bool initialized;
    bool useML;
    std::mt19937 m_rng{12345}; // Persistent RNG for Monte Carlo (deterministic)
    double originLat, originLon, originAlt;
    double lastPredictionTime;
    double lastVx, lastVy, lastVz;
    double lastGeoX = 0.0, lastGeoY = 0.0, lastGeoZ = 0.0;   // smoothed pos (for velocity)

    std::deque<TimeSeriesPoint> history;

    // Reusable ML inference scratch buffers. Sized once after loadMLModel()
    // succeeds (to tflitePredictor.getInputSize() / getOutputSize()) and reused
    // on every predictWithML() call so the allocation-free runInference()
    // forward pass is never shadowed by a caller-side heap allocation on the
    // 10 ms real-time loop (forGeo.txt §1).
    std::vector<float> mlInput_;
    std::vector<float> mlOutput_;

    std::vector<PredictedState> predictWithKalman(double currentTime,
                                                    double horizonSeconds,
                                                    int numPoints) {
        std::vector<PredictedState> predictions;

        double x, y, z, vx, vy, vz;
        kalman.getState(x, y, z, vx, vy, vz);

        double uncertainty = kalman.getPositionUncertainty();
        double dt = horizonSeconds / numPoints;

        for (int i = 0; i < numPoints; i++) {
            double t = currentTime + (i + 1) * dt;

            // Constant-velocity extrapolation including vertical climb (Z).
            double futureX = x + vx * (i + 1) * dt;
            double futureY = y + vy * (i + 1) * dt;
            double futureZ = z + vz * (i + 1) * dt;

            PredictedState pred;
            pred.timestamp = t;
            localToGeo3D(futureX, futureY, futureZ, pred.latitude, pred.longitude, pred.altitude);
            pred.speed = std::sqrt(vx * vx + vy * vy + vz * vz); // 3D speed
            pred.heading = std::atan2(vy, vx) * 180.0 / M_PI;    // horizontal heading
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

        // Reusable input/output buffers (allocation-free across frames once
        // sized by loadMLModel). mlInput_ is zeroed each call to mirror the
        // old `std::vector<float> mlInput(80, 0.0f)` semantics for the
        // under-filled tail when fewer than 20 history samples are queued.
        const size_t inSize = tflitePredictor.getInputSize();
        if (inSize == 0) return {};
        mlInput_.assign(inSize, 0.0f);

        const size_t n = std::min<size_t>(20, history.size());
        auto it = history.end();
        for (size_t i = 0; i < n; ++i) --it;  // walk back to the window start

        size_t s = 0;
        for (auto p = it; p != history.end() && s < 20; ++p, ++s) {
            double x = 0.0, y = 0.0;
            geoToLocal(p->latitude, p->longitude, x, y);
            const double dx = x - curX;
            const double dy = y - curY;
            mlInput_[s * 4 + 0] = static_cast<float>(dx / 100.0);
            mlInput_[s * 4 + 1] = static_cast<float>(dy / 100.0);
            mlInput_[s * 4 + 2] = static_cast<float>(p->speed / 10.0);
            mlInput_[s * 4 + 3] = static_cast<float>((p->heading * M_PI / 180.0) / M_PI);
        }

        // Reuse the output buffer (sized to the network output, e.g. 100 floats)
        // so runInference's final copy never allocates either.
        const size_t outSize = tflitePredictor.getOutputSize();
        if (mlOutput_.size() != outSize) mlOutput_.resize(outSize);
        const TFLiteStatus status = tflitePredictor.runInference(mlInput_, mlOutput_);
        if (status != TFLiteStatus::Ok || mlOutput_.size() < 2) return {};

        // Output: 50 future (dx/100, dy/100) meter offsets at 1 s steps.
        std::vector<PredictedState> predictions;
        const size_t outputPoints = mlOutput_.size() / 2;
        predictions.reserve(outputPoints);
        for (size_t i = 0; i < outputPoints; ++i) {
            const double dx = mlOutput_[i * 2] * 100.0;
            const double dy = mlOutput_[i * 2 + 1] * 100.0;
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

    // 3D WGS84 -> local metric: lat/lon -> East/North metres (same projection as
    // geoToLocal), altitude -> local Z metres (relative to the seeded originAlt).
    void geoToLocal3D(double lat, double lon, double alt,
                      double& x, double& y, double& z) {
        geoToLocal(lat, lon, x, y);
        z = alt - originAlt;
    }

    void localToGeo(double x, double y, double& lat, double& lon) {
        lat = originLat + y / 111000.0;
        lon = originLon + x / (111000.0 * std::cos(lat * M_PI / 180.0));
    }

    // Inverse of geoToLocal3D: local metric -> WGS84 lat/lon/alt.
    void localToGeo3D(double x, double y, double z,
                      double& lat, double& lon, double& alt) {
        localToGeo(x, y, lat, lon);
        alt = z + originAlt;
    }
};
