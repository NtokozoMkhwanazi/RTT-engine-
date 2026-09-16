#pragma once

/**
 * GeoStorageSystem - Handles time-series database storage (Phase 2)
 *
 * Dedicated system for data persistence with multithreading:
 * - In-memory circular buffer for recent data
 * - InfluxDB persistent storage
 * - Time-range queries and trajectory retrieval
 * - Playback controller for historical data
 * - Batch writing on separate thread to avoid blocking
 */

#include "../ECS.h"
#include "../../geospatial/TimeSeriesDB.h"
#include "../../geospatial/GeospatialConverter.h"
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <iostream>

namespace ecs {

class GeoStorageSystem : public System {
public:
    GeoStorageSystem() : running(false), batchWriteIntervalMs(100) {}

    ~GeoStorageSystem() {
        stopStorageThread();
    }

    /**
     * Initialize with InfluxDB connection
     */
    void initialize(const std::string& influxHost = "http://localhost",
                    int influxPort = 8086,
                    const std::string& database = "geospatial") {
        timeSeriesDB.initialize(influxHost, influxPort, database);
        startStorageThread();
        std::cout << "[GeoStorageSystem] Initialized\n";
    }

    /**
     * Store a single data point (thread-safe, batches writes)
     */
    void store(const TimeSeriesPoint& point) {
        std::lock_guard<std::mutex> lock(writeQueueMutex);
        writeQueue.push(point);
        cv.notify_one();
    }

    /**
     * Store multiple data points (batch)
     */
    void storeBatch(const std::vector<TimeSeriesPoint>& points) {
        std::lock_guard<std::mutex> lock(writeQueueMutex);
        for (const auto& point : points) {
            writeQueue.push(point);
        }
        cv.notify_one();
    }

    /**
     * Query points by time range
     */
    std::vector<TimeSeriesPoint> queryByTimeRange(double startTime, double endTime,
                                                   const std::string& entityId = "") {
        return timeSeriesDB.queryByTimeRange(startTime, endTime, entityId);
    }

    /**
     * Get trajectory for an entity
     */
    std::vector<TimeSeriesPoint> getTrajectory(const std::string& entityId,
                                               size_t maxPoints = 100) {
        return timeSeriesDB.getTrajectory(entityId, maxPoints);
    }

    /**
     * Create a playback controller for historical data
     */
    TimeSeriesDB::PlaybackController createPlayback() {
        return timeSeriesDB.createPlayback();
    }

    /**
     * Get total points stored
     */
    size_t getTotalPoints() const {
        return timeSeriesDB.getTotalPoints();
    }

    /**
     * Check if InfluxDB is connected
     */
    bool isInfluxDBConnected() const {
        return timeSeriesDB.isInfluxDBConnected();
    }

    /**
     * Clear all stored data
     */
    void clear() {
        timeSeriesDB.clear();
    }

    /**
     * Main update - processes write queue
     */
    void update(float dt) override {
        // Drain any remaining items in write queue
        processWriteQueue();
    }

    TimeSeriesDB& getTimeSeriesDB() { return timeSeriesDB; }
    const TimeSeriesDB& getTimeSeriesDB() const { return timeSeriesDB; }

    const char* getName() const override { return "GeoStorageSystem"; }

private:
    TimeSeriesDB timeSeriesDB;
    std::queue<TimeSeriesPoint> writeQueue;
    std::mutex writeQueueMutex;
    std::condition_variable cv;
    std::thread storageThread;
    std::atomic<bool> running;
    int batchWriteIntervalMs;

    void startStorageThread() {
        if (running) return;
        running = true;
        storageThread = std::thread([this]() { storageLoop(); });
        std::cout << "[GeoStorageSystem] Started storage thread\n";
    }

    void stopStorageThread() {
        running = false;
        cv.notify_all();
        if (storageThread.joinable()) {
            storageThread.join();
        }
    }

    void storageLoop() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(batchWriteIntervalMs));
            processWriteQueue();
        }
    }

    void processWriteQueue() {
        std::vector<TimeSeriesPoint> batch;
        {
            std::lock_guard<std::mutex> lock(writeQueueMutex);
            while (!writeQueue.empty()) {
                batch.push_back(writeQueue.front());
                writeQueue.pop();
            }
        }

        if (!batch.empty()) {
            timeSeriesDB.recordBatch(batch);
        }
    }
};

} // namespace ecs
