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
#include <string>
#include <string_view>
#include <cstdint>
#include <optional>
#include <mutex>

// Forward declare ECS components
namespace ecs {
    struct GeospatialComponent;
    struct TransformComponent;
    struct PredictionComponent;
    class GeoIngestionSystem;  // facade pushes GPS config down to the pipeline
}

namespace geo {

// ============================================================================
// Lightweight data snapshots (no string allocs, POD-friendly)
// ============================================================================

// A live-entity position fed in as the GPS source ("track the bot"): the
// playable character's world position converted to WGS84, like a real GPS
// receiver consuming satellite measurements.
struct ExternalFix {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double speed = 0.0;
    double heading = 0.0;
    double accuracy = 0.0;
};

// Classification of a live telemetry twin. Maps 1:1 to the asset-type tags
// parsed out of incoming feed payloads (see ParseFeedPayload) and consumed
// downstream by the web dashboard color-coding, the InfluxDB tag routing, the
// proximity geofence and the dynamic mesh-attachment renderer.
enum class AssetKind : uint8_t {
    Unknown = 0,
    LightVehicle,   // Cars, bakkies, utility transports
    Personnel,      // Miners, operators, on-foot crew members
    HeavyMachinery, // Excavators, draglines, drills, continuous miners
    StaticSensor    // Gas monitors, seismic geophones, ventilation fans
};

// Canonical, UI-friendly names for each AssetKind. The web dashboard's canvas
// color mapping keys off these normalized strings so alias-heavy feeds like
// "haul_truck" / "diggers" still color-code correctly.
inline const char* AssetKindToString(AssetKind k) {
    switch (k) {
        case AssetKind::LightVehicle:   return "vehicle";
        case AssetKind::Personnel:      return "personnel";
        case AssetKind::HeavyMachinery: return "machinery";
        case AssetKind::StaticSensor:   return "sensor";
        default:                        return "unknown";
    }
}

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
    // Per-twin 3D Kalman prediction (each tracked twin predicts its OWN future,
    // so the dashboard can draw a cone per twin, not just the walker's cone).
    std::vector<PredictedState> predictions;
    AssetKind kind = AssetKind::Unknown; // <--- Asset classification flag
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
    int satellites;        // GPS satellite count (for the web dashboard)
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

// A single recent position of the live GPS fix - surfaced to the web dashboard
// as the trajectory trail. Kept small (capped in GeoAPI) so the canvas shows
// the live path instead of an indefinitely growing buffer.
struct TrajectoryPoint {
    double latitude  = 0.0;
    double longitude = 0.0;
    double altitude  = 0.0;
    double timestamp = 0.0;
};

// A proximate-pair safety alert (from SafetyGeofenceSystem) surfaced on the
// dashboard so operators see proximity conditions in the browser, not only the
// editor console. Defined here with AssetKind-only fields to avoid a
// geospatial <-> geofence include cycle.
struct LiveAlert {
    uint32_t entityA = 0;
    uint32_t entityB = 0;
    AssetKind kindA = AssetKind::Unknown;
    AssetKind kindB = AssetKind::Unknown;
    double distanceMeters   = 0.0;
    double horizontalMeters = 0.0;
    double verticalDeltaM   = 0.0;
    double timestamp        = 0.0;
    int level               = 1;          // 1=warning, 2=critical
    std::string message;
    // Evacuation interlock vector (forGeo.txt §2): reciprocal escape bearing
    // (deg, clockwise from N) and a target WGS84 fix for the personnel twin to
    // steer toward to exit the safety cylinder, plus the planned step distance.
    double escapeBearingDeg   = 0.0;
    double evacLatitude       = 0.0;
    double evacLongitude      = 0.0;
    double evacAltitude       = 0.0;
    double evacuationDistance = 0.0;
};

// A data point drained from feeds registered through this facade (polled in
// the background by the DataFeedManager). GeospatialSystem feeds these into
// the live ingestion -> storage -> prediction pipeline each frame.
struct FeedDataPoint {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double timestamp = 0.0;
    double speed = 0.0;
    double heading = 0.0;
    double accuracy = 0.0;
    std::string sourceId;
    bool isValid = false;
    // Digital-twin classification drained off the parsed feed payload. The
    // enum is the type-safe handle (safety geofence / mesh selection /
    // InfluxDB tags); assetTypeString is the canonical, web-dashboard-friendly
    // name produced by AssetKindToString().
    AssetKind assetKind = AssetKind::Unknown;
    std::string assetTypeString;
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

    // Build the JSON body served by GeoHTTPServer's /feed endpoint (read by
    // the modernized dashboard at http://localhost:8123/). Reflects the live
    // GPS fix, cached stats, registered feeds, tracked physics twins and any
    // cached trajectory - all mutex-protected reads, safe to call from the
    // server's accept thread.
    std::string buildFeedJson() const;

    // ---- Dashboard surfacing (Todo 7+: web enrich) ----
    // Record a proximate-pair safety alert so it is visible on the dashboard,
    // not only the editor console. Called from the SafetyGeofenceSystem alert
    // sink; safe to call from any thread.
    void recordAlert(const LiveAlert& alert);
    // Recent safety alerts surfaced on the dashboard (capped ring, oldest-first).
    const std::vector<LiveAlert>& getRecentAlerts() const;

    // ---- Configuration ----
    GeoConfig getConfig() const;
    void setGPSMode(GPSTracker::Mode mode);
    void setGPSSpeed(double speed);
    void setGPSNoise(double noiseMeters);
    void setPredictionInterval(float seconds);
    void setPredictionHorizon(double seconds, int points);

    // ---- Track a live entity (e.g. the playable character) ----
    // When tracking is enabled, the GPS fix is driven by pushExternalFix()
    // (fed each frame by the main loop from the entity's world position)
    // instead of the internal simulation.
    void setTrackEntity(bool on);
    bool isTrackingEntity() const;
    void pushExternalFix(const ExternalFix& fix);
    bool hasExternalFix() const;
    ExternalFix takeExternalFix();

    // ---- Facade sync (called once per frame by the GeospatialSystem) ----
    // Pushes real fix/snapshot/stats data into this facade so the status bar,
    // panels and terminal read live values.
    void syncFrom(const GPSFix& fix, std::vector<EntitySnapshot> snapshots,
                  const GeoStats& stats);
    // Applies facade GPS config (mode/speed/noise) to the live pipeline when
    // it changed - connects the UI controls to the real ingestion system.
    void syncTo(ecs::GeoIngestionSystem& ingestion);

    // Feed observations into the facade's own predictive model (used by
    // requestPrediction for on-demand queries). Altitude (meters, WGS84) feeds
    // the 3D Kalman filter's Z/vertical-climb state.
    void addObservation(double lat, double lon, double timestamp,
                        double speed = 0.0, double heading = 0.0,
                        double altitude = 0.0);

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

    // Drive one live prediction tick: feed the current GPS fix (with altitude)
    // into the 3D Kalman and refresh the cached predictions on the configured
    // interval. Public so the engine loop + tests can advance the model.
    void updatePredictions();

    // ---- Data feed management ----
    // Registers a real polling feed (REST default; "WebSocket"/"ws" selects
    // the WebSocket slot). Returns false for an empty URL. Polled in the
    // background; parsed points are drained by GeospatialSystem::update and
    // fed into the live pipeline.
    bool addFeed(std::string_view url, std::string_view type = "REST");
    void removeFeed(size_t index);
    size_t getFeedCount() const;
    // Description of the feed at index (for UI listing); empty when out of
    // range.
    DataFeedManager::FeedInfo getFeedInfo(size_t index) const;
    // Atomically drain feed points received since the last call (main thread).
    std::vector<FeedDataPoint> drainFeedData();

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

    // Track-a-live-entity state
    bool m_trackEntity = false;
    bool m_externalFixValid = false;
    ExternalFix m_externalFix;

    // Latest real fix from the pipeline (fed by syncFrom)
    GPSFix m_lastFix{};

    // Last config pushed down to the pipeline (change detection)
    GPSTracker::Mode m_lastPushedMode = GPSTracker::Mode::DISABLED;
    double m_lastPushedSpeed = -1.0;
    double m_lastPushedNoise = -1.0;

    // Cached trajectory (lazy)
    std::vector<TimeSeriesPoint> m_cachedTrajectory;
    bool m_trajectoryDirty = true;

    // Stats cache
    GeoStats m_statsCache{};
    float m_statsRefreshTimer = 0.0f;

    // System time
    float m_systemTime = 0.0f;

    // Internal ingestion buffer (fed by feed-poll threads; drained on the
    // main thread by GeospatialSystem::update).
    struct IngestedData {
        std::string entityId;
        double latitude, longitude, altitude;
        double speed, heading, accuracy, timestamp;
        bool isValid;
        // Asset-twin classification threaded from ParseFeedPayload's type
        // extraction through to the drained FeedDataPoint (and onward to the
        // /feed JSON provider + ECS). See AddFeed's sink lambda.
        std::string assetTypeString;
        AssetKind assetKind = AssetKind::Unknown;
    };
    std::vector<IngestedData> m_ingestionBuffer;
    mutable std::mutex m_feedMutex;

    // ---- Dashboard-live buffers (Todo 7: web enrich) ----
    // Recent positions of the live GPS fix, surfaced to the dashboard as the
    // trajectory trail (the engine computes the path every frame but never
    // exposed it - now surfaced via /feed). Written by syncFrom() (main
    // thread) and read by buildFeedJson() (server thread).
    std::vector<TrajectoryPoint> m_liveTrajectory;
    static constexpr size_t kMaxLiveTrajectory = 500;

    // Recent proximate-pair safety alerts from SafetyGeofenceSystem, surfaced
    // on the dashboard (not only the editor console). Written by recordAlert()
    // (main thread) and read by buildFeedJson() (server thread).
    std::vector<LiveAlert> m_recentAlerts;
    static constexpr size_t kMaxRecentAlerts = 32;

    // Guards the dashboard-live buffers above (m_liveTrajectory / m_recentAlerts).
    mutable std::mutex m_geoMutex;
};

} // namespace geo
