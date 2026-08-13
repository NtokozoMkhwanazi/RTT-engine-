#include "GeoAPI.h"
#include "../ecs/components/TransformComponent.h"
#include "../ecs/components/GeospatialComponent.h"
#include "../ecs/components/PredictionComponent.h"
#include "../ecs/systems/GeoIngestionSystem.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <chrono>
#include <sstream>
#include <json/json.h>

namespace {

std::string FeedToLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Extract a numeric field from a parsed JSON object, trying common aliases.
// Returns 0.0 when absent / not numeric.
double FeedNumber(const Json::Value& root, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        const Json::Value& v = root[k];
        if (v.isDouble()) return v.asDouble();
        if (v.isInt()) return v.asInt();
        if (v.isUInt()) return v.asUInt();
        if (v.isString()) {
            const std::string s = v.asString();
            if (!s.empty()) return std::atof(s.c_str());
        }
    }
    return 0.0;
}

// Parse a REST feed payload into a GeoDataPoint. Handles the common shapes:
// a flat object {"lat":...,"lon":...,"speed":...} or a single-element
// array of such objects.
GeoDataPoint ParseFeedPayload(const std::string& json) {
    GeoDataPoint p;
    if (json.empty()) return p;

    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(json);
    if (!Json::parseFromStream(builder, ss, &root, &errs)) return p;

    const Json::Value* obj = &root;
    if (root.isArray() && root.size() > 0) obj = &root[0];
    if (!obj->isObject()) return p;

    p.latitude = FeedNumber(*obj, {"lat", "latitude"});
    p.longitude = FeedNumber(*obj, {"lon", "lng", "longitude"});
    p.altitude = FeedNumber(*obj, {"alt", "altitude", "height"});
    p.speed = FeedNumber(*obj, {"speed", "velocity"});
    p.heading = FeedNumber(*obj, {"heading", "course", "bearing"});
    p.accuracy = FeedNumber(*obj, {"accuracy", "hdop"});
    p.timestamp = FeedNumber(*obj, {"timestamp", "ts", "time"});
    if (p.timestamp == 0.0) p.timestamp = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const Json::Value& id = (*obj)["id"];
    if (id.isString()) p.sourceId = id.asString();
    else if (id.isInt()) p.sourceId = std::to_string(id.asInt());

    return p;
}

} // namespace

namespace geo {

GeoAPI::GeoAPI() = default;
GeoAPI::~GeoAPI() = default;

void GeoAPI::initialize(double originLat, double originLon, double originAlt) {
    m_converter.setOrigin(originLat, originLon, originAlt);
}

void GeoAPI::shutdown() {
    m_dataFeedManager.stopAllFeeds();
}

void GeoAPI::registerEntity(uint32_t ecsEntityId) {
    for (const auto& e : m_trackedEntities) {
        if (e.ecsId == ecsEntityId) return;
    }

    EntityTracking tracking;
    tracking.ecsId = ecsEntityId;
    m_trackedEntities.push_back(std::move(tracking));
}

void GeoAPI::unregisterEntity(uint32_t ecsEntityId) {
    m_trackedEntities.erase(
        std::remove_if(m_trackedEntities.begin(), m_trackedEntities.end(),
            [ecsEntityId](const auto& e) { return e.ecsId == ecsEntityId; }),
        m_trackedEntities.end()
    );
}

void GeoAPI::unregisterAll() {
    m_trackedEntities.clear();
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

void GeoAPI::refreshSnapshots() {
    // The orchestrator feeds fresh snapshots each frame via syncFrom(); this
    // legacy hook just keeps the flag for API parity.
    m_snapshotsDirty = true;
}

const std::vector<EntitySnapshot>& GeoAPI::getSnapshots() const {
    return m_snapshots;
}

size_t GeoAPI::getSnapshotCount() const {
    return m_snapshots.size();
}

std::optional<EntitySnapshot> GeoAPI::getSnapshot(uint32_t ecsEntityId) const {
    for (const auto& s : m_snapshots) {
        if (s.id == ecsEntityId) return s;
    }
    return std::nullopt;
}

GPSStatus GeoAPI::getGPSStatus() const {
    GPSStatus s{};
    if (m_lastFix.isValid) {
        s.latitude = m_lastFix.latitude;
        s.longitude = m_lastFix.longitude;
        s.altitude = m_lastFix.altitude;
        s.speed = m_lastFix.speed;
        s.heading = m_lastFix.heading;
        s.accuracy = m_lastFix.horizontalAccuracy;
        s.timestamp = m_lastFix.timestamp;
        s.isValid = true;
    }
    s.mode = m_gpsTracker.getMode();
    s.modeName = GPSTracker::getModeName(s.mode);
    return s;
}

GeoStats GeoAPI::getStats() const {
    return m_statsCache;
}

GeoConfig GeoAPI::getConfig() const {
    GeoConfig c{};
    const glm::dvec3 origin = m_converter.getOriginWGS84();
    c.originLat = origin.x;
    c.originLon = origin.y;
    c.originAlt = origin.z;
    c.gpsMode = m_gpsTracker.getMode();
    c.gpsSpeed = m_gpsTracker.getSpeed();
    c.gpsNoiseMeters = m_gpsTracker.getNoiseLevel();
    c.predictionInterval = m_predictionInterval;
    c.predictionHorizon = (int)m_predictionHorizon;
    c.predictionPoints = m_predictionPoints;
    return c;
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
    return m_converter.localToGeospatial(glm::dvec3(local));
}

const std::vector<TimeSeriesPoint>& GeoAPI::getTrajectory(std::string_view entityId, size_t maxPoints) {
    // Cached: only re-query the time-series DB when marked dirty.
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
    if (!m_lastFix.isValid) return;
    m_cachedPredictions = m_predictionModel.predictTrajectory(m_lastFix.timestamp, horizon, points);
}

bool GeoAPI::addFeed(std::string_view url, std::string_view type) {
    // Trim whitespace; reject empty / whitespace-only URLs.
    std::string u(url);
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    u.erase(u.begin(), std::find_if(u.begin(), u.end(), notSpace));
    u.erase(std::find_if(u.rbegin(), u.rend(), notSpace).base(), u.end());
    if (u.empty()) return false;

    const std::string t = FeedToLower(std::string(type));
    const std::string feedUrl = u;
    const bool isWs = (t == "websocket" || t == "ws");

    // Sink: runs on the feed's poll thread; buffers valid points for the
    // main thread to drain into the pipeline. Points without an id fall back
    // to a per-feed entity so multiple feeds don't collapse into one
    // time-series entity.
    auto sink = [this, feedUrl](const GeoDataPoint& p) {
        IngestedData d;
        d.entityId = p.sourceId.empty() ? ("feed_" + feedUrl) : p.sourceId;
        d.latitude = p.latitude;
        d.longitude = p.longitude;
        d.altitude = p.altitude;
        d.speed = p.speed;
        d.heading = p.heading;
        d.accuracy = p.accuracy;
        d.timestamp = p.timestamp;
        d.isValid = (p.latitude >= -90.0 && p.latitude <= 90.0 &&
                     p.longitude >= -180.0 && p.longitude <= 180.0 &&
                     (p.latitude != 0.0 || p.longitude != 0.0));
        if (d.isValid) {
            std::lock_guard<std::mutex> lock(m_feedMutex);
            m_ingestionBuffer.push_back(std::move(d));
        }
    };

    if (isWs) {
        m_dataFeedManager.addWebSocketFeed(feedUrl, sink);
    } else {
        // Poll every 5 s; the payload parser handles common REST GPS shapes.
        m_dataFeedManager.addRESTFeed(feedUrl, 5000, sink, &ParseFeedPayload);
    }

    // Ensure the polling threads are running (no-op once started).
    m_dataFeedManager.startAllFeeds();
    return true;
}

void GeoAPI::removeFeed(size_t index) {
    m_dataFeedManager.removeFeed(index);
}

size_t GeoAPI::getFeedCount() const {
    return m_dataFeedManager.getFeedCount();
}

DataFeedManager::FeedInfo GeoAPI::getFeedInfo(size_t index) const {
    return m_dataFeedManager.getFeedInfo(index);
}

std::vector<FeedDataPoint> GeoAPI::drainFeedData() {
    std::vector<IngestedData> buffer;
    {
        std::lock_guard<std::mutex> lock(m_feedMutex);
        buffer.swap(m_ingestionBuffer);
    }

    std::vector<FeedDataPoint> out;
    out.reserve(buffer.size());
    for (auto& d : buffer) {
        FeedDataPoint p;
        p.latitude = d.latitude;
        p.longitude = d.longitude;
        p.altitude = d.altitude;
        p.speed = d.speed;
        p.heading = d.heading;
        p.accuracy = d.accuracy;
        p.timestamp = d.timestamp;
        p.sourceId = std::move(d.entityId);
        p.isValid = d.isValid;
        out.push_back(std::move(p));
    }
    return out;
}

GeospatialConverter& GeoAPI::getConverter() {
    return m_converter;
}

TimeSeriesDB& GeoAPI::getTimeSeriesDB() {
    return m_storageDB;
}

PredictiveModel& GeoAPI::getPredictiveModel() {
    return m_predictionModel;
}

DataFeedManager& GeoAPI::getDataFeedManager() {
    return m_dataFeedManager;
}

GPSTracker& GeoAPI::getGPSTracker() {
    return m_gpsTracker;
}

void GeoAPI::update(float dt) {
    m_systemTime += dt;
}

void GeoAPI::setTrackEntity(bool on) {
    m_trackEntity = on;
    if (!on) m_externalFixValid = false;
}

bool GeoAPI::isTrackingEntity() const {
    return m_trackEntity;
}

void GeoAPI::pushExternalFix(const ExternalFix& fix) {
    m_externalFix = fix;
    m_externalFixValid = true;
}

bool GeoAPI::hasExternalFix() const {
    return m_externalFixValid;
}

ExternalFix GeoAPI::takeExternalFix() {
    ExternalFix f = m_externalFix;
    m_externalFixValid = false;
    return f;
}

void GeoAPI::syncFrom(const GPSFix& fix, std::vector<EntitySnapshot> snapshots,
                      const GeoStats& stats) {
    m_lastFix = fix;
    m_snapshots = std::move(snapshots);
    m_statsCache = stats;
}

void GeoAPI::syncTo(ecs::GeoIngestionSystem& ingestion) {
    // Apply panel/terminal GPS config to the live pipeline only when changed
    // (avoids resetting simulation state every frame).
    const GPSTracker::Mode mode = m_gpsTracker.getMode();
    if (mode != m_lastPushedMode) {
        ingestion.setGPSMode(mode);
        m_lastPushedMode = mode;
    }
    const double speed = m_gpsTracker.getSpeed();
    if (speed != m_lastPushedSpeed) {
        ingestion.setGPSSpeed(speed);
        m_lastPushedSpeed = speed;
    }
    const double noise = m_gpsTracker.getNoiseLevel();
    if (noise != m_lastPushedNoise) {
        ingestion.setGPSNoise(noise);
        m_lastPushedNoise = noise;
    }
}

void GeoAPI::addObservation(double lat, double lon, double timestamp,
                            double speed, double heading) {
    m_predictionModel.addObservation(lat, lon, timestamp, speed, heading);
}

void GeoAPI::syncEntities() {
}

void GeoAPI::processQueuedData() {
}

void GeoAPI::updatePredictions() {
}

} // namespace geo