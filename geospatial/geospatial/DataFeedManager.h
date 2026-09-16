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
#include <deque>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <type_traits>
#include <cstdlib>
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

    // Public description of a registered feed (for UI listing).
    struct FeedInfo {
        std::string url;
        std::string type;   // "REST" or "WebSocket"
        bool active = false;
    };

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
        auto feed = std::make_unique<RESTFeed>();
        feed->url = url;
        feed->intervalMs = intervalMs;
        feed->callback = std::move(callback);
        feed->parser = std::move(parser);
        feed->active = true;
        RESTFeed* raw = feed.get();
        restFeeds.push_back(std::move(feed));
        // If feeds are already running, start this one immediately. Each feed
        // lives on its own heap allocation (unique_ptr), so the worker
        // thread's &feed capture stays valid regardless of container growth
        // or the removal of other feeds.
        if (running) startFeedThread(*raw);
    }

    /**
     * Add a WebSocket feed (simulated with polling for simplicity)
     */
    void addWebSocketFeed(const std::string& url, DataCallback callback) {
        std::lock_guard<std::mutex> lock(feedsMutex);
        auto feed = std::make_unique<WSFeed>();
        feed->url = url;
        feed->callback = std::move(callback);
        feed->active = true;
        WSFeed* raw = feed.get();
        wsFeeds.push_back(std::move(feed));
        if (running) startFeedThread(*raw);
    }

    /** Number of registered feeds (REST + WebSocket). */
    size_t getFeedCount() const {
        std::lock_guard<std::mutex> lock(feedsMutex);
        return restFeeds.size() + wsFeeds.size();
    }

    /** Description of the feed at the given index (REST feeds first). */
    FeedInfo getFeedInfo(size_t index) const {
        std::lock_guard<std::mutex> lock(feedsMutex);
        if (index < restFeeds.size()) {
            const auto& f = *restFeeds[index];
            return {f.url, "REST", f.active};
        }
        const size_t wsIdx = index - restFeeds.size();
        if (wsIdx < wsFeeds.size()) {
            const auto& f = *wsFeeds[wsIdx];
            return {f.url, "WebSocket", f.active};
        }
        return {};
    }

    /**
     * Stop and remove the feed at the given index (REST feeds first).
     * No-op when the index is out of range.
     *
     * The worker thread is signalled to stop and JOINED WITHOUT holding
     * feedsMutex, so an in-flight poll (bounded by CURLOPT_TIMEOUT) never
     * blocks other feed operations or the UI thread.
     */
    void removeFeed(size_t index) {
        // Locate the target, signal stop (active=false + wake) under the lock.
        bool* activeFlag = nullptr;
        std::thread* joinTarget = nullptr;
        {
            std::lock_guard<std::mutex> lock(feedsMutex);
            if (index < restFeeds.size()) {
                auto& f = *restFeeds[index];
                activeFlag = &f.active;
                joinTarget = &f.thread;
            } else {
                const size_t wsIdx = index - restFeeds.size();
                if (wsIdx < wsFeeds.size()) {
                    auto& f = *wsFeeds[wsIdx];
                    activeFlag = &f.active;
                    joinTarget = &f.thread;
                }
            }
            if (!activeFlag) return;
            *activeFlag = false;      // worker loop exits after its wake
            sleepCv.notify_all();
        }

        // Join outside the lock: an in-flight poll (bounded by the 10 s curl
        // timeout) must not block other feed operations or the UI thread.
        if (joinTarget->joinable()) joinTarget->join();

        // Erase by identity (indices may have shifted while unlocked).
        std::lock_guard<std::mutex> lock(feedsMutex);
        if (index < restFeeds.size() && joinTarget == &restFeeds[index]->thread) {
            restFeeds.erase(restFeeds.begin() + static_cast<ptrdiff_t>(index));
            return;
        }
        const size_t wsIdx = index - restFeeds.size();
        if (wsIdx < wsFeeds.size() && joinTarget == &wsFeeds[wsIdx]->thread) {
            wsFeeds.erase(wsFeeds.begin() + static_cast<ptrdiff_t>(wsIdx));
        }
    }

    /**
     * Parse NMEA 0183 sentence ($GPRMC, $GPGGA, etc.)
     */
    static NMEAData parseNMEASentence(const std::string& sentence) {
        NMEAData result;
        if (sentence.empty() || sentence[0] != '$') return result;

        // $GPRMC parsing: $GPRMC,time,status,lat,N/S,lon,E/W,speed,track,date,...
        // field indices: 0=GPRMC 1=time 2=status 3=lat 4=N/S 5=lon 6=E/W
        //                7=speed(knots) 8=track 9=date 10=magvar 11=E/W 12=cs
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

            if (fields.size() >= 7 && fields[2] == "A") { // Active
                result.valid = true;
                // Parse latitude (fields[3] + fields[4] for N/S)
                if (fields.size() > 4) {
                    const double lat = safeToDouble(fields[3]);
                    result.lat = parseNMEACoord(lat);
                    if (fields[4] == "S") result.lat = -result.lat;
                }
                // Parse longitude (fields[5] + fields[6] for E/W)
                if (fields.size() > 6) {
                    const double lon = safeToDouble(fields[5]);
                    result.lon = parseNMEACoord(lon);
                    if (fields[6] == "W") result.lon = -result.lon;
                }
                if (fields.size() > 7) result.speed = safeToDouble(fields[7]) * 0.514444; // knots to m/s
                if (fields.size() > 8) result.track = safeToDouble(fields[8]);
            }
        }
        return result;
    }

    /**
     * Start all feeds (idempotent).
     */
    void startAllFeeds() {
        std::lock_guard<std::mutex> lock(feedsMutex);
        if (running) return;
        running = true;
        for (auto& feed : restFeeds) startFeedThread(*feed);
        for (auto& feed : wsFeeds) startFeedThread(*feed);
    }

    /**
     * Stop all feeds
     */
    void stopAllFeeds() {
        running = false;
        sleepCv.notify_all();
        for (auto& feed : restFeeds) {
            if (feed->thread.joinable()) feed->thread.join();
        }
        for (auto& feed : wsFeeds) {
            if (feed->thread.joinable()) feed->thread.join();
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

    // Feeds are heap-allocated (unique_ptr) so each worker thread's
    // [this, &feed] capture references a stable object: deque growth does not
    // move it, and erasing OTHER feeds (from any index) does not invalidate
    // it. Only the erased feed's own thread is joined before its object dies.
    std::deque<std::unique_ptr<RESTFeed>> restFeeds;
    std::deque<std::unique_ptr<WSFeed>> wsFeeds;
    std::queue<GeoDataPoint> dataQueue;
    mutable std::mutex feedsMutex;
    std::mutex queueMutex;
    std::atomic<bool> running;

    // Wake-up for feed worker threads: removal/shutdown interrupts their poll
    // sleep so a join never blocks for a full poll interval. Dedicated mutex
    // (never the feedsMutex) so a joining thread never deadlocks on it.
    std::mutex sleepMutex;
    std::condition_variable sleepCv;

    // Start the worker thread for a single feed (call with feedsMutex held).
    // Templated so it works for both RESTFeed and WSFeed.
    template <typename Feed>
    void startFeedThread(Feed& feed) {
        if (feed.thread.joinable()) return;
        feed.active = true;
        if constexpr (std::is_same_v<Feed, RESTFeed>) {
            feed.thread = std::thread([this, &feed]() { restFeedLoop(feed); });
        } else {
            feed.thread = std::thread([this, &feed]() { wsFeedLoop(feed); });
        }
    }

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
            // Interruptible sleep: removal/shutdown wakes the thread so the
            // join never blocks for a full poll interval.
            std::unique_lock<std::mutex> lk(sleepMutex);
            sleepCv.wait_for(lk, std::chrono::milliseconds(feed.intervalMs),
                             [this, &feed] { return !running || !feed.active; });
        }
    }

    void wsFeedLoop(WSFeed& feed) {
        // Simplified - in production use libwebsockets or similar
        while (running && feed.active) {
            std::unique_lock<std::mutex> lk(sleepMutex);
            sleepCv.wait_for(lk, std::chrono::milliseconds(100),
                             [this, &feed] { return !running || !feed.active; });
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

    // strtod-based parse that never throws on malformed input.
    static double safeToDouble(const std::string& s) {
        if (s.empty()) return 0.0;
        return std::strtod(s.c_str(), nullptr);
    }
};
