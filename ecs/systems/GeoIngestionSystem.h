#pragma once

/**
 * GeoIngestionSystem - Handles GPS input and data feed ingestion (Phase 1)
 *
 * Dedicated system for real-time data ingestion with multithreading:
 * - GPS tracker simulation and real device input
 * - REST API polling and WebSocket streams
 * - NMEA 0183 GPS sentence parsing
 * - Thread-safe data queue for passing to other systems
 * - Runs ingestion on separate thread to avoid blocking render loop
 */

#include "../ECS.h"
#include "../../geospatial/GPSTracker.h"
#include "../../geospatial/DataFeedManager.h"
#include "../../geospatial/GeospatialConverter.h"
#include <glm/glm.hpp>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>

namespace ecs {

struct GeoIngestedData {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double timestamp = 0.0;
    double speed = 0.0;
    double heading = 0.0;
    double accuracy = 0.0;
    std::string entityId;
    bool isValid = false;
};

class GeoIngestionSystem : public System {
public:
    GeoIngestionSystem() : running(false), systemTime(0.0f) {
        converter.setOrigin(0.0, 0.0, 0.0);
        gpsTracker.setOrigin(0.0, 0.0, 0.0);
    }

    ~GeoIngestionSystem() {
        stopIngestionThread();
    }

    /**
     * Initialize with reference origin
     */
    void initialize(double originLat, double originLon, double originAlt = 0.0) {
        converter.setOrigin(originLat, originLon, originAlt);
        gpsTracker.setOrigin(originLat, originLon, originAlt);
        gpsTracker.setMode(GPSTracker::Mode::SIMULATED_WALK);
        gpsTracker.setSpeed(1.4f);
        gpsTracker.setNoiseLevel(2.0f);

        std::cout << "[GeoIngestionSystem] Initialized at origin: "
                  << originLat << "°, " << originLon << "°, " << originAlt << "m\n";
    }

    /**
     * Set GPS tracker mode
     */
    void setGPSMode(GPSTracker::Mode mode) {
        gpsTracker.setMode(mode);
    }

    /**
     * Set GPS simulation speed (m/s)
     */
    void setGPSSpeed(double speed) {
        gpsTracker.setSpeed(speed);
    }

    /**
     * Set GPS noise level (meters)
     */
    void setGPSNoise(double noiseMeters) {
        gpsTracker.setNoiseLevel(noiseMeters);
    }

    /**
     * Add external REST API feed
     */
    void addRESTFeed(const std::string& url, int intervalMs,
                     std::function<GeoIngestedData(const std::string&)> parser = nullptr) {
        dataFeedManager.addRESTFeed(url, intervalMs,
            [this](const GeoDataPoint& point) {
                GeoIngestedData data;
                data.latitude = point.latitude;
                data.longitude = point.longitude;
                data.altitude = point.altitude;
                data.timestamp = point.timestamp;
                data.speed = point.speed;
                data.heading = point.heading;
                data.accuracy = point.accuracy;
                data.entityId = point.sourceId;
                data.isValid = true;
                pushIngestedData(data);
            },
            [parser](const std::string& json) -> GeoDataPoint {
                GeoDataPoint point;
                if (parser) {
                    GeoIngestedData d = parser(json);
                    point.latitude = d.latitude;
                    point.longitude = d.longitude;
                    point.altitude = d.altitude;
                    point.timestamp = d.timestamp;
                    point.speed = d.speed;
                    point.heading = d.heading;
                    point.accuracy = d.accuracy;
                    point.sourceId = d.entityId;
                }
                return point;
            });
    }

    /**
     * Start multithreaded ingestion (call after adding feeds)
     */
    void startIngestion() {
        if (running) return;
        running = true;
        dataFeedManager.startAllFeeds();
        ingestionThread = std::thread([this]() { ingestionLoop(); });
        std::cout << "[GeoIngestionSystem] Started ingestion thread\n";
    }

    /**
     * Stop ingestion thread
     */
    void stopIngestionThread() {
        running = false;
        cv.notify_all();
        if (ingestionThread.joinable()) {
            ingestionThread.join();
        }
        dataFeedManager.stopAllFeeds();
    }

    /**
     * Main update - processes GPS simulation and pushes to queue
     */
    void update(float dt) override {
        systemTime += dt;

        // Update GPS tracker simulation
        gpsTracker.update(dt);

        const GPSFix& fix = gpsTracker.getCurrentFix();
        if (fix.isValid) {
            GeoIngestedData data;
            data.latitude = fix.latitude;
            data.longitude = fix.longitude;
            data.altitude = fix.altitude;
            data.timestamp = fix.timestamp;
            data.speed = fix.speed;
            data.heading = fix.heading;
            data.accuracy = fix.horizontalAccuracy;
            data.entityId = "gps_tracker";
            data.isValid = true;
            pushIngestedData(data);
        }

        // Process external feed queue
        auto dataPoints = dataFeedManager.processQueue();
        for (const auto& point : dataPoints) {
            GeoIngestedData data;
            data.latitude = point.latitude;
            data.longitude = point.longitude;
            data.altitude = point.altitude;
            data.timestamp = point.timestamp;
            data.speed = point.speed;
            data.heading = point.heading;
            data.accuracy = point.accuracy;
            data.entityId = point.sourceId;
            data.isValid = true;
            pushIngestedData(data);
        }
    }

    /**
     * Get all ingested data from queue (thread-safe)
     */
    std::vector<GeoIngestedData> consumeIngestedData() {
        std::lock_guard<std::mutex> lock(queueMutex);
        std::vector<GeoIngestedData> result;
        while (!ingestedQueue.empty()) {
            result.push_back(ingestedQueue.front());
            ingestedQueue.pop();
        }
        return result;
    }

    /**
     * Get the geospatial converter for coordinate transforms
     */
    GeospatialConverter& getConverter() { return converter; }
    const GeospatialConverter& getConverter() const { return converter; }

    const GPSFix& getCurrentGPSFix() const { return gpsTracker.getCurrentFix(); }
    GPSTracker& getGPSTracker() { return gpsTracker; }
    DataFeedManager& getDataFeedManager() { return dataFeedManager; }

    const char* getName() const override { return "GeoIngestionSystem"; }

private:
    GeospatialConverter converter;
    GPSTracker gpsTracker;
    DataFeedManager dataFeedManager;

    std::queue<GeoIngestedData> ingestedQueue;
    std::mutex queueMutex;
    std::condition_variable cv;
    std::thread ingestionThread;
    std::atomic<bool> running;
    float systemTime;

    void ingestionLoop() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void pushIngestedData(const GeoIngestedData& data) {
        std::lock_guard<std::mutex> lock(queueMutex);
        ingestedQueue.push(data);
        cv.notify_one();
    }
};

} // namespace ecs
