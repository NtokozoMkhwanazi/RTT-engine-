#pragma once

/**
 * Time-Series Database - Historical Data Storage (Phase 2)
 *
 * Uses InfluxDB for persistent time-series storage with in-memory caching.
 * Supports:
 * - In-memory circular buffer (for recent data, fast access)
 * - InfluxDB storage (for persistent data, time-range queries)
 * - Query by time range, entity, area
 * - Playback at various speeds
 */

#include <vector>
#include <deque>
#include <map>
#include <string>
#include <mutex>
#include <chrono>
#include <iostream>
#include "InfluxDBClient.h"

struct TimeSeriesPoint {
    double timestamp = 0.0;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double speed = 0.0;
    double heading = 0.0;
    double accuracy = 0.0;
    std::string entityId;
};

class TimeSeriesDB {
public:
    TimeSeriesDB() : maxMemoryPoints(10000) {}

    /**
     * Initialize with InfluxDB connection
     * Falls back to memory-only if connection fails
     */
    bool initialize(const std::string& influxHost = "http://localhost",
                    int influxPort = 8086,
                    const std::string& database = "geospatial") {
        this->influxHost = influxHost;
        this->influxPort = influxPort;
        this->databaseName = database;

        // Try to connect to InfluxDB
        bool influxConnected = influxClient.connect(influxHost, influxPort, database);
        if (influxConnected) {
            influxClient.createDatabase();
            std::cout << "[TimeSeriesDB] Initialized with InfluxDB: " << influxHost << ":" << influxPort << "/" << database << "\n";
        } else {
            std::cout << "[TimeSeriesDB] InfluxDB unavailable, using memory-only mode\n";
        }

        return true;
    }

    /**
     * Record a data point
     */
    void record(const TimeSeriesPoint& point) {
        std::lock_guard<std::mutex> lock(dataMutex);

        // Add to in-memory buffer
        memoryBuffer.push_back(point);
        if (memoryBuffer.size() > maxMemoryPoints) {
            memoryBuffer.pop_front();
        }

        // Add to entity index
        entityBuffers[point.entityId].push_back(point);
        if (entityBuffers[point.entityId].size() > 1000) {
            entityBuffers[point.entityId].pop_front();
        }

        // Persist to InfluxDB if connected
        if (influxClient.isConnected()) {
            persistPoint(point);
        }
    }

    /**
     * Batch record multiple points
     */
    void recordBatch(const std::vector<TimeSeriesPoint>& points) {
        std::lock_guard<std::mutex> lock(dataMutex);

        for (const auto& point : points) {
            memoryBuffer.push_back(point);
            entityBuffers[point.entityId].push_back(point);

            if (memoryBuffer.size() > maxMemoryPoints) {
                memoryBuffer.pop_front();
            }
            if (entityBuffers[point.entityId].size() > 1000) {
                entityBuffers[point.entityId].pop_front();
            }
        }

        if (influxClient.isConnected() && !points.empty()) {
            persistBatch(points);
        }
    }

    /**
     * Number of points currently stored in memory.
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(dataMutex);
        return memoryBuffer.size();
    }

    /**
     * Query points by time range
     */
    std::vector<TimeSeriesPoint> queryByTimeRange(double startTime, double endTime,
                                                   const std::string& entityId = "") {
        std::lock_guard<std::mutex> lock(dataMutex);

        if (influxClient.isConnected() && !entityId.empty()) {
            return queryInfluxDB(startTime, endTime, entityId);
        }

        // In-memory query
        std::vector<TimeSeriesPoint> result;
        auto it = entityId.empty() ? std::map<std::string, std::deque<TimeSeriesPoint>>{{"*", memoryBuffer}} : entityBuffers;
        if (!entityId.empty()) {
            auto found = entityBuffers.find(entityId);
            if (found != entityBuffers.end()) {
                for (const auto& point : found->second) {
                    if (point.timestamp >= startTime && point.timestamp <= endTime) {
                        result.push_back(point);
                    }
                }
            }
        } else {
            for (const auto& point : memoryBuffer) {
                if (point.timestamp >= startTime && point.timestamp <= endTime) {
                    result.push_back(point);
                }
            }
        }
        return result;
    }

    /**
     * Get trajectory for an entity
     */
    std::vector<TimeSeriesPoint> getTrajectory(const std::string& entityId,
                                                size_t maxPoints = 1000) {
        std::lock_guard<std::mutex> lock(dataMutex);

        if (influxClient.isConnected()) {
            auto rows = influxClient.getTrajectory("points", entityId, maxPoints);
            return rowsToPoints(rows);
        }

        // In-memory fallback
        std::vector<TimeSeriesPoint> result;
        auto it = entityBuffers.find(entityId);
        if (it != entityBuffers.end()) {
            const auto& buffer = it->second;
            size_t startIdx = (buffer.size() > maxPoints) ? buffer.size() - maxPoints : 0;
            for (size_t i = startIdx; i < buffer.size(); i++) {
                result.push_back(buffer[i]);
            }
        }
        return result;
    }

    /**
     * Playback data at specified speed
     */
    class PlaybackController {
    public:
        PlaybackController(TimeSeriesDB* db) : database(db), playing(false), speed(1.0), lastQueryTime(0.0) {}

        void start(double startTime, double endTime, double playbackSpeed = 1.0) {
            playing = true;
            speed = playbackSpeed;
            playbackStart = startTime;
            playbackEnd = endTime;
            currentTime = startTime;
            lastQueryTime = startTime;
            realStartTime = std::chrono::high_resolution_clock::now();
        }

        void stop() { playing = false; }

        bool isPlaying() const { return playing; }

        // Returns next point(s) since last call
        std::vector<TimeSeriesPoint> update() {
            if (!playing) return {};

            auto now = std::chrono::high_resolution_clock::now();
            double elapsed = std::chrono::duration<double>(now - realStartTime).count();
            currentTime = playbackStart + elapsed * speed;

            if (currentTime >= playbackEnd) {
                playing = false;
                return {};
            }

            auto points = database->queryByTimeRange(lastQueryTime, currentTime);
            lastQueryTime = currentTime;
            return points;
        }

    private:
        TimeSeriesDB* database;
        bool playing;
        double speed;
        double playbackStart, playbackEnd;
        double currentTime;
        double lastQueryTime;
        std::chrono::time_point<std::chrono::high_resolution_clock> realStartTime;
    };

    PlaybackController createPlayback() { return PlaybackController(this); }

    /**
     * Clear all data
     */
    void clear() {
        std::lock_guard<std::mutex> lock(dataMutex);
        memoryBuffer.clear();
        entityBuffers.clear();
        if (influxClient.isConnected()) {
            influxClient.dropDatabase();
            influxClient.createDatabase();
        }
    }

    size_t getTotalPoints() const {
        std::lock_guard<std::mutex> lock(dataMutex);
        return memoryBuffer.size();
    }

    bool isInfluxDBConnected() const {
        return influxClient.isConnected();
    }

private:
    InfluxDBClient influxClient;
    std::string influxHost;
    int influxPort;
    std::string databaseName;
    std::deque<TimeSeriesPoint> memoryBuffer;
    std::map<std::string, std::deque<TimeSeriesPoint>> entityBuffers;
    mutable std::mutex dataMutex;
    size_t maxMemoryPoints;

    void persistPoint(const TimeSeriesPoint& point) {
        InfluxPoint ip;
        ip.measurement = "points";
        ip.tags["entityId"] = point.entityId;
        ip.fields["latitude"] = point.latitude;
        ip.fields["longitude"] = point.longitude;
        ip.fields["altitude"] = point.altitude;
        ip.fields["speed"] = point.speed;
        ip.fields["heading"] = point.heading;
        ip.fields["accuracy"] = point.accuracy;
        ip.timestamp = static_cast<uint64_t>(point.timestamp * 1e9);

        influxClient.writePoint(ip);
    }

    void persistBatch(const std::vector<TimeSeriesPoint>& points) {
        std::vector<InfluxPoint> influxPoints;
        influxPoints.reserve(points.size());

        for (const auto& point : points) {
            InfluxPoint ip;
            ip.measurement = "points";
            ip.tags["entityId"] = point.entityId;
            ip.fields["latitude"] = point.latitude;
            ip.fields["longitude"] = point.longitude;
            ip.fields["altitude"] = point.altitude;
            ip.fields["speed"] = point.speed;
            ip.fields["heading"] = point.heading;
            ip.fields["accuracy"] = point.accuracy;
            ip.timestamp = static_cast<uint64_t>(point.timestamp * 1e9);
            influxPoints.push_back(ip);
        }

        influxClient.writePoints(influxPoints);
    }

    std::vector<TimeSeriesPoint> queryInfluxDB(double startTime, double endTime,
                                                const std::string& entityId) {
        uint64_t startNs = static_cast<uint64_t>(startTime * 1e9);
        uint64_t endNs = static_cast<uint64_t>(endTime * 1e9);

        auto rows = influxClient.queryByTimeRange("points", entityId, startNs, endNs);
        return rowsToPoints(rows);
    }

    std::vector<TimeSeriesPoint> rowsToPoints(const std::vector<InfluxRow>& rows) {
        std::vector<TimeSeriesPoint> result;
        result.reserve(rows.size());

        for (const auto& row : rows) {
            TimeSeriesPoint point;
            point.timestamp = row.time / 1e9;

            auto getDbl = [&](const std::string& key, double def) {
                auto it = row.values.find(key);
                if (it != row.values.end()) {
                    try { return std::stod(it->second); } catch (...) { return def; }
                }
                return def;
            };

            auto getStr = [&](const std::string& key, const std::string& def) {
                auto it = row.values.find(key);
                return (it != row.values.end()) ? it->second : def;
            };

            point.latitude = getDbl("latitude", 0.0);
            point.longitude = getDbl("longitude", 0.0);
            point.altitude = getDbl("altitude", 0.0);
            point.speed = getDbl("speed", 0.0);
            point.heading = getDbl("heading", 0.0);
            point.accuracy = getDbl("accuracy", 0.0);
            point.entityId = getStr("entityId", "");

            result.push_back(point);
        }

        return result;
    }
};
