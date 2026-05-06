#include "GeoAPI.h"
#include "../ecs/components/TransformComponent.h"
#include "../ecs/components/GeospatialComponent.h"
#include "../ecs/components/PredictionComponent.h"
#include <algorithm>
#include <iostream>
#include <chrono>

namespace geo {

GeoAPI::GeoAPI() = default;
GeoAPI::~GeoAPI() = default;

void GeoAPI::initialize(double originLat, double originLon, double originAlt) {
    m_converter.setOrigin(originLat, originLon, originAlt);
    m_gpsTracker.setOrigin(originLat, originLon);
    m_storageDB.initialize("http://localhost", 8086, "geospatial");
    m_predictionModel.initialize(originLat, originLon);

    m_snapshots.reserve(64);
    m_trackedEntities.reserve(64);

    std::cout << "[GeoAPI] Initialized at origin: "
              << originLat << "°, " << originLon << "°, " << originAlt << "m\n";
}

void GeoAPI::shutdown() {
    m_trackedEntities.clear();
    m_snapshots.clear();
    m_cachedPredictions.clear();
    m_cachedTrajectory.clear();
}

void GeoAPI::registerEntity(uint32_t ecsEntityId) {
    for (const auto& e : m_trackedEntities) {
        if (e.ecsId == ecsEntityId) return;
    }

    EntityTracking tracking;
    tracking.ecsId = ecsEntityId;
    tracking.entityId = "entity_" + std::to_string(ecsEntityId);
    m_trackedEntities.push_back(tracking);
    m_snapshotsDirty = true;

    std::cout << "[GeoAPI] Registered entity " << ecsEntityId << "\n";
}

void GeoAPI::unregisterEntity(uint32_t ecsEntityId) {
    m_trackedEntities.erase(
        std::remove_if(m_trackedEntities.begin(), m_trackedEntities.end(),
            [ecsEntityId](const EntityTracking& e) { return e.ecsId == ecsEntityId; }),
        m_trackedEntities.end());
    m_snapshotsDirty = true;
}

void GeoAPI::unregisterAll() {
    m_trackedEntities.clear();
    m_snapshots.clear();
    m_snapshotsDirty = true;
}

void GeoAPI::attachComponents(uint32_t ecsEntityId,
                               ecs::GeospatialComponent* geo,
                               ecs::TransformComponent* transform,
                               ecs::PredictionComponent* prediction) {
    for (auto& e : m_trackedEntities) {
        if (e.ecsId == ecsEntityId) {
            e.geo = geo;
            e.transform = transform;
            e.prediction = prediction;
            return;
        }
    }
}

void GeoAPI::syncEntities() {
    m_snapshots.clear();
    m_snapshots.reserve(m_trackedEntities.size());

    for (auto& tracking : m_trackedEntities) {
        EntitySnapshot snap{};
        snap.id = tracking.ecsId;

        if (tracking.geo && tracking.geo->isValid()) {
            snap.latitude = tracking.geo->latitude;
            snap.longitude = tracking.geo->longitude;
            snap.altitude = tracking.geo->altitude;
            snap.accuracy = tracking.geo->horizontalAccuracy;
            snap.timestamp = tracking.geo->timestamp;
            snap.isValid = true;
        }

        if (tracking.prediction && tracking.prediction->isValid) {
            snap.hasPrediction = true;
            if (!tracking.prediction->predictions.empty()) {
                snap.predictionConfidence = tracking.prediction->predictions.back().confidence;
            }
        }

        snap.speed = 0.0;
        snap.heading = 0.0;

        m_snapshots.push_back(snap);
    }

    m_snapshotsDirty = false;
}

const std::vector<EntitySnapshot>& GeoAPI::getSnapshots() const {
    return m_snapshots;
}

size_t GeoAPI::getSnapshotCount() const {
    return m_snapshots.size();
}

std::optional<EntitySnapshot> GeoAPI::getSnapshot(uint32_t ecsEntityId) const {
    auto it = std::lower_bound(m_snapshots.begin(), m_snapshots.end(), ecsEntityId,
        [](const EntitySnapshot& a, uint32_t id) { return a.id < id; });
    if (it != m_snapshots.end() && it->id == ecsEntityId) {
        return *it;
    }
    return std::nullopt;
}

GPSStatus GeoAPI::getGPSStatus() const {
    const GPSFix& fix = const_cast<GPSTracker&>(m_gpsTracker).getCurrentFix();
    GPSStatus status{};
    status.latitude = fix.latitude;
    status.longitude = fix.longitude;
    status.altitude = fix.altitude;
    status.speed = fix.speed;
    status.heading = fix.heading;
    status.accuracy = fix.horizontalAccuracy;
    status.timestamp = fix.timestamp;
    status.isValid = fix.isValid;
    status.mode = m_gpsTracker.getMode();
    status.modeName = m_gpsTracker.getCurrentModeName();
    return status;
}

GeoStats GeoAPI::getStats() const {
    return m_statsCache;
}

GeoConfig GeoAPI::getConfig() const {
    GeoConfig cfg{};
    cfg.originLat = m_converter.getOriginLat();
    cfg.originLon = m_converter.getOriginLon();
    cfg.originAlt = m_converter.getOriginAlt();
    cfg.gpsMode = m_gpsTracker.getMode();
    cfg.gpsSpeed = 1.4;  // default sim speed
    cfg.gpsNoiseMeters = 2.0; // default noise
    cfg.predictionInterval = m_predictionInterval;
    cfg.predictionHorizon = m_predictionHorizon;
    cfg.predictionPoints = m_predictionPoints;
    return cfg;
}

void GeoAPI::setGPSMode(GPSTracker::Mode mode) {
    m_gpsTracker.setMode(mode);
}

void GeoAPI::setGPSSpeed(double speed) {
    m_gpsTracker.setSpeed(speed);
}

void GeoAPI::setGPSNoise(double noiseMeters) {
    m_gpsTracker.setNoiseLevel(noiseMeters);
}

void GeoAPI::setPredictionInterval(float seconds) {
    m_predictionInterval = seconds;
}

void GeoAPI::setPredictionHorizon(double seconds, int points) {
    m_predictionHorizon = seconds;
    m_predictionPoints = points;
}

glm::vec3 GeoAPI::geoToLocal(double lat, double lon, double alt) const {
    return m_converter.geospatialToLocal(lat, lon, alt);
}

glm::dvec3 GeoAPI::localToGeo(const glm::vec3& local) const {
    return m_converter.localToGeospatial(local);
}

const std::vector<TimeSeriesPoint>& GeoAPI::getTrajectory(std::string_view entityId, size_t maxPoints) {
    if (m_trajectoryDirty) {
        m_cachedTrajectory = m_storageDB.getTrajectory(std::string(entityId), maxPoints);
        m_trajectoryDirty = false;
    }
    return m_cachedTrajectory;
}

void GeoAPI::markTrajectoryDirty() {
    m_trajectoryDirty = true;
}

const std::vector<PredictedState>& GeoAPI::getCachedPredictions() const {
    return m_cachedPredictions;
}

void GeoAPI::requestPrediction(double horizon, int points) {
    m_predictionHorizon = horizon;
    m_predictionPoints = points;
    m_cachedPredictions = m_predictionModel.predictTrajectory(
        m_gpsTracker.getCurrentFix().timestamp, horizon, points);
}

// Feed management - simplified wrapper around DataFeedManager
struct FeedInfo {
    std::string url;
    std::string type;
    bool active;
};
static std::vector<FeedInfo> s_feedRegistry;

bool GeoAPI::addFeed(std::string_view url, std::string_view type) {
    std::string urlStr(url);
    std::string typeStr(type);

    // Store in registry for UI display
    s_feedRegistry.push_back({urlStr, typeStr, true});

    // Register actual feed with callback to push to storage
    m_dataFeedManager.addRESTFeed(urlStr, 5000, [this](const GeoDataPoint& point) {
        m_dataFeedManager.pushDataPoint(point);
    });

    return true;
}

void GeoAPI::removeFeed(size_t index) {
    if (index < s_feedRegistry.size()) {
        s_feedRegistry.erase(s_feedRegistry.begin() + index);
    }
    // Note: DataFeedManager doesn't support removing individual feeds
    // Would need to stopAllFeeds and re-add remaining ones
}

size_t GeoAPI::getFeedCount() const {
    return s_feedRegistry.size();
}

GeospatialConverter& GeoAPI::getConverter() { return m_converter; }
TimeSeriesDB& GeoAPI::getTimeSeriesDB() { return m_storageDB; }
PredictiveModel& GeoAPI::getPredictiveModel() { return m_predictionModel; }
DataFeedManager& GeoAPI::getDataFeedManager() { return m_dataFeedManager; }
GPSTracker& GeoAPI::getGPSTracker() { return m_gpsTracker; }

void GeoAPI::update(float dt) {
    m_systemTime += dt;

    // Update GPS tracker
    m_gpsTracker.update(dt);

    // Process queued data from feeds
    processQueuedData();

    // Update predictions at interval
    m_lastPredictionTime += dt;
    if (m_lastPredictionTime >= m_predictionInterval) {
        m_lastPredictionTime = 0.0f;
        updatePredictions();
    }

    // Update stats cache every 0.5s
    m_statsRefreshTimer += dt;
    if (m_statsRefreshTimer >= 0.5f) {
        m_statsRefreshTimer = 0.0f;
        m_statsCache.trackedEntityCount = m_trackedEntities.size();
        m_statsCache.totalPointsStored = m_storageDB.getTotalPoints();
        m_statsCache.predictionInterval = m_predictionInterval;
        m_statsCache.lastUpdateTime = m_systemTime;
    }

    // Mark snapshots dirty for next sync
    m_snapshotsDirty = true;
}

void GeoAPI::processQueuedData() {
    auto queuedPoints = m_dataFeedManager.processQueue();
    for (const auto& point : queuedPoints) {
        // Store in time-series DB
        TimeSeriesPoint tsPoint;
        tsPoint.timestamp = point.timestamp;
        tsPoint.latitude = point.latitude;
        tsPoint.longitude = point.longitude;
        tsPoint.altitude = point.altitude;
        tsPoint.speed = point.speed;
        tsPoint.heading = point.heading;
        tsPoint.accuracy = point.accuracy;
        tsPoint.entityId = point.sourceId;
        m_storageDB.record(tsPoint);

        // Update prediction model
        m_predictionModel.addObservation(point.latitude, point.longitude,
                                         point.timestamp, point.speed, point.heading);

        // Update tracked entities
        for (auto& tracking : m_trackedEntities) {
            if (tracking.geo && tracking.entityId == point.sourceId) {
                tracking.geo->latitude = point.latitude;
                tracking.geo->longitude = point.longitude;
                tracking.geo->altitude = point.altitude;
                tracking.geo->horizontalAccuracy = point.accuracy;
                tracking.geo->timestamp = point.timestamp;

                if (tracking.transform) {
                    tracking.transform->position = m_converter.geospatialToLocal(
                        point.latitude, point.longitude, point.altitude);
                }
            }
        }

        m_trajectoryDirty = true;
    }

    // Also consume simulated GPS data
    const GPSFix& fix = m_gpsTracker.getCurrentFix();
    if (fix.isValid && !m_trackedEntities.empty()) {
        // For single-entity simulation mode, update first tracked entity
        auto& tracking = m_trackedEntities[0];
        if (tracking.geo) {
            tracking.geo->latitude = fix.latitude;
            tracking.geo->longitude = fix.longitude;
            tracking.geo->altitude = fix.altitude;
            tracking.geo->horizontalAccuracy = fix.horizontalAccuracy;
            tracking.geo->timestamp = fix.timestamp;

            if (tracking.transform) {
                tracking.transform->position = m_converter.geospatialToLocal(
                    fix.latitude, fix.longitude, fix.altitude);
            }
        }

        // Add observation to prediction model
        m_predictionModel.addObservation(fix.latitude, fix.longitude,
                                         fix.timestamp, fix.speed, fix.heading);
    }
}

void GeoAPI::updatePredictions() {
    const GPSFix& fix = m_gpsTracker.getCurrentFix();
    if (fix.isValid) {
        m_cachedPredictions = m_predictionModel.predictTrajectory(
            fix.timestamp, m_predictionHorizon, m_predictionPoints);

        // Update prediction components on tracked entities
        for (auto& tracking : m_trackedEntities) {
            if (tracking.prediction && !m_cachedPredictions.empty()) {
                tracking.prediction->isValid = true;
                tracking.prediction->predictions = m_cachedPredictions;

                // Run monte carlo for uncertainty
                auto mcPaths = m_predictionModel.monteCarloSimulation(
                    fix.timestamp, m_predictionHorizon, 20, m_predictionPoints);
                tracking.prediction->monteCarloPaths = mcPaths;

                // Get uncertainty ellipse
                double semiMajor, semiMinor, orientation;
                m_predictionModel.getUncertaintyEllipse(semiMajor, semiMinor, orientation);
                tracking.prediction->uncertaintyEllipseSemiAxes = glm::vec3(
                    static_cast<float>(semiMajor), static_cast<float>(semiMinor), 0.0f);
                tracking.prediction->uncertaintyOrientation = static_cast<float>(orientation);
            }
        }
    }
}

} // namespace geo
