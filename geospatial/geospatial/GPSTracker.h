#pragma once

/**
 * GPS Tracker - Simulated Real-Time Geospatial Feed
 *
 * Provides a simulated GPS data source for testing geospatial systems.
 * Can be extended later to read from real GPS devices (NMEA serial, etc.)
 * or network APIs.
 *
 * Features:
 * - Simulated movement along configurable paths
 * - Realistic GPS noise (position jitter, accuracy variation)
 * - Timestamp tracking
 * - Multiple "satellite" tracking modes
 *
 * Usage:
 *   GPSTracker tracker;
 *   tracker.setMode(GPSTracker::Mode::SIMULATED_WALK);
 *   tracker.setOrigin(-33.8688, 151.2093);  // Sydney
 *   tracker.update(dt);
 *   auto fix = tracker.getCurrentFix();
 *   // fix.latitude, fix.longitude, fix.altitude, fix.accuracy, ...
 */

#include <glm/glm.hpp>
#include <cmath>
#include <string>
#include <vector>
#include <chrono>

struct GPSFix {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double speed = 0.0;           // m/s
    double heading = 0.0;         // Degrees clockwise from North
    double horizontalAccuracy = 0.0;  // Meters
    double verticalAccuracy = 0.0;    // Meters
    double timestamp = 0.0;       // Unix epoch seconds
    int satelliteCount = 0;
    bool isValid = false;
};

class GPSTracker {
public:
    enum class Mode {
        DISABLED,           // No GPS data
        SIMULATED_STATIC,   // Fixed position with noise
        SIMULATED_WALK,     // Walking path simulation
        SIMULATED_VEHICLE,  // Vehicle movement
        SIMULATED_AIRCRAFT, // Aircraft movement
        // Future: REAL_DEVICE, REAL_NETWORK
    };

    GPSTracker() = default;

    /**
     * Set the simulation origin (center point for paths).
     */
    void setOrigin(double lat, double lon, double alt = 0.0) {
        simOriginLat = lat;
        simOriginLon = lon;
        simOriginAlt = alt;
    }

    /**
     * Set the simulation mode.
     */
    void setMode(Mode mode) {
        currentMode = mode;
        simTime = 0.0;
    }

    /**
     * Set a custom path for SIMULATED_WALK mode.
     * Path is a series of (lat, lon, alt) waypoints.
     */
    void setPath(const std::vector<glm::dvec3>& waypoints) {
        pathWaypoints = waypoints;
        currentWaypoint = 0;
    }

    /**
     * Set speed for simulated movement (meters/second).
     */
    void setSpeed(double speed) {
        simSpeed = speed;
    }

    /**
     * Set GPS noise level (meters of position jitter).
     */
    void setNoiseLevel(double noiseMeters) {
        noiseLevel = noiseMeters;
    }

    /**
     * Update the GPS simulation.
     * Call this every frame with delta time.
     */
    void update(float dt) {
        simTime += dt;

        switch (currentMode) {
            case Mode::DISABLED:
                currentFix.isValid = false;
                break;

            case Mode::SIMULATED_STATIC:
                updateStatic(dt);
                break;

            case Mode::SIMULATED_WALK:
                updateWalk(dt);
                break;

            case Mode::SIMULATED_VEHICLE:
                updateVehicle(dt);
                break;

            case Mode::SIMULATED_AIRCRAFT:
                updateAircraft(dt);
                break;
        }

        // Update timestamp
        currentFix.timestamp = getCurrentTimestamp();
    }

    /**
     * Get the current GPS fix.
     */
    const GPSFix& getCurrentFix() const {
        return currentFix;
    }

    /**
     * Get simulation time (seconds).
     */
    double getSimTime() const { return simTime; }

    /**
     * Get current mode.
     */
    Mode getMode() const { return currentMode; }

    /**
     * Get simulated speed (m/s).
     */
    double getSpeed() const { return simSpeed; }

    /**
     * Get GPS noise level (meters).
     */
    double getNoiseLevel() const { return noiseLevel; }

    /**
     * Get mode name for display.
     */
    static const char* getModeName(Mode mode) {
        switch (mode) {
            case Mode::DISABLED: return "Disabled";
            case Mode::SIMULATED_STATIC: return "Static (Sim)";
            case Mode::SIMULATED_WALK: return "Walk (Sim)";
            case Mode::SIMULATED_VEHICLE: return "Vehicle (Sim)";
            case Mode::SIMULATED_AIRCRAFT: return "Aircraft (Sim)";
            default: return "Unknown";
        }
    }

    /**
     * Get current mode name (instance method).
     */
    const char* getCurrentModeName() const {
        return getModeName(currentMode);
    }

    /**
     * Reset the simulation.
     */
    void reset() {
        simTime = 0.0;
        currentWaypoint = 0;
        currentFix.isValid = false;
    }

private:
    // State
    Mode currentMode = Mode::DISABLED;
    double simTime = 0.0;
    double simSpeed = 1.4;  // Walking speed ~1.4 m/s
    double noiseLevel = 2.0; // 2m GPS noise (realistic for consumer GPS)

    // Origin
    double simOriginLat = 0.0;
    double simOriginLon = 0.0;
    double simOriginAlt = 0.0;

    // Path
    std::vector<glm::dvec3> pathWaypoints;
    size_t currentWaypoint = 0;

    // Current fix
    GPSFix currentFix;

    // Pseudo-random state (deterministic for reproducibility)
    unsigned int rngState = 12345;

    float pseudoRandom() {
        // Simple LCG
        rngState = rngState * 1103515245 + 12345;
        return ((rngState >> 16) & 0x7FFF) / 32767.0f;
    }

    float pseudoRandomRange(float min, float max) {
        return min + pseudoRandom() * (max - min);
    }

    /**
     * Get current timestamp (Unix epoch).
     * Uses real time for realism.
     */
    double getCurrentTimestamp() const {
        auto now = std::chrono::system_clock::now();
        auto epoch = now.time_since_epoch();
        return std::chrono::duration<double>(epoch).count();
    }

    /**
     * Add realistic GPS noise to a position.
     */
    void addNoise(double& lat, double& lon, double& alt) {
        // ~1 degree latitude ≈ 111km
        // ~1 degree longitude ≈ 111km * cos(lat)
        double latNoise = (pseudoRandomRange(-1.0f, 1.0f) * noiseLevel) / 111000.0;
        double lonNoise = (pseudoRandomRange(-1.0f, 1.0f) * noiseLevel) /
                          (111000.0 * std::cos(lat * M_PI / 180.0));
        double altNoise = pseudoRandomRange(-1.0f, 1.0f) * noiseLevel * 1.5f; // Vertical is worse

        lat += latNoise;
        lon += lonNoise;
        alt += altNoise;
    }

    /**
     * SIMULATED_STATIC: Fixed position with GPS drift.
     */
    void updateStatic(float dt) {
        (void)dt;
        double lat = simOriginLat;
        double lon = simOriginLon;
        double alt = simOriginAlt;

        addNoise(lat, lon, alt);

        currentFix.latitude = lat;
        currentFix.longitude = lon;
        currentFix.altitude = alt;
        currentFix.speed = 0.0;
        currentFix.heading = 0.0;
        currentFix.horizontalAccuracy = noiseLevel;
        currentFix.verticalAccuracy = noiseLevel * 1.5f;
        currentFix.satelliteCount = 8 + (int)(pseudoRandom() * 4);
        currentFix.isValid = true;
    }

    /**
     * SIMULATED_WALK: Walking path simulation.
     * Moves in a figure-8 pattern around the origin.
     */
    void updateWalk(float dt) {
        // Figure-8 pattern: parametric equations
        double t = simTime * simSpeed * 0.01;  // Scale for reasonable movement
        double scale = 0.001;  // Degrees scale (~111m per 0.001 deg)

        double baseLat = simOriginLat + std::sin(t) * scale;
        double baseLon = simOriginLon + std::sin(2.0 * t) * scale * 0.5;
        double baseAlt = simOriginAlt + std::sin(t * 0.5) * 2.0;  // Gentle altitude variation

        addNoise(baseLat, baseLon, baseAlt);

        // Calculate heading from movement direction
        double nextT = (simTime + 0.1f) * simSpeed * 0.01;
        double nextLat = simOriginLat + std::sin(nextT) * scale;
        double nextLon = simOriginLon + std::sin(2.0 * nextT) * scale * 0.5;

        double dLat = nextLat - baseLat;
        double dLon = nextLon - baseLon;
        double heading = std::atan2(dLon, dLat) * 180.0 / M_PI;
        if (heading < 0) heading += 360.0;

        currentFix.latitude = baseLat;
        currentFix.longitude = baseLon;
        currentFix.altitude = baseAlt;
        currentFix.speed = simSpeed;
        currentFix.heading = heading;
        currentFix.horizontalAccuracy = noiseLevel;
        currentFix.verticalAccuracy = noiseLevel * 1.5f;
        currentFix.satelliteCount = 6 + (int)(pseudoRandom() * 6);
        currentFix.isValid = true;
    }

    /**
     * SIMULATED_VEHICLE: Vehicle movement simulation.
     * Faster, smoother movement along a larger path.
     */
    void updateVehicle(float dt) {
        (void)dt;
        double t = simTime * 0.005;  // Slower angular rate for larger path
        double scale = 0.005;  // ~550m radius

        // Circular path with slight altitude variation
        double baseLat = simOriginLat + std::cos(t) * scale;
        double baseLon = simOriginLon + std::sin(t) * scale;
        double baseAlt = simOriginAlt + std::sin(t * 2.0) * 5.0;

        addNoise(baseLat, baseLon, baseAlt);

        // Heading is tangent to circle
        double heading = std::atan2(std::cos(t), -std::sin(t)) * 180.0 / M_PI;
        if (heading < 0) heading += 360.0;

        currentFix.latitude = baseLat;
        currentFix.longitude = baseLon;
        currentFix.altitude = baseAlt;
        currentFix.speed = simSpeed * 5.0;  // ~7 m/s (25 km/h)
        currentFix.heading = heading;
        currentFix.horizontalAccuracy = noiseLevel * 0.8f;  // Better accuracy at speed
        currentFix.verticalAccuracy = noiseLevel * 1.2f;
        currentFix.satelliteCount = 8 + (int)(pseudoRandom() * 5);
        currentFix.isValid = true;
    }

    /**
     * SIMULATED_AIRCRAFT: Aircraft movement simulation.
     * High speed, high altitude, large coverage area.
     */
    void updateAircraft(float dt) {
        (void)dt;
        double t = simTime * 0.001;  // Very slow angular rate for large path
        double scale = 0.05;  // ~5.5km radius

        // Large circular path
        double baseLat = simOriginLat + std::cos(t) * scale;
        double baseLon = simOriginLon + std::sin(t) * scale;
        double baseAlt = simOriginAlt + 1000.0 + std::sin(t * 0.5) * 200.0;  // 1000m altitude

        addNoise(baseLat, baseLon, baseAlt);

        double heading = std::atan2(std::cos(t), -std::sin(t)) * 180.0 / M_PI;
        if (heading < 0) heading += 360.0;

        currentFix.latitude = baseLat;
        currentFix.longitude = baseLon;
        currentFix.altitude = baseAlt;
        currentFix.speed = simSpeed * 50.0;  // ~70 m/s (250 km/h)
        currentFix.heading = heading;
        currentFix.horizontalAccuracy = noiseLevel * 0.5f;
        currentFix.verticalAccuracy = noiseLevel * 0.8f;
        currentFix.satelliteCount = 10 + (int)(pseudoRandom() * 4);
        currentFix.isValid = true;
    }
};
