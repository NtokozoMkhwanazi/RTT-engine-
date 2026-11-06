#include "GeoAPI.h"
#include "../ecs/components/TransformComponent.h"
#include "../ecs/components/GeospatialComponent.h"
#include "../ecs/components/PredictionComponent.h"
#include "../ecs/systems/GeoIngestionSystem.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <limits>
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

// Escape a string for safe inclusion inside a JSON string literal.
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Maps dynamic asset-type text strings from telemetry nodes (corporate data
// feeds use a variety of naming conventions) to the strongly-typed AssetKind
// enum. Used by ParseFeedPayload to classify every ingested twin.
geo::AssetKind MapStringToAssetKind(const std::string& inputStr) {
    // Convert to lowercase to stay robust against telemetry variations.
    std::string s = inputStr;
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);

    if (s.empty()) return geo::AssetKind::Unknown;

    // Personnel aliases
    if (s == "human" || s == "personnel" || s == "miner" || s == "worker" ||
        s == "pedestrian" || s == "foot") {
        return geo::AssetKind::Personnel;
    }

    // Heavy Machinery aliases (common in mining and civil sectors). Note:
    // "truck" resolves to HeavyMachinery here because the industrial feed
    // vocabulary overwhelmingly means haul trucks; light vehicles come in as
    // "car"/"utility"/"van"/"bakkie" instead.
    if (s == "machinery" || s == "heavy" || s == "excavator" || s == "truck" ||
        s == "dragline" || s == "loader" || s == "drill") {
        return geo::AssetKind::HeavyMachinery;
    }

    // Light Vehicle aliases
    if (s == "car" || s == "vehicle" || s == "light_vehicle" || s == "utility" ||
        s == "bakkie" || s == "van") {
        return geo::AssetKind::LightVehicle;
    }

    // Static infrastructure / IoT sensors
    if (s == "sensor" || s == "static" || s == "monitor" || s == "gas" ||
        s == "geophone" || s == "fan") {
        return geo::AssetKind::StaticSensor;
    }

    return geo::AssetKind::Unknown;
}

// Parse a REST feed payload into a GeoDataPoint. Handles the common shapes:
// a flat object {"lat":...,"lon":...,"speed":...} or a single-element
// array of such objects. Also classifies the twin via the asset-type field.
GeoDataPoint ParseFeedPayload(const std::string& json) {
    GeoDataPoint p;
    // Baseline type context + raw capture for debugging.
    p.dataType = "digital_twin_telemetry";
    p.rawJson = json;
    if (json.empty()) return p;

    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(json);
    if (!Json::parseFromStream(builder, ss, &root, &errs)) return p;

    const Json::Value* obj = &root;
    if (root.isArray() && root.size() > 0) obj = &root[0];
    if (!obj->isObject()) return p;

    // 1. Direct spatial vector allocations (richer alias set per data feed).
    p.latitude  = FeedNumber(*obj, {"lat", "latitude", "lat_deg"});
    p.longitude = FeedNumber(*obj, {"lon", "lng", "longitude", "lon_deg"});
    p.altitude  = FeedNumber(*obj, {"alt", "altitude", "height", "z"});
    p.speed     = FeedNumber(*obj, {"speed", "velocity", "mps"});
    p.heading   = FeedNumber(*obj, {"heading", "course", "bearing", "track"});
    p.accuracy  = FeedNumber(*obj, {"accuracy", "hdop", "precision"});
    p.timestamp = FeedNumber(*obj, {"timestamp", "ts", "time"});
    if (p.timestamp == 0.0) {
        p.timestamp = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // 2. Identify unique global asset ID handles (fall back to assetId, then
    //    an unidentified-twin placeholder so feeds never collapse into one
    //    time-series entity).
    const Json::Value& id = (*obj)["id"];
    if (id.isString())                 p.sourceId = id.asString();
    else if (id.isInt())               p.sourceId = std::to_string(id.asInt());
    else if ((*obj)["assetId"].isString()) p.sourceId = (*obj)["assetId"].asString();
    else                               p.sourceId = "unidentified_twin";

    // 3. Digital-twin classification extraction: look for common property
    //    classification keywords used across corporate data feeds, then pack
    //    the resolved type string (and its enum) into the data point.
    std::string typeString = "";
    if ((*obj)["asset_type"].isString())    typeString = (*obj)["asset_type"].asString();
    else if ((*obj)["type"].isString())     typeString = (*obj)["type"].asString();
    else if ((*obj)["kind"].isString())     typeString = (*obj)["kind"].asString();
    else if ((*obj)["category"].isString()) typeString = (*obj)["category"].asString();
    else if ((*obj)["class"].isString())    typeString = (*obj)["class"].asString();

    p.dataType = typeString.empty() ? "unknown" : typeString;

    return p;
}

} // namespace

namespace geo {

GeoAPI::GeoAPI() = default;
GeoAPI::~GeoAPI() = default;

void GeoAPI::initialize(double originLat, double originLon, double originAlt) {
    m_converter.setOrigin(originLat, originLon, originAlt);
    // Seed the prediction model's WGS84->local origin (incl. altitude) so its
    // lat/lon/alt -> East/North/Z conversion is correct when observations flow.
    m_predictionModel.initialize(originLat, originLon, originAlt);
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
        s.satellites = m_lastFix.satelliteCount;
    }
    s.mode = m_gpsTracker.getMode();
    s.modeName = GPSTracker::getModeName(s.mode);
    return s;
}

GeoStats GeoAPI::getStats() const {
    return m_statsCache;
}

std::string GeoAPI::buildFeedJson() const {
    const GPSStatus gps = getGPSStatus();
    const GeoStats stats = getStats();

    // Number formatting helper: always emit valid JSON numbers (no "nan"/"inf").
    auto num = [](double v) -> std::string {
        if (v != v || v ==  std::numeric_limits<double>::infinity() ||
            v == -std::numeric_limits<double>::infinity()) {
            return "null";
        }
        return std::to_string(v);
    };

    std::string json;
    json.reserve(1024);
    json += "{";

    // Live fix
    json += "\"mode\":\"" + JsonEscape(gps.modeName ? gps.modeName : "UNKNOWN") + "\",";
    json += "\"tracking\":\"" + std::string(m_trackEntity ? "entity" : "simulated") + "\",";
    json += "\"timestamp\":" + num(gps.timestamp);

    json += ",\"fix\":{";
    json += "\"valid\":" + std::string(gps.isValid ? "true" : "false") + ",";
    json += "\"latitude\":" + num(gps.latitude) + ",";
    json += "\"longitude\":" + num(gps.longitude) + ",";
    json += "\"altitude\":" + num(gps.altitude) + ",";
    json += "\"speed_mps\":" + num(gps.speed) + ",";
    json += "\"heading_deg\":" + num(gps.heading) + ",";
    json += "\"accuracy_m\":" + num(gps.accuracy) + ",";
    json += "\"satellites\":" + std::to_string(gps.satellites);
    json += "}";

    // Stats
    json += ",\"stats\":{";
    json += "\"pointsStored\":" + std::to_string(stats.totalPointsStored) + ",";
    json += "\"trackedEntities\":" + std::to_string(stats.trackedEntityCount) + ",";
    json += "\"ingestionRate\":" + num(stats.ingestionRate);
    json += "}";

    // Registered feeds
    json += ",\"feeds\":[";
    const size_t feedCount = getFeedCount();
    for (size_t i = 0; i < feedCount; ++i) {
        const DataFeedManager::FeedInfo info = getFeedInfo(i);
        if (i) json += ",";
        json += "{\"type\":\"" + JsonEscape(info.type) + "\",";
        json += "\"url\":\"" + JsonEscape(info.url) + "\"}";
    }
    json += "]";

    // Tracked physics twins - colour-coded on the canvas by `type`, each with its
    // own prediction cone when it has observations (per-twin 3D Kalman).
    json += ",\"physics\":[";
    const auto& snaps = getSnapshots();
    bool first = true;
    for (const auto& s : snaps) {
        if (!s.isValid) continue;
        if (!first) json += ",";
        first = false;
        json += "{\"lat\":" + num(s.latitude) + ",\"lon\":" + num(s.longitude) + ",";
        json += "\"alt\":" + num(s.altitude) + ",";
        json += "\"type\":\"" + JsonEscape(AssetKindToString(s.kind)) + "\",";
        json += "\"hasPrediction\":" + std::string(s.hasPrediction ? "true" : "false") + ",";
        json += "\"confidence\":" + num(s.predictionConfidence);
        if (s.hasPrediction) {
            json += ",\"predictions\":[";
            bool pfirst = true;
            for (const auto& p : s.predictions) {
                if (!pfirst) json += ",";
                pfirst = false;
                json += "{\"lat\":" + num(p.latitude) + ",\"lon\":" + num(p.longitude) + ",";
                json += "\"alt\":" + num(p.altitude) + ",";
                json += "\"confidence\":" + num(p.confidence) + ",";
                json += "\"uncertaintyRadius\":" + num(p.uncertaintyRadius) + "}";
            }
            json += "]";
        }
        json += "}";
    }
    json += "]";  // close physics[]

    // Live walker trajectory trail (Todo 7: web enrich). The dashboard canvas
    // plots these as a connected path; previously this was always empty, so
    // the plot read "no points yet" even though the engine walked the path.
    json += ",\"trajectory\":[";
    {
        std::lock_guard<std::mutex> lk(m_geoMutex);
        bool tfirst = true;
        for (const auto& p : m_liveTrajectory) {
            if (!tfirst) json += ",";
            tfirst = false;
            json += "{\"lat\":" + num(p.latitude) + ",\"lon\":" + num(p.longitude) + "}";
        }
    }
    json += "]";

    // Recent proximate-pair safety alerts (Todo 7: web enrich). The engine
    // computes these in SafetyGeofenceSystem but previously only logged them to
    // the editor console; they are now surfaced on the dashboard too.
    json += ",\"alerts\":[";
    {
        std::lock_guard<std::mutex> lk(m_geoMutex);
        bool afirst = true;
        for (const auto& a : m_recentAlerts) {
            if (!afirst) json += ",";
            afirst = false;
            json += "{\"entityA\":" + std::to_string(a.entityA) + ",";
            json += "\"entityB\":" + std::to_string(a.entityB) + ",";
            json += "\"kindA\":\"" + JsonEscape(AssetKindToString(a.kindA)) + "\",";
            json += "\"kindB\":\"" + JsonEscape(AssetKindToString(a.kindB)) + "\",";
            json += "\"distanceMeters\":" + num(a.distanceMeters) + ",";
            json += "\"horizontalMeters\":" + num(a.horizontalMeters) + ",";
            json += "\"verticalDeltaM\":" + num(a.verticalDeltaM) + ",";
            json += "\"level\":" + std::to_string(a.level) + ",";
            json += "\"timestamp\":" + num(a.timestamp) + ",";
            json += "\"escapeBearingDeg\":" + num(a.escapeBearingDeg) + ",";
            json += "\"evacLatitude\":" + num(a.evacLatitude) + ",";
            json += "\"evacLongitude\":" + num(a.evacLongitude) + ",";
            json += "\"evacAltitude\":" + num(a.evacAltitude) + ",";
            json += "\"evacuationDistance\":" + num(a.evacuationDistance) + ",";
            json += "\"message\":\"" + JsonEscape(a.message) + "\"}";
        }
    }
    json += "]";

    // Cached 3D Kalman predictions for the live entity (Todo 9/10). The
    // dashboard plots these as a forward "prediction cone" + a labelled next-fix,
    // not just a dot. Each point carries lat/lon/alt, heading, speed, confidence
    // and a positional uncertainty radius (metres) so the cone can be drawn.
    json += ",\"predictions\":[";
    {
        const auto& preds = getCachedPredictions();
        std::lock_guard<std::mutex> lk(m_geoMutex);
        bool pfirst = true;
        for (const auto& p : preds) {
            if (!pfirst) json += ",";
            pfirst = false;
            json += "{\"timestamp\":" + num(p.timestamp) + ",";
            json += "\"latitude\":" + num(p.latitude) + ",";
            json += "\"longitude\":" + num(p.longitude) + ",";
            json += "\"altitude\":" + num(p.altitude) + ",";
            json += "\"speed_mps\":" + num(p.speed) + ",";
            json += "\"heading_deg\":" + num(p.heading) + ",";
            json += "\"uncertaintyRadius\":" + num(p.uncertaintyRadius) + ",";
            json += "\"confidence\":" + num(p.confidence) + "}";
        }
    }
    json += "]";

    json += "}";  // close root object

    return json;
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

        // --- Pass asset classification along ---
        // Resolve the raw feed type string (p.dataType) to the strongly-typed
        // AssetKind enum plus a canonical, web-dashboard-friendly name. The
        // /feed JSON provider + ECS read these off the drained FeedDataPoint
        // to color-code / route twins without dropping a frame.
        d.assetKind = MapStringToAssetKind(p.dataType);
        d.assetTypeString = geo::AssetKindToString(d.assetKind);

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
        // Forward the asset-twin classification resolved in the addFeed sink.
        p.assetTypeString = std::move(d.assetTypeString);
        p.assetKind = d.assetKind;
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

    // Append the live fix to the dashboard trajectory ring (Todo 7: surfaced
    // as trajectory[] on /feed so the canvas draws a live trail instead of a
    // blank "no points yet" view). syncFrom() runs on the main thread every
    // frame; buildFeedJson() reads it from the server thread.
    if (m_lastFix.isValid) {
        std::lock_guard<std::mutex> lk(m_geoMutex);
        m_liveTrajectory.push_back(TrajectoryPoint{
            m_lastFix.latitude, m_lastFix.longitude,
            m_lastFix.altitude, m_lastFix.timestamp});
        if (m_liveTrajectory.size() > kMaxLiveTrajectory)
            m_liveTrajectory.erase(m_liveTrajectory.begin(),
                                   m_liveTrajectory.end() - kMaxLiveTrajectory);
    }
}

void GeoAPI::recordAlert(const LiveAlert& alert) {
    std::lock_guard<std::mutex> lk(m_geoMutex);
    m_recentAlerts.push_back(alert);
    if (m_recentAlerts.size() > kMaxRecentAlerts)
        m_recentAlerts.erase(m_recentAlerts.begin(),
                             m_recentAlerts.end() - kMaxRecentAlerts);
}

const std::vector<LiveAlert>& GeoAPI::getRecentAlerts() const {
    return m_recentAlerts;  // read-only; callers that mutate should lock
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
                            double speed, double heading, double altitude) {
    m_predictionModel.addObservation(lat, lon, timestamp, speed, heading, altitude);
}

void GeoAPI::syncEntities() {
}

void GeoAPI::processQueuedData() {
}

void GeoAPI::updatePredictions() {
    // Live the 3D Kalman: feed the latest valid fix (with altitude) into the
    // model so it learns the real walk/climb trajectory, then refresh the cached
    // predictions on the configured interval. The dashboard renders the result
    // (Todo 9/10: prediction is no longer dormant in the engine path).
    const GPSStatus fix = getGPSStatus();
    if (!fix.isValid) return;

    addObservation(fix.latitude, fix.longitude, fix.timestamp,
                   fix.speed, fix.heading, fix.altitude);

    if (fix.timestamp - m_lastPredictionTime >= m_predictionInterval) {
        requestPrediction(m_predictionHorizon, m_predictionPoints);
        m_lastPredictionTime = fix.timestamp;
    }
}

} // namespace geo