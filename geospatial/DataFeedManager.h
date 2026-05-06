#pragma once

/**
 * Data Feed Manager - Real-Time Data Ingestion (Phase 1)
 *
 * Handles live data feeds from various sources:
 * - REST API polling (weather, traffic, IoT)
 * - WebSocket streams (real-time GPS, sensors)
 * - NMEA 0183 serial GPS devices
 * - MQTT/CoAP for IoT sensors
 * - File-based playback (CSV, JSON)
 */

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <curl/curl.h>
#include <iostream>

// Data point structure for all feed types
struct GeoDataPoint {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double timestamp = 0.0;
    double speed = 0.0;
    double heading = 0.0;
    double accuracy = 0.0;
    std::string sourceId;
    std::string dataType; // "gps", "weather", "sensor", etc.
    std::string rawJson;  // Original data for debugging
};

// NMEA parsing result
struct NMEAData {
    bool valid = false;
    double lat = 0.0;
    double lon = 0.0;
    double speed = 0.0;  // knots
    double track = 0.0;  // degrees
    std::string timeStr;
};

class DataFeedManager {
public:
    DataFeedManager() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        running = false;
    }

    ~DataFeedManager() {
        stopAllFeeds();
        curl_global_cleanup();
    }

    // Callback type for receiving data points
    using DataCallback = std::function<void(const GeoDataPoint&)>;

    /**
     * Add a REST API feed (polling)
     * @param url The REST endpoint URL
     * @param intervalMs Poll interval in milliseconds
     * @param parser Custom parser function (returns GeoDataPoint from JSON)
     * @param callback Called with each new data point
     */
    void addRESTFeed(const std::string& url,
                     int intervalMs,
                     DataCallback callback,
                     std::function<GeoDataPoint(const std::string&)> parser = defaultJSONParser) {
        std::lock_guard<std::mutex> lock(feedsMutex);
        restFeeds.push_back({url, intervalMs, callback, parser, true});
    }

    /**
     * Add a WebSocket feed (simulated with polling for simplicity)
     */
    void addWebSocketFeed(const std::string& url, DataCallback callback) {
        std::lock_guard<std::mutex> lock(feedsMutex);
        wsFeeds.push_back({url, callback, true});
    }

    /**
     * Parse NMEA 0183 sentence ($GPRMC, $GPGGA, etc.)
     */
    static NMEAData parseNMEASentence(const std::string& sentence) {
        NMEAData result;
        if (sentence.empty() || sentence[0] != '$') return result;

        // Simple $GPRMC parsing: $GPRMC,time,status,lat,N/S,lon,E/W,speed,track,date,...
        if (sentence.find("$GPRMC") != std::string::npos) {
            std::vector<std::string> fields;
            size_t start = 1;
            for (size_t i = 1; i < sentence.length(); i++) {
                if (sentence[i] == ',') {
                    fields.push_back(sentence.substr(start, i - start));
                    start = i + 1;
                }
            }
            fields.push_back(sentence.substr(start));

            if (fields.size() >= 8 && fields[1] == "A") { // Active
                result.valid = true;
                // Parse latitude (fields[2] + fields[3] for N/S)
                if (fields.size() > 3) {
                    double lat = std::stod(fields[2]);
                    result.lat = parseNMEACoord(lat);
                    if (fields[3] == "S") result.lat = -result.lat;
                }
                // Parse longitude (fields[4] + fields[5] for E/W)
                if (fields.size() > 5) {
                    double lon = std::stod(fields[4]);
                    result.lon = parseNMEACoord(lon);
                    if (fields[5] == "W") result.lon = -result.lon;
                }
                if (fields.size() > 6) result.speed = std::stod(fields[6]) * 0.514444; // knots to m/s
                if (fields.size() > 7) result.track = std::stod(fields[7]);
            }
        }
        return result;
    }

    /**
     * Start all feeds
     */
    void startAllFeeds() {
        running = true;
        // Start REST feed threads
        for (auto& feed : restFeeds) {
            feed.thread = std::thread([this, &feed]() { restFeedLoop(feed); });
        }
        // Start WebSocket feed threads
        for (auto& feed : wsFeeds) {
            feed.thread = std::thread([this, &feed]() { wsFeedLoop(feed); });
        }
    }

    /**
     * Stop all feeds
     */
    void stopAllFeeds() {
        running = false;
        for (auto& feed : restFeeds) {
            if (feed.thread.joinable()) feed.thread.join();
        }
        for (auto& feed : wsFeeds) {
            if (feed.thread.joinable()) feed.thread.join();
        }
    }

    /**
     * Push a data point to the processing queue
     */
    void pushDataPoint(const GeoDataPoint& point) {
        std::lock_guard<std::mutex> lock(queueMutex);
        dataQueue.push(point);
    }

    /**
     * Process all queued data points
     */
    std::vector<GeoDataPoint> processQueue() {
        std::lock_guard<std::mutex> lock(queueMutex);
        std::vector<GeoDataPoint> result;
        while (!dataQueue.empty()) {
            result.push_back(dataQueue.front());
            dataQueue.pop();
        }
        return result;
    }

private:
    struct RESTFeed {
        std::string url;
        int intervalMs;
        DataCallback callback;
        std::function<GeoDataPoint(const std::string&)> parser;
        bool active;
        std::thread thread;
    };

    struct WSFeed {
        std::string url;
        DataCallback callback;
        bool active;
        std::thread thread;
    };

    std::vector<RESTFeed> restFeeds;
    std::vector<WSFeed> wsFeeds;
    std::queue<GeoDataPoint> dataQueue;
    std::mutex feedsMutex;
    std::mutex queueMutex;
    std::atomic<bool> running;

    // cURL write callback
    static size_t curlWriteCallback(char* ptr, size_t size, size_t nmemb, std::string* data) {
        data->append(ptr, size * nmemb);
        return size * nmemb;
    }

    void restFeedLoop(RESTFeed& feed) {
        while (running && feed.active) {
            CURL* curl = curl_easy_init();
            if (curl) {
                std::string response;
                curl_easy_setopt(curl, CURLOPT_URL, feed.url.c_str());
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

                CURLcode res = curl_easy_perform(curl);
                if (res == CURLE_OK) {
                    GeoDataPoint point = feed.parser(response);
                    if (point.timestamp == 0.0) {
                        point.timestamp = getCurrentTimestamp();
                    }
                    feed.callback(point);
                }
                curl_easy_cleanup(curl);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(feed.intervalMs));
        }
    }

    void wsFeedLoop(WSFeed& feed) {
        // Simplified - in production use libwebsockets or similar
        while (running && feed.active) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    static double getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        return std::chrono::duration<double>(now.time_since_epoch()).count();
    }

    static GeoDataPoint defaultJSONParser(const std::string& json) {
        GeoDataPoint point;
        // Simple JSON parsing - in production use jsoncpp
        // This is a placeholder
        point.timestamp = getCurrentTimestamp();
        return point;
    }

    static double parseNMEACoord(double coord) {
        // NMEA format: DDMM.mmmm -> decimal degrees
        int degrees = static_cast<int>(coord / 100);
        double minutes = coord - (degrees * 100);
        return degrees + minutes / 60.0;
    }
};
