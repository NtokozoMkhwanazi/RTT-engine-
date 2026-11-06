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
#include "../components/ModelComponent.h"   // forwarded to GeoVisualizationSystem::registerEntity
#include "../components/AnimatedCharacterComponent.h" // npcChar field + geo-animated-NPC drive
#include "GeoIngestionSystem.h"
#include "GeoStorageSystem.h"
#include "GeoPredictionSystem.h"
#include "GeoVisualizationSystem.h"
#include "../../geospatial/GeospatialConverter.h"
#include "../../geospatial/GeoAPI.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
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
        // Stash the origin for per-twin 3D Kalman models (each twin gets its own).
        geoOriginLat = originLat; geoOriginLon = originLon; geoOriginAlt = originAlt;

        // Initialize all subsystems
        ingestionSystem.initialize(originLat, originLon, originAlt);
        storageSystem.initialize("http://localhost", 8086, "geospatial");

        // Set Python environment path BEFORE loading the model
        predictionSystem.setPythonEnvPath("/home/run-time-terror/Documents/3D GAME ENGINE/tflite_venv");

        predictionSystem.initialize(originLat, originLon, "geospatial/trajectory_predict.tflite");
        visualizationSystem.init();
        // Bind the WGS84->local converter so prediction cones render in LOCAL
        // space in GeoVisualizationSystem::update (else raw lat/lon-as-meters
        // drifts the cone away from the twin mesh). Available now that
        // ingestionSystem.initialize() has constructed its converter_.
        visualizationSystem.setConverter(&ingestionSystem.getConverter());

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
                        PredictionComponent* prediction = nullptr,
                        ModelComponent* model = nullptr,
                        AnimatedCharacterComponent* npc = nullptr) {
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
        state.npcChar = npc;
        state.isActive = true;
        state.twinModel.initialize(geoOriginLat, geoOriginLon, geoOriginAlt);

        entityStates.push_back(state);
        geospatialEntities.push_back(id);

        geo->entityId = "entity_" + std::to_string(id);

        // Register with visualization system (forward ModelComponent so the viz
        // system can suppress the fallback point-marker for mesh-bearing twins)
        visualizationSystem.registerEntity(id, geo, transform, prediction, model);

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
                                            data.timestamp, data.speed, data.heading,
                                            data.altitude);

            // Update registered entities
            for (auto& state : entityStates) {
                if (!state.isActive || !state.geo) continue;

                if (state.geo->entityId == data.entityId) {
                    glm::vec3 localPos = ingestionSystem.getConverter().geospatialToLocal(
                        data.latitude, data.longitude, data.altitude);

                    if (state.transform) {
                        if (state.npcChar && state.npcChar->character &&
                            state.npcChar->character->ready()) {
                            // Animated NPC geo twin: the geospatial fix OWNS the
                            // character's absolute position (ENU metres) and feeds
                            // heading/speed into the locomotion FSM (| FSM derives
                            // heading from moveDir + plays walk/jog/run by speed).
                            // TickAnimatedCharacter() mirrors character->position
                            // back into the ECS transform, so the live lat/lon fix
                            // is overwritten by the next update -> the NPC tracks
                            // the feed exactly, no velocity drift.
                            state.npcChar->character->position = localPos;
                            state.npcChar->geoDriven = true;
                            const float h = glm::radians((float)data.heading);
                            state.npcChar->geoMoveDir =
                                glm::vec2(-std::sin(h), -std::cos(h));
                            state.npcChar->geoMoveSpeed = float(std::clamp(
                                data.speed / 2.0, 0.0, 1.0));
                        } else {
                            state.transform->position = localPos;
                        }
                    }

                    state.geo->latitude = data.latitude;
                    state.geo->longitude = data.longitude;
                    state.geo->altitude = data.altitude;
                    state.geo->horizontalAccuracy = data.accuracy;
                    state.geo->timestamp = data.timestamp;

                    // Feed this twin's live observation into its OWN 3D Kalman
                    // so its prediction cone leads its real path.
                    state.twinModel.addObservation(data.latitude, data.longitude,
                                                   data.timestamp, data.speed,
                                                   data.heading, data.altitude);
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

                // Per-twin prediction: each tracked twin extrapolates its OWN
                // future from its own observations (external sim feeds that
                // matched it by entityId). predictTrajectory returns {} for
                // twins with no history, so static/unfed twins get no cone.
                for (auto& state : entityStates) {
                    if (!state.isActive) continue;
                    state.lastPredicted = state.twinModel.predictTrajectory(
                        fix.timestamp, 60.0, 50);
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

    /**
     * Build UI-facing snapshots of every tracked entity. Feeds the Geo tracking
     * panel's entity table (geo::GeoAPI::syncFrom).
     */
    std::vector<geo::EntitySnapshot> getEntitySnapshots() const {
        std::vector<geo::EntitySnapshot> snaps;
        snaps.reserve(entityStates.size());
        for (const auto& s : entityStates) {
            if (!s.isActive || !s.geo) continue;
            geo::EntitySnapshot snap{};
            snap.id = static_cast<uint32_t>(s.id);
            snap.latitude = s.geo->latitude;
            snap.longitude = s.geo->longitude;
            snap.altitude = s.geo->altitude;
            snap.accuracy = s.geo->horizontalAccuracy;
            snap.timestamp = s.geo->timestamp;
            snap.isValid = true;
            // Colour the twin on the dashboard by its AssetKind (Todo 7 mesh
            // twins -> now tracked geospatial twins, not orphaned stubs).
            snap.kind = static_cast<geo::AssetKind>(s.geo->assetKind);
            // Per-twin prediction cone: each twin predicts its own future from
            // its own observed trajectory (external sim data matched by id).
            snap.hasPrediction = !s.lastPredicted.empty();
            snap.predictions = s.lastPredicted;
            if (snap.hasPrediction) {
                snap.predictionConfidence =
                    static_cast<float>(s.lastPredicted.back().confidence);
            }
            snaps.push_back(snap);
        }
        return snaps;
    }

    void setPredictionInterval(float interval) { predictionInterval = interval; }
    float getPredictionInterval() const { return predictionInterval; }

    /**
     * Push an explicit observation to a specific tracked twin (by entityId).
     * Used by external sim endpoints that emit per-entity telemetry, and by the
     * editor demo to seed a twin's cone. Feeds that twin's own 3D Kalman.
     */
    bool addTwinObservation(EntityID id, double lat, double lon, double alt,
                            double speed = 0.0, double heading = 0.0,
                            double timestamp = 0.0) {
        for (auto& state : entityStates) {
            if (state.id == id && state.isActive && state.geo) {
                state.twinModel.addObservation(lat, lon, timestamp,
                                               speed, heading, alt);
                state.lastPredicted = state.twinModel.predictTrajectory(
                    timestamp, 60.0, 50);
                return true;
            }
        }
        return false;
    }

    const char* getName() const override { return "GeospatialSystem"; }

private:
    struct GeoEntityState {
        EntityID id;
        GeospatialComponent* geo = nullptr;
        TransformComponent* transform = nullptr;
        PredictionComponent* prediction = nullptr;
        AnimatedCharacterComponent* npcChar = nullptr; // geo-animated Personnel twin (if any)
        bool isActive = false;
        // Per-twin 3D Kalman (Todo 9/10): each tracked twin predicts its OWN
        // future from the ingested/sim observations matched to its entityId,
        // so the dashboard draws a cone per twin instead of one global cone.
        PredictiveModel twinModel;
        std::vector<PredictedState> lastPredicted;
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

    // Origin seeded at initialize(); used to initialize each twin's local
    // WGS84->metric frame for its own 3D Kalman.
    double geoOriginLat = 0.0, geoOriginLon = 0.0, geoOriginAlt = 0.0;
};

} // namespace ecs
