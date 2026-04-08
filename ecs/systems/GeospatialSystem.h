#pragma once

/**
 * Geospatial System - Manages real-time geospatial data for ECS entities
 *
 * Bridges the GPS tracker with ECS entities that have GeospatialComponent.
 * Converts WGS84 coordinates to local engine space and updates TransformComponent.
 *
 * This system:
 * 1. Receives GPS fixes from GPSTracker
 * 2. Converts to local space via GeospatialConverter
 * 3. Updates entity TransformComponent position
 * 4. Provides geospatial queries (distance, bearing, etc.)
 */

#include "../ECS.h"
#include "../components/GeospatialComponent.h"
#include "../components/TransformComponent.h"
#include "../../geospatial/GeospatialConverter.h"
#include "../../geospatial/GPSTracker.h"

#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <iostream>

namespace ecs {

class GeospatialSystem : public System {
public:
    GeospatialSystem() = default;
    ~GeospatialSystem() = default;

    /**
     * Initialize the geospatial system with a reference origin.
     * Call this once when loading a geospatial scene.
     */
    void initialize(double originLat, double originLon, double originAlt = 0.0) {
        converter.setOrigin(originLat, originLon, originAlt);
        gpsTracker.setOrigin(originLat, originLon, originAlt);
        gpsTracker.setMode(GPSTracker::Mode::SIMULATED_WALK);
        gpsTracker.setSpeed(1.4f);  // Walking speed
        gpsTracker.setNoiseLevel(2.0f);  // 2m GPS noise

        std::cout << "[GeospatialSystem] Initialized at origin: "
                  << originLat << "°, " << originLon << "°, " << originAlt << "m\n";
    }

    /**
     * Set GPS tracker mode.
     */
    void setGPSMode(GPSTracker::Mode mode) {
        gpsTracker.setMode(mode);
    }

    /**
     * Set GPS simulation speed (m/s).
     */
    void setGPSSpeed(double speed) {
        gpsTracker.setSpeed(speed);
    }

    /**
     * Set GPS noise level (meters).
     */
    void setGPSNoise(double noiseMeters) {
        gpsTracker.setNoiseLevel(noiseMeters);
    }

    /**
     * Update the GPS simulation.
     * Call every frame with delta time.
     */
    void update(float dt) override {
        // Update GPS tracker
        gpsTracker.update(dt);

        // Get current fix
        const GPSFix& fix = gpsTracker.getCurrentFix();
        if (!fix.isValid) return;

        // Convert to local space
        glm::vec3 localPos = converter.geospatialToLocal(
            fix.latitude, fix.longitude, fix.altitude
        );

        // Update all entities with GeospatialComponent
        // For now, we track the GPS position as a "master" entity
        // In the future, multiple entities can have their own geospatial data
        (void)localPos;  // Used when entities have GeospatialComponent
    }

    /**
     * Update a specific entity's transform from its geospatial component.
     * Call this when an entity's GeospatialComponent changes.
     */
    void updateEntityFromGeospatial(EntityID entityID,
                                     const GeospatialComponent& geo,
                                     TransformComponent& transform) {
        if (!converter.isOriginSet()) {
            std::cerr << "[GeospatialSystem] Origin not set! Cannot convert.\n";
            return;
        }

        // Convert WGS84 to local space
        glm::vec3 localPos = converter.geospatialToLocal(
            geo.latitude, geo.longitude, geo.altitude
        );

        // Update transform
        transform.position = localPos;
    }

    /**
     * Get the current GPS fix.
     */
    const GPSFix& getCurrentGPSFix() const {
        return gpsTracker.getCurrentFix();
    }

    /**
     * Get the GPS tracker.
     */
    GPSTracker& getGPSTracker() { return gpsTracker; }
    const GPSTracker& getGPSTracker() const { return gpsTracker; }

    /**
     * Get the geospatial converter.
     */
    GeospatialConverter& getConverter() { return converter; }
    const GeospatialConverter& getConverter() const { return converter; }

    /**
     * Calculate distance between two entities with GeospatialComponent.
     */
    double distanceBetween(const GeospatialComponent& a,
                           const GeospatialComponent& b) const {
        return GeospatialConverter::haversineDistance(
            a.latitude, a.longitude,
            b.latitude, b.longitude
        );
    }

    /**
     * Calculate bearing from entity A to entity B.
     */
    double bearingBetween(const GeospatialComponent& a,
                          const GeospatialComponent& b) const {
        return GeospatialConverter::bearing(
            a.latitude, a.longitude,
            b.latitude, b.longitude
        );
    }

    /**
     * Get entity count with geospatial data.
     */
    size_t getGeospatialEntityCount() const { return geospatialEntities.size(); }

    const char* getName() const override { return "GeospatialSystem"; }

private:
    GeospatialConverter converter;
    GPSTracker gpsTracker;
    std::vector<EntityID> geospatialEntities;
};

} // namespace ecs
