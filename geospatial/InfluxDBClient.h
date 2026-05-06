#pragma once

/**
 * Lightweight InfluxDB Client (v1 HTTP API) using libcurl
 *
 * Avoids the C++20 requirement of influxdb-cxx by implementing
 * the v1 HTTP write/query API directly with libcurl.
 *
 * InfluxDB v1 write: POST /write?db=<db>
 *   Body: measurement,tag1=val1 field1=val1 timestamp
 *
 * InfluxDB v1 query: GET /query?db=<db>&q=<query>
 *   Response: JSON with series/results
 */

#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <mutex>
#include <curl/curl.h>
#include <iostream>

struct InfluxPoint {
    std::string measurement;
    std::map<std::string, std::string> tags;
    std::map<std::string, double> fields;
    uint64_t timestamp = 0; // nanoseconds since epoch (0 = use server time)
};

struct InfluxRow {
    uint64_t time;
    std::map<std::string, std::string> values;
};

class InfluxDBClient {
public:
    InfluxDBClient() : initialized(false) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    ~InfluxDBClient() {
        curl_global_cleanup();
    }

    /**
     * Connect to InfluxDB server
     */
    bool connect(const std::string& host, int port, const std::string& database) {
        this->host = host;
        this->port = port;
        this->database = database;
        this->url = host + ":" + std::to_string(port);
        this->initialized = true;
        std::cout << "[InfluxDBClient] Connected to " << url << "/" << database << "\n";
        return true;
    }

    /**
     * Write a single point
     */
    bool writePoint(const InfluxPoint& point) {
        std::vector<InfluxPoint> points = {point};
        return writePoints(points);
    }

    /**
     * Write multiple points in batch
     */
    bool writePoints(const std::vector<InfluxPoint>& points) {
        if (!initialized) {
            std::cerr << "[InfluxDBClient] Not initialized\n";
            return false;
        }

        std::string body = buildLineProtocol(points);
        if (body.empty()) return false;

        std::string writeUrl = url + "/write?db=" + database;
        std::string response;

        CURL* curl = curl_easy_init();
        if (!curl) return false;

        curl_easy_setopt(curl, CURLOPT_URL, writeUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            std::cerr << "[InfluxDBClient] Write failed: " << curl_easy_strerror(res) << "\n";
            return false;
        }

        return true;
    }

    /**
     * Query InfluxDB
     */
    std::vector<InfluxRow> query(const std::string& queryString) {
        if (!initialized) {
            std::cerr << "[InfluxDBClient] Not initialized\n";
            return {};
        }

        std::string encodedQuery;
        char* escaped = curl_escape(queryString.c_str(), queryString.size());
        if (!escaped) return {};
        encodedQuery = escaped;
        curl_free(escaped);
        std::string queryUrl = url + "/query?db=" + database + "&q=" + encodedQuery;

        std::string response;
        CURL* curl = curl_easy_init();
        if (!curl) return {};

        curl_easy_setopt(curl, CURLOPT_URL, queryUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            std::cerr << "[InfluxDBClient] Query failed: " << curl_easy_strerror(res) << "\n";
            return {};
        }

        return parseJSONResponse(response);
    }

    /**
     * Query by time range for a specific measurement and entity
     */
    std::vector<InfluxRow> queryByTimeRange(const std::string& measurement,
                                             const std::string& entityId,
                                             uint64_t startNs, uint64_t endNs) {
        std::string q = "SELECT * FROM " + measurement +
                        " WHERE entityId = '" + entityId +
                        "' AND time >= " + std::to_string(startNs) +
                        " AND time <= " + std::to_string(endNs) +
                        " ORDER BY time";
        return query(q);
    }

    /**
     * Get all points for an entity (recent first, limited)
     */
    std::vector<InfluxRow> getTrajectory(const std::string& measurement,
                                          const std::string& entityId,
                                          int limit = 1000) {
        std::string q = "SELECT * FROM " + measurement +
                        " WHERE entityId = '" + entityId +
                        "' ORDER BY time DESC LIMIT " + std::to_string(limit);
        return query(q);
    }

    /**
     * Drop the database (reset)
     */
    bool dropDatabase() {
        std::string q = "DROP DATABASE " + database;
        std::string response;
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        char* escaped = curl_escape(q.c_str(), q.size());
        std::string queryUrl = url + "/query?q=" + std::string(escaped);
        curl_free(escaped);

        curl_easy_setopt(curl, CURLOPT_URL, queryUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        return res == CURLE_OK;
    }

    /**
     * Create database if not exists
     */
    bool createDatabase() {
        std::string q = "CREATE DATABASE " + database;
        std::string response;
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        char* escaped = curl_escape(q.c_str(), q.size());
        std::string queryUrl = url + "/query?q=" + std::string(escaped);
        curl_free(escaped);

        curl_easy_setopt(curl, CURLOPT_URL, queryUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        return res == CURLE_OK;
    }

    bool isConnected() const { return initialized; }

private:
    bool initialized = false;
    std::string host;
    int port;
    std::string database;
    std::string url;
    std::mutex writeMutex;

    static size_t writeCallback(char* ptr, size_t size, size_t nmemb, std::string* data) {
        data->append(ptr, size * nmemb);
        return size * nmemb;
    }

    std::string buildLineProtocol(const std::vector<InfluxPoint>& points) {
        std::lock_guard<std::mutex> lock(writeMutex);
        std::stringstream ss;

        for (const auto& point : points) {
            // measurement,tags fields timestamp
            ss << point.measurement;

            if (!point.tags.empty()) {
                for (auto it = point.tags.begin(); it != point.tags.end(); ++it) {
                    ss << (it == point.tags.begin() ? "," : ",")
                       << it->first << "=" << it->second;
                }
            }

            ss << " ";
            bool firstField = true;
            for (const auto& field : point.fields) {
                if (!firstField) ss << ",";
                ss << field.first << "=" << field.second;
                firstField = false;
            }

            if (point.timestamp > 0) {
                ss << " " << point.timestamp;
            }

            ss << "\n";
        }

        return ss.str();
    }

    std::vector<InfluxRow> parseJSONResponse(const std::string& json) {
        // Minimal JSON parsing for InfluxDB response format
        // Response: {"results":[{"series":[{"name":"points","columns":["time","lat",...],"values":[[...]]}]}]}
        std::vector<InfluxRow> result;

        // Find the values array
        size_t valuesPos = json.find("\"values\"");
        if (valuesPos == std::string::npos) return result;

        size_t arrStart = json.find('[', valuesPos);
        if (arrStart == std::string::npos) return result;

        // Find columns first
        size_t columnsPos = json.find("\"columns\"");
        std::vector<std::string> columns;
        if (columnsPos != std::string::npos) {
            size_t colArrStart = json.find('[', columnsPos);
            size_t colArrEnd = json.find(']', colArrStart);
            if (colArrStart != std::string::npos && colArrEnd != std::string::npos) {
                std::string colStr = json.substr(colArrStart + 1, colArrEnd - colArrStart - 1);
                size_t pos = 0;
                while ((pos = colStr.find('"', pos)) != std::string::npos) {
                    size_t end = colStr.find('"', pos + 1);
                    if (end != std::string::npos) {
                        columns.push_back(colStr.substr(pos + 1, end - pos - 1));
                        pos = end + 1;
                    } else break;
                }
            }
        }

        // Parse each row
        size_t pos = arrStart + 1;
        int depth = 1;
        std::string currentRow;

        while (pos < json.size() && depth > 0) {
            char c = json[pos];
            if (c == '[') {
                if (depth == 1) {
                    currentRow.clear();
                }
                depth++;
            } else if (c == ']') {
                depth--;
                if (depth == 1 && !currentRow.empty()) {
                    InfluxRow row;
                    parseRowValues(currentRow, columns, row);
                    result.push_back(row);
                }
            } else if (depth == 2) {
                currentRow += c;
            }
            pos++;
        }

        return result;
    }

    void parseRowValues(const std::string& rowStr, const std::vector<std::string>& columns, InfluxRow& row) {
        std::vector<std::string> values;
        size_t pos = 0;
        bool inQuotes = false;

        for (size_t i = 0; i < rowStr.size(); i++) {
            char c = rowStr[i];
            if (c == '"') {
                inQuotes = !inQuotes;
            } else if (c == ',' && !inQuotes) {
                std::string val = rowStr.substr(pos, i - pos);
                // Trim quotes
                if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                    val = val.substr(1, val.size() - 2);
                }
                values.push_back(val);
                pos = i + 1;
            }
        }
        // Last value
        std::string val = rowStr.substr(pos);
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        if (!val.empty()) values.push_back(val);

        for (size_t i = 0; i < columns.size() && i < values.size(); i++) {
            if (columns[i] == "time") {
                try { row.time = std::stoull(values[i]); } catch (...) { row.time = 0; }
            } else {
                row.values[columns[i]] = values[i];
            }
        }
    }
};
