#include "geo_config_panel.h"
#include "console.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>

namespace UI {

// Helper: format coordinate as DMS without string alloc
static void FormatLatLon(double lat, double lon) {
    char buf[128];

    char latDir = lat >= 0 ? 'N' : 'S';
    double latAbs = lat < 0 ? -lat : lat;
    int latDeg = (int)latAbs;
    int latMin = (int)((latAbs - latDeg) * 60);
    double latSec = (latAbs - latDeg - latMin / 60.0) * 3600;

    char lonDir = lon >= 0 ? 'E' : 'W';
    double lonAbs = lon < 0 ? -lon : lon;
    int lonDeg = (int)lonAbs;
    int lonMin = (int)((lonAbs - lonDeg) * 60);
    double lonSec = (lonAbs - lonDeg - lonMin / 60.0) * 3600;

    snprintf(buf, sizeof(buf), "%d°%d'%.1f\"%c  %d°%d'%.1f\"%c",
             latDeg, latMin, latSec, latDir,
             lonDeg, lonMin, lonSec, lonDir);
    ImGui::TextUnformatted(buf);
}

// Helper: colored status indicator
static void StatusIndicator(bool active, const char* label) {
    ImVec4 color = active ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f) : ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
    ImGui::TextColored(color, active ? "[ON]" : "[OFF]");
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
}

void RenderGeoPanel(geo::GeoAPI& geoApi, GeoPanelState& state) {
    // Sub-tab bar
    const char* tabs[] = { "Tracking", "Feeds", "Prediction", "Storage" };
    float tabW = ImGui::GetContentRegionAvail().x / 4.0f - 2.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 4));
    for (int i = 0; i < 4; i++) {
        if (i > 0) ImGui::SameLine();
        bool active = (state.activeSubTab == i);
        ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(0.15f, 0.25f, 0.15f, 1.0f) : ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(0.6f, 1.0f, 0.6f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        if (ImGui::Button(tabs[i], ImVec2(tabW, 22))) {
            state.activeSubTab = i;
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::PopStyleVar();

    ImGui::Separator();

    // Render active tab
    switch (state.activeSubTab) {
        case 0: RenderGeoTrackingTab(geoApi, state); break;
        case 1: RenderGeoFeedsTab(geoApi, state); break;
        case 2: RenderGeoPredictionTab(geoApi, state); break;
        case 3: RenderGeoStorageTab(geoApi, state); break;
    }
}

void RenderGeoTrackingTab(geo::GeoAPI& geoApi, GeoPanelState& state) {
    auto gps = geoApi.getGPSStatus();

    // GPS status block
    ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "GPS STATUS");
    ImGui::Separator();

    StatusIndicator(gps.isValid, gps.isValid ? "Fix Acquired" : "No Fix");
    ImGui::Text("Mode: %s", gps.modeName);

    if (gps.isValid) {
        FormatLatLon(gps.latitude, gps.longitude);
        ImGui::Text("Alt: %.2f m", gps.altitude);
        ImGui::Text("Speed: %.2f m/s  |  Heading: %.1f°", gps.speed, gps.heading);
        ImGui::Text("Accuracy: ±%.2f m", gps.accuracy);
    }
    ImGui::Spacing();

    // GPS simulation controls
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.9f, 1.0f), "SIMULATION");
    ImGui::Separator();

    // Combo index maps 1:1 to GPSTracker::Mode (0=DISABLED..4=AIRCRAFT).
    const char* modes[] = { "Disabled", "Static", "Walk", "Vehicle", "Aircraft" };
    state.gpsModeIndex = std::max(0, std::min(4, state.gpsModeIndex));
    if (ImGui::Combo("Mode", &state.gpsModeIndex, modes, 5)) {
        geoApi.setGPSMode(static_cast<GPSTracker::Mode>(state.gpsModeIndex));
    }

    if (ImGui::SliderFloat("Speed", &state.gpsSpeed, 0.1f, 10.0f, "%.1fx")) {
        geoApi.setGPSSpeed(state.gpsSpeed);
    }

    if (ImGui::SliderFloat("Noise", &state.gpsNoise, 0.0f, 50.0f, "%.1f m")) {
        geoApi.setGPSNoise(state.gpsNoise);
    }

    ImGui::Spacing();

    // Tracked entities table (using cached snapshots, no ECS query)
    ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f), "TRACKED ENTITIES (%zu)", geoApi.getSnapshotCount());
    ImGui::Separator();

    const auto& snapshots = geoApi.getSnapshots();
    if (snapshots.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "No entities tracked");
    } else {
        // Use ImGui table for efficient rendering
        if (ImGui::BeginTable("##TrackedEntities", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Accuracy", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Pred", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableHeadersRow();

            for (const auto& snap : snapshots) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", snap.id);

                ImGui::TableNextColumn();
                if (snap.isValid) {
                    char pos[64];
                    snprintf(pos, sizeof(pos), "%.6f, %.6f", snap.latitude, snap.longitude);
                    ImGui::TextUnformatted(pos);
                } else {
                    ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "--");
                }

                ImGui::TableNextColumn();
                if (snap.isValid) {
                    char acc[32];
                    snprintf(acc, sizeof(acc), "±%.1f", snap.accuracy);
                    ImGui::TextUnformatted(acc);
                }

                ImGui::TableNextColumn();
                if (snap.hasPrediction) {
                    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "%.0f%%", snap.predictionConfidence * 100);
                } else {
                    ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "--");
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::Spacing();

    // Visualization toggles
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "VISUALIZATION");
    ImGui::Separator();

    ImGui::Checkbox("Points", &state.showPoints);
    ImGui::SameLine();
    ImGui::Checkbox("Trajectory", &state.showTrajectory);
    ImGui::SameLine();
    ImGui::Checkbox("Prediction", &state.showPrediction);
    ImGui::SameLine();
    ImGui::Checkbox("Uncertainty", &state.showUncertainty);
    ImGui::SameLine();
    ImGui::Checkbox("Monte Carlo", &state.showMonteCarlo);

    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("Size", &state.pointSize, 2.0f, 20.0f, "%.0f");
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("Opacity", &state.trajectoryOpacity, 0.1f, 1.0f, "%.2f");
    ImGui::SetNextItemWidth(120);
    ImGui::SliderInt("Max History", &state.maxHistoryPoints, 50, 2000);
}

void RenderGeoFeedsTab(geo::GeoAPI& geoApi, GeoPanelState& state) {
    ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "DATA FEEDS");
    ImGui::Separator();

    // Existing feeds
    size_t feedCount = geoApi.getFeedCount();
    if (feedCount > 0) {
        if (ImGui::BeginTable("##Feeds", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("URL", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < feedCount; i++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("Feed %zu", i);
                ImGui::TableNextColumn();
                ImGui::Text("REST");
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(("X##" + std::to_string(i)).c_str())) {
                    geoApi.removeFeed(i);
                }
            }
            ImGui::EndTable();
        }
    } else {
        ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "No feeds configured");
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.9f, 1.0f), "ADD FEED");
    ImGui::Separator();

    ImGui::SetNextItemWidth(-180);
    ImGui::InputTextWithHint("##FeedURL", "http://...", state.feedUrlBuffer, sizeof(state.feedUrlBuffer));
    ImGui::SameLine();
    if (ImGui::Button("Add", ImVec2(60, 0))) {
        if (strlen(state.feedUrlBuffer) > 0) {
            geoApi.addFeed(state.feedUrlBuffer, state.feedTypeBuffer);
            EditorConsole::Log(std::string("Added feed: ") + state.feedUrlBuffer);
            memset(state.feedUrlBuffer, 0, sizeof(state.feedUrlBuffer));
        }
    }
}

void RenderGeoPredictionTab(geo::GeoAPI& geoApi, GeoPanelState& state) {
    ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "PREDICTION MODEL");
    ImGui::Separator();

    const auto& predictions = geoApi.getCachedPredictions();

    ImGui::Text("Cached Predictions: %zu points", predictions.size());
    if (!predictions.empty()) {
        ImGui::Text("Confidence (midpoint): %.2f", predictions[predictions.size() / 2].confidence);
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.9f, 1.0f), "CONFIGURATION");
    ImGui::Separator();

    if (ImGui::SliderFloat("Interval", &state.predictionInterval, 0.5f, 10.0f, "%.1f s")) {
        geoApi.setPredictionInterval(state.predictionInterval);
    }

    float horizonFloat = static_cast<float>(state.predictionHorizon);
    if (ImGui::SliderFloat("Horizon", &horizonFloat, 10.0f, 300.0f, "%.0f s")) {
        state.predictionHorizon = static_cast<double>(horizonFloat);
        geoApi.setPredictionHorizon(state.predictionHorizon, state.predictionPoints);
    }

    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderInt("Points", &state.predictionPoints, 10, 200)) {
        geoApi.setPredictionHorizon(state.predictionHorizon, state.predictionPoints);
    }

    ImGui::Spacing();
    if (ImGui::Button("Request Prediction Now")) {
        geoApi.requestPrediction(state.predictionHorizon, state.predictionPoints);
    }
}

void RenderGeoStorageTab(geo::GeoAPI& geoApi, GeoPanelState& state) {
    ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "DATA STORAGE");
    ImGui::Separator();

    auto stats = geoApi.getStats();

    ImGui::Text("Tracked Entities: %zu", stats.trackedEntityCount);
    ImGui::Text("Points Stored: %zu", stats.totalPointsStored);
    ImGui::Text("Prediction Interval: %.1f s", stats.predictionInterval);
    ImGui::Text("System Uptime: %.0f s", stats.lastUpdateTime);

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.9f, 1.0f), "ORIGIN");
    ImGui::Separator();

    glm::dvec3 origin = geoApi.getConverter().getOriginWGS84();
    ImGui::Text("Lat: %.8f", origin.x);
    ImGui::Text("Lon: %.8f", origin.y);
    ImGui::Text("Alt: %.2f m", origin.z);
}

void RenderGeoStatusBar(geo::GeoAPI& geoApi) {
    auto gps = geoApi.getGPSStatus();
    auto stats = geoApi.getStats();

    if (gps.isValid) {
        char buf[96];
        snprintf(buf, sizeof(buf), "GPS: %.6f, %.6f | Entities: %zu | Pts: %zu",
                 gps.latitude, gps.longitude, stats.trackedEntityCount, stats.totalPointsStored);
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "%s", buf);
    } else {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Geo: No GPS fix | Entities: %zu", stats.trackedEntityCount);
    }
}

// ============================================================================
// Geo panel state persistence (geo_panel.cfg)
// ============================================================================
namespace {
constexpr const char* kGeoPanelCfgFile = "geo_panel.cfg";

std::string TrimCfg(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool ParseCfgInt(const std::string& v, int& out) {
    const std::string t = TrimCfg(v);
    if (t.empty()) return false;
    char* end = nullptr;
    const long val = std::strtol(t.c_str(), &end, 10);
    if (end == t.c_str() || *end != '\0') return false;
    out = static_cast<int>(val);
    return true;
}

bool ParseCfgFloat(const std::string& v, float& out) {
    const std::string t = TrimCfg(v);
    if (t.empty()) return false;
    char* end = nullptr;
    const double val = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0') return false;
    out = static_cast<float>(val);
    return true;
}

bool ParseCfgDouble(const std::string& v, double& out) {
    const std::string t = TrimCfg(v);
    if (t.empty()) return false;
    char* end = nullptr;
    const double val = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0') return false;
    out = val;
    return true;
}

bool ParseCfgBool(const std::string& v, bool& out) {
    const std::string t = TrimCfg(v);
    if (t == "1" || t == "true" || t == "TRUE" || t == "True") { out = true; return true; }
    if (t == "0" || t == "false" || t == "FALSE" || t == "False") { out = false; return true; }
    return false;
}
}  // namespace

bool SaveGeoPanelState(const GeoPanelState& s) {
    std::ofstream f(kGeoPanelCfgFile);
    if (!f.is_open()) return false;
    // Full double precision so doubles (origin coords, prediction horizon)
    // round-trip exactly.
    f << std::setprecision(std::numeric_limits<double>::max_digits10);
    f << "activeSubTab=" << s.activeSubTab << "\n";
    f << "gpsModeIndex=" << s.gpsModeIndex << "\n";
    f << "gpsSpeed=" << s.gpsSpeed << "\n";
    f << "gpsNoise=" << s.gpsNoise << "\n";
    f << "predictionInterval=" << s.predictionInterval << "\n";
    f << "predictionHorizon=" << s.predictionHorizon << "\n";
    f << "predictionPoints=" << s.predictionPoints << "\n";
    f << "trackBot=" << (s.trackBot ? 1 : 0) << "\n";
    f << "showPoints=" << (s.showPoints ? 1 : 0) << "\n";
    f << "showTrajectory=" << (s.showTrajectory ? 1 : 0) << "\n";
    f << "showPrediction=" << (s.showPrediction ? 1 : 0) << "\n";
    f << "showUncertainty=" << (s.showUncertainty ? 1 : 0) << "\n";
    f << "showMonteCarlo=" << (s.showMonteCarlo ? 1 : 0) << "\n";
    f << "pointSize=" << s.pointSize << "\n";
    f << "trajectoryOpacity=" << s.trajectoryOpacity << "\n";
    f << "maxHistoryPoints=" << s.maxHistoryPoints << "\n";
    f << "originLat=" << s.originLat << "\n";
    f << "originLon=" << s.originLon << "\n";
    f << "originAlt=" << s.originAlt << "\n";
    return f.good();
}

bool LoadGeoPanelState(GeoPanelState& s) {
    std::ifstream f(kGeoPanelCfgFile);
    if (!f.is_open()) return false;

    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;  // garbage lines are ignored
        const std::string key = TrimCfg(line.substr(0, eq));
        const std::string val = TrimCfg(line.substr(eq + 1));

        int i = 0; float fl = 0.0f; double d = 0.0; bool b = false;
        if (key == "activeSubTab" && ParseCfgInt(val, i)) s.activeSubTab = i;
        else if (key == "gpsModeIndex" && ParseCfgInt(val, i)) s.gpsModeIndex = std::max(0, std::min(4, i));
        else if (key == "gpsSpeed" && ParseCfgFloat(val, fl)) s.gpsSpeed = fl;
        else if (key == "gpsNoise" && ParseCfgFloat(val, fl)) s.gpsNoise = fl;
        else if (key == "predictionInterval" && ParseCfgFloat(val, fl)) s.predictionInterval = fl;
        else if (key == "predictionHorizon" && ParseCfgDouble(val, d)) s.predictionHorizon = d;
        else if (key == "predictionPoints" && ParseCfgInt(val, i)) s.predictionPoints = i;
        else if (key == "trackBot" && ParseCfgBool(val, b)) s.trackBot = b;
        else if (key == "showPoints" && ParseCfgBool(val, b)) s.showPoints = b;
        else if (key == "showTrajectory" && ParseCfgBool(val, b)) s.showTrajectory = b;
        else if (key == "showPrediction" && ParseCfgBool(val, b)) s.showPrediction = b;
        else if (key == "showUncertainty" && ParseCfgBool(val, b)) s.showUncertainty = b;
        else if (key == "showMonteCarlo" && ParseCfgBool(val, b)) s.showMonteCarlo = b;
        else if (key == "pointSize" && ParseCfgFloat(val, fl)) s.pointSize = std::max(2.0f, fl);
        else if (key == "trajectoryOpacity" && ParseCfgFloat(val, fl)) s.trajectoryOpacity = fl;
        else if (key == "maxHistoryPoints" && ParseCfgInt(val, i)) s.maxHistoryPoints = std::min(2000, i);
        else if (key == "originLat" && ParseCfgDouble(val, d)) s.originLat = d;
        else if (key == "originLon" && ParseCfgDouble(val, d)) s.originLon = d;
        else if (key == "originAlt" && ParseCfgDouble(val, d)) s.originAlt = d;
    }
    return true;
}

}
