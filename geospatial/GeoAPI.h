#pragma once

/**
 * GeoAPI - Clean, performant public interface for the geospatial module
 *
 * Design principles:
 * - Batch operations to avoid per-entity overhead
 * - Snapshot/cached reads to avoid recomputing in UI
 * - Lazy evaluation for expensive queries (trajectories, predictions)
 * - Thread-safe read access for UI without blocking ingestion
 * - Zero string allocations in hot paths
 */

#include "GeospatialConverter.h"
#include "GPSTracker.h"
#include "DataFeedManager.h"
#include "TimeSeriesDB.h"
#include "PredictiveModel.h"
#include <glm/glm.hpp>
#include <vector>
#include <string_view>
#include <cstdint>
#include <optional>

// Forward declare ECS components
namespace ecs {
    struct GeospatialComponent;
    struct TransformComponent;
    struct PredictionComponent;
}

namespace geo {

// ============================================================================
// Lightweight data snapshots (no string allocs, POD-friendly)
// ============================================================================

struct EntitySnapshot {
    uint32_t id;
    double latitude;
    double longitude;
    double altitude;
    double speed;
    double heading;
    double accuracy;
    double timestamp;
    bool isValid;
    bool hasPrediction;
    float predictionConfidence;
};

struct GPSStatus {
    double latitude;
    double longitude;
    double altitude;
    double speed;
    double heading;
    double accuracy;
    double timestamp;
    bool isValid;
    GPSTracker::Mode mode;
    const char* modeName;
};

struct GeoStats {
    size_t trackedEntityCount;
    size_t totalPointsStored;
    float predictionInterval;
    double ingestionRate;   // points per second
    double lastUpdateTime;
};

struct GeoConfig {
    double originLat;
    double originLon;
    double originAlt;
    GPSTracker::Mode gpsMode;
    double gpsSpeed;
    double gpsNoiseMeters;
    float predictionInterval;
    int predictionHorizon;
    int predictionPoints;
};

// ============================================================================
// GeoAPI - Single entry point for all geospatial operations
// ============================================================================

class GeoAPI {
public:
    GeoAPI();
    ~GeoAPI();

    // ---- Lifecycle ----
    void initialize(double originLat, double originLon, double originAlt = 0.0);
    void shutdown();

    // ---- Entity tracking (batch operations) ----
    void registerEntity(uint32_t ecsEntityId);
    void unregisterEntity(uint32_t ecsEntityId);
    void unregisterAll();

    // ---- Component attachment (called from ECS system) ----
    void attachComponents(uint32_t ecsEntityId,
                          ecs::GeospatialComponent* geo,
                          ecs::TransformComponent* transform,
                          ecs::PredictionComponent* prediction);

    // ---- Snapshots (cached, O(1) read for UI) ----
    void refreshSnapshots();                        // Call once per frame from ECS sync
    const std::vector<EntitySnapshot>& getSnapshots() const;
    size_t getSnapshotCount() const;

    // Get single entity snapshot by ID (binary search on sorted cache)
    std::optional<EntitySnapshot> getSnapshot(uint32_t ecsEntityId) const;

    // ---- GPS status (zero-copy read) ----
    GPSStatus getGPSStatus() const;

    // ---- Stats (cached, updated on refresh) ----
    GeoStats getStats() const;

    // ---- Configuration ----
    GeoConfig getConfig() const;
    void setGPSMode(GPSTracker::Mode mode);
    void setGPSSpeed(double speed);
    void setGPSNoise(double noiseMeters);
    void setPredictionInterval(float seconds);
    void setPredictionHorizon(double seconds, int points);

    // ---- Coordinate conversion ----
    glm::vec3 geoToLocal(double lat, double lon, double alt) const;
    glm::dvec3 localToGeo(const glm::vec3& local) const;

    // ---- Queries (lazy / cached) ----
    // Returns cached trajectory; refresh only if dirty flag set
    const std::vector<TimeSeriesPoint>& getTrajectory(std::string_view entityId, size_t maxPoints = 100);
    void markTrajectoryDirty();

    // Get predictions (cached from last prediction cycle)
    const std::vector<PredictedState>& getCachedPredictions() const;
    void requestPrediction(double horizon = 60.0, int points = 50);

    // ---- Data feed management ----
    bool addFeed(std::string_view url, std::string_view type = "REST");
    void removeFeed(size_t index);
    size_t getFeedCount() const;

    // ---- Direct subsystem access (for advanced use) ----
    GeospatialConverter& getConverter();
    TimeSeriesDB& getTimeSeriesDB();
    PredictiveModel& getPredictiveModel();
    DataFeedManager& getDataFeedManager();
    GPSTracker& getGPSTracker();

    // ---- Update (called from ECS system) ----
    void update(float dt);

private:
    struct EntityTracking {
        uint32_t ecsId;
        std::string entityId;
        ecs::GeospatialComponent* geo = nullptr;
        ecs::TransformComponent* transform = nullptr;
        ecs::PredictionComponent* prediction = nullptr;
    };

    void syncEntities();
    void processQueuedData();
    void updatePredictions();

    // Subsystems
    GeospatialConverter m_converter;
    GPSTracker m_gpsTracker;
    DataFeedManager m_dataFeedManager;
    TimeSeriesDB m_storageDB;
    PredictiveModel m_predictionModel;

    // Entity tracking
    std::vector<EntityTracking> m_trackedEntities;

    // Cached snapshots (avoids per-frame ECS queries)
    std::vector<EntitySnapshot> m_snapshots;
    bool m_snapshotsDirty = true;

    // Cached predictions
    std::vector<PredictedState> m_cachedPredictions;
    float m_lastPredictionTime = 0.0f;
    float m_predictionInterval = 2.0f;
    double m_predictionHorizon = 60.0;
    int m_predictionPoints = 50;

    // Cached trajectory (lazy)
    std::vector<TimeSeriesPoint> m_cachedTrajectory;
    bool m_trajectoryDirty = true;

    // Stats cache
    GeoStats m_statsCache;
    float m_statsRefreshTimer = 0.0f;

    // System time
    float m_systemTime = 0.0f;

    // Internal ingestion buffer
    struct IngestedData {
        std::string entityId;
        double latitude, longitude, altitude;
        double speed, heading, accuracy, timestamp;
        bool isValid;
    };
    std::vector<IngestedData> m_ingestionBuffer;
};

} // namespace geo
