#pragma once

/**
 * GeospatialSystem - Orchestrator for all geospatial subsystems
 *
 * Coordinates the four specialized systems:
 * - GeoIngestionSystem: GPS input and data feed ingestion
 * - GeoStorageSystem: Time-series database storage
 * - GeoPredictionSystem: Trajectory prediction (Kalman + ML)
 * - GeoVisualizationSystem: OpenGL visualization
 *
 * This system manages the data flow between subsystems:
 * Ingestion -> Storage -> Prediction -> Visualization
 * Each subsystem runs on its own thread where applicable.
 */

#include "../ECS.h"
#include "../components/GeospatialComponent.h"
#include "../components/TransformComponent.h"
#include "../components/PredictionComponent.h"
#include "GeoIngestionSystem.h"
#include "GeoStorageSystem.h"
#include "GeoPredictionSystem.h"
#include "GeoVisualizationSystem.h"
#include "../../geospatial/GeospatialConverter.h"
#include "../../geospatial/GeoAPI.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <iostream>
#include <memory>

namespace ecs {

/**
 * Main GeospatialSystem - Orchestrates all geospatial subsystems
 */
class GeospatialSystem : public System {
public:
    GeospatialSystem() : predictionInterval(2.0f), lastPredictionTime(0.0f), systemTime(0.0f) {}

    ~GeospatialSystem() = default;

    /**
     * Initialize all geospatial subsystems
     */
    void initialize(double originLat, double originLon, double originAlt = 0.0) {
        // Initialize all subsystems
        ingestionSystem.initialize(originLat, originLon, originAlt);
        storageSystem.initialize("http://localhost", 8086, "geospatial");

        // Set Python environment path BEFORE loading the model
        predictionSystem.setPythonEnvPath("/home/run-time-terror/Documents/3D GAME ENGINE/tflite_venv");

        predictionSystem.initialize(originLat, originLon, "geospatial/trajectory_predict.tflite");
        visualizationSystem.init();

        // Start multithreaded subsystems
        ingestionSystem.startIngestion();

        entityStates.reserve(64);

        std::cout << "[GeospatialSystem] Orchestrator initialized at origin: "
                  << originLat << "°, " << originLon << "°, " << originAlt << "m\n";
    }

    /**
     * Register an entity for geospatial tracking across all subsystems
     */
    void registerEntity(EntityID id, GeospatialComponent* geo, TransformComponent* transform,
                        PredictionComponent* prediction = nullptr) {
        if (!geo || !transform) {
            std::cout << "[GeospatialSystem] Failed to register entity " << id
                      << ": missing required components\n";
            return;
        }

        GeoEntityState state;
        state.id = id;
        state.geo = geo;
        state.transform = transform;
        state.prediction = prediction;
        state.isActive = true;

        entityStates.push_back(state);
        geospatialEntities.push_back(id);

        geo->entityId = "entity_" + std::to_string(id);

        // Register with visualization system
        visualizationSystem.registerEntity(id, geo, transform, prediction);

        std::cout << "[GeospatialSystem] Registered entity " << id
                  << " (" << geo->entityId << ")\n";
    }

    /**
     * Unregister an entity
     */
    void unregisterEntity(EntityID id) {
        entityStates.erase(
            std::remove_if(entityStates.begin(), entityStates.end(),
                [id](const GeoEntityState& s) { return s.id == id; }),
            entityStates.end());
        geospatialEntities.erase(
            std::remove_if(geospatialEntities.begin(), geospatialEntities.end(),
                [id](EntityID eid) { return eid == id; }),
            geospatialEntities.end());

        visualizationSystem.unregisterEntity(id);
    }

    /**
     * Set GPS tracker mode
     */
    void setGPSMode(GPSTracker::Mode mode) {
        ingestionSystem.setGPSMode(mode);
    }

    /**
     * Set GPS simulation speed
     */
    void setGPSSpeed(double speed) {
        ingestionSystem.setGPSSpeed(speed);
    }

    /**
     * Set GPS noise level
     */
    void setGPSNoise(double noiseMeters) {
        ingestionSystem.setGPSNoise(noiseMeters);
    }

    /**
     * Main update - processes data flow between subsystems
     */
    void update(float dt) override {
        systemTime += dt;

        // Update ingestion system (GPS + feeds)
        ingestionSystem.update(dt);

        // Consume ingested data and pass to storage
        auto ingestedData = ingestionSystem.consumeIngestedData();
        for (const auto& data : ingestedData) {
            if (!data.isValid) continue;

            // Store in time-series DB
            TimeSeriesPoint tsPoint;
            tsPoint.timestamp = data.timestamp;
            tsPoint.latitude = data.latitude;
            tsPoint.longitude = data.longitude;
            tsPoint.altitude = data.altitude;
            tsPoint.speed = data.speed;
            tsPoint.heading = data.heading;
            tsPoint.accuracy = data.accuracy;
            tsPoint.entityId = data.entityId;
            storageSystem.store(tsPoint);

            // Add to prediction model
            predictionSystem.addObservation(data.latitude, data.longitude,
                                           data.timestamp, data.speed, data.heading);

            // Update registered entities
            for (auto& state : entityStates) {
                if (!state.isActive || !state.geo) continue;

                if (state.geo->entityId == data.entityId) {
                    glm::vec3 localPos = ingestionSystem.getConverter().geospatialToLocal(
                        data.latitude, data.longitude, data.altitude);

                    if (state.transform) {
                        state.transform->position = localPos;
                    }

                    state.geo->latitude = data.latitude;
                    state.geo->longitude = data.longitude;
                    state.geo->altitude = data.altitude;
                    state.geo->horizontalAccuracy = data.accuracy;
                    state.geo->timestamp = data.timestamp;
                }
            }
        }

        // Generate predictions at interval
        lastPredictionTime += dt;
        if (lastPredictionTime >= predictionInterval) {
            lastPredictionTime = 0.0f;

            auto fix = ingestionSystem.getCurrentGPSFix();
            if (fix.isValid) {
                auto predictionResult = predictionSystem.generatePredictions(
                    fix.timestamp, 60.0, 50);

                // Update prediction components
                for (auto& state : entityStates) {
                    if (!state.isActive || !state.prediction) continue;
                    predictionSystem.updatePredictionComponent(*state.prediction, predictionResult);
                }
            }
        }

        // Update storage system (processes write queue)
        storageSystem.update(dt);

        // Update prediction system (processes request queue)
        predictionSystem.update(dt);

        // Update visualization system (renders)
        visualizationSystem.update(dt);
    }

    // Access to subsystems
    GeoIngestionSystem& getIngestionSystem() { return ingestionSystem; }
    GeoStorageSystem& getStorageSystem() { return storageSystem; }
    GeoPredictionSystem& getPredictionSystem() { return predictionSystem; }
    GeoVisualizationSystem& getVisualizationSystem() { return visualizationSystem; }

    const GPSFix& getCurrentGPSFix() const { return ingestionSystem.getCurrentGPSFix(); }
    GPSTracker& getGPSTracker() { return ingestionSystem.getGPSTracker(); }
    GeospatialConverter& getConverter() { return ingestionSystem.getConverter(); }
    DataFeedManager& getDataFeedManager() { return ingestionSystem.getDataFeedManager(); }
    TimeSeriesDB& getTimeSeriesDB() { return storageSystem.getTimeSeriesDB(); }
    PredictiveModel& getPredictiveModel() { return predictionSystem.getPredictiveModel(); }

    std::vector<PredictedState> getPredictedTrajectory(double horizon = 60.0, int points = 50) {
        return predictionSystem.getPredictiveModel().predictTrajectory(
            getCurrentGPSFix().timestamp, horizon, points);
    }

    std::vector<TimeSeriesPoint> getEntityTrajectory(const std::string& entityId,
                                                      size_t maxPoints = 100) {
        return storageSystem.getTrajectory(entityId, maxPoints);
    }

    std::vector<TimeSeriesPoint> queryTimeRange(double startTime, double endTime,
                                                const std::string& entityId = "") {
        return storageSystem.queryByTimeRange(startTime, endTime, entityId);
    }

    TimeSeriesDB::PlaybackController createPlayback() {
        return storageSystem.createPlayback();
    }

    geo::GeoAPI& getGeoAPI() { return geoAPI; }
    const geo::GeoAPI& getGeoAPI() const { return geoAPI; }

    double distanceBetween(const GeospatialComponent& a,
                           const GeospatialComponent& b) const {
        return GeospatialConverter::haversineDistance(
            a.latitude, a.longitude, b.latitude, b.longitude);
    }

    double bearingBetween(const GeospatialComponent& a,
                          const GeospatialComponent& b) const {
        return GeospatialConverter::bearing(
            a.latitude, a.longitude, b.latitude, b.longitude);
    }

    size_t getGeospatialEntityCount() const { return geospatialEntities.size(); }

    void setPredictionInterval(float interval) { predictionInterval = interval; }
    float getPredictionInterval() const { return predictionInterval; }

    const char* getName() const override { return "GeospatialSystem"; }

private:
    struct GeoEntityState {
        EntityID id;
        GeospatialComponent* geo = nullptr;
        TransformComponent* transform = nullptr;
        PredictionComponent* prediction = nullptr;
        bool isActive = false;
    };

    GeoIngestionSystem ingestionSystem;
    GeoStorageSystem storageSystem;
    GeoPredictionSystem predictionSystem;
    GeoVisualizationSystem visualizationSystem;

    geo::GeoAPI geoAPI;

    std::vector<EntityID> geospatialEntities;
    std::vector<GeoEntityState> entityStates;

    float predictionInterval;
    float lastPredictionTime;
    float systemTime;
};

} // namespace ecs
