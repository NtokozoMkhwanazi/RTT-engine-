#include "geo_config_panel.h"
#include "console.h"
#include "editor_theme.h"
#include "editor_state.h"   // g_editor (camera) for the Teleport action
#include "IconsFontAwesome6.h"  // ICON_FA_* macros
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

// Helper: colored status indicator (theme-based)
static void StatusIndicator(bool active, const char* label) {
    const auto& t = EditorTheme::Get();
    ImVec4 color = active ? t.accentGreen : t.accentDanger;
    ImGui::TextColored(color, active ? "[ON]" : "[OFF]");
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
}

void RenderGeoPanel(geo::GeoAPI& geoApi, GeoPanelState& state) {
    // Sub-tab bar (Unreal-style subtle segment control)
    const auto& theme = EditorTheme::Get();
    static const char* tabs[] = { ICON_FA_SATELLITE " Tracking", ICON_FA_ROUTE " Trajectory",
                                   ICON_FA_CLOUD_ARROW_UP " Import", ICON_FA_CLOUD_ARROW_DOWN " Export" };
    float tabW = ImGui::GetContentRegionAvail().x / 4.0f - 2.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 4));
    for (int i = 0; i < 4; i++) {
        if (i > 0) ImGui::SameLine();
        bool active = (state.activeSubTab == i);
        ImVec4 btnCol = active ? EditorTheme::WithAlpha(theme.accentGreen, 0.18f)
                               : theme.frameBg;
        ImVec4 txtCol = active ? theme.accentGreen : theme.textDim;
        ImGui::PushStyleColor(ImGuiCol_Button,        EditorTheme::ToU32(btnCol));
        ImGui::PushStyleColor(ImGuiCol_Text,          EditorTheme::ToU32(txtCol));
        // PushID isolates tab-button IDs from widget labels in the sub-tabs
        // (e.g. "Trajectory" appears as both a tab button and a checkbox).
        ImGui::PushID("GeoTabs");
        if (ImGui::Button(tabs[i], ImVec2(tabW, 22))) {
            state.activeSubTab = i;
        }
        ImGui::PopID();
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
    const auto& theme = EditorTheme::Get();
    // GPS status block
    ImGui::TextColored(theme.text, ICON_FA_SATELLITE " GPS STATUS");
    ImGui::Separator();

    StatusIndicator(gps.isValid, gps.isValid ? "Fix Acquired" : "No Fix");
    ImGui::Text(ICON_FA_CIRCLE_INFO " Mode: %s", gps.modeName);

    if (gps.isValid) {
        FormatLatLon(gps.latitude, gps.longitude);
        ImGui::Text(ICON_FA_ARROW_UP " Alt: %.2f m", gps.altitude);
        ImGui::Text(ICON_FA_GAUGE " Speed: %.2f m/s  |  " ICON_FA_COMPASS " Heading: %.1f°", gps.speed, gps.heading);
        ImGui::Text(ICON_FA_LOCATION_DOT " Accuracy: ±%.2f m", gps.accuracy);
    }
    ImGui::Spacing();

    // GPS simulation controls
    ImGui::TextColored(theme.accentBright, ICON_FA_SLIDERS " SIMULATION");
    ImGui::Separator();

    // Combo index maps 1:1 to GPSTracker::Mode (0=DISABLED..4=AIRCRAFT).
    const char* modes[] = { ICON_FA_BAN " Disabled", ICON_FA_PERSON " Static",
                            ICON_FA_PERSON_WALKING " Walk", ICON_FA_CAR " Vehicle",
                            ICON_FA_PLANE " Aircraft" };
    if (ImGui::Combo(ICON_FA_GAUGE " Mode", &state.gpsModeIndex, modes, 5)) {
        geoApi.setGPSMode(static_cast<GPSTracker::Mode>(state.gpsModeIndex));
        SaveGeoPanelState(state);
    }

    if (ImGui::SliderFloat(ICON_FA_GAUGE " Speed", &state.gpsSpeed, 0.1f, 10.0f, "%.1fx")) {
        geoApi.setGPSSpeed(state.gpsSpeed);
    }

    if (ImGui::SliderFloat(ICON_FA_WAVE_SQUARE " Noise", &state.gpsNoise, 0.0f, 50.0f, "%.1f m")) {
        geoApi.setGPSNoise(state.gpsNoise);
    }

    ImGui::Spacing();

    // Tracked entities table (using cached snapshots, no ECS query)
    ImGui::TextColored(theme.accentWarn, ICON_FA_USERS " TRACKED ENTITIES (%zu)", geoApi.getSnapshotCount());
    ImGui::Separator();

    const auto& snapshots = geoApi.getSnapshots();
    if (snapshots.empty()) {
        ImGui::TextColored(theme.textDim, ICON_FA_INBOX " No entities tracked");
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
                ImGui::PushID((int)snap.id);
                ImGui::TableNextColumn();
                ImGui::Text("%u", snap.id);

                ImGui::TableNextColumn();
                if (snap.isValid) {
                    char pos[64];
                    snprintf(pos, sizeof(pos), "%.6f, %.6f", snap.latitude, snap.longitude);
                    ImGui::TextUnformatted(pos);
                    // Inline copy button -> system clipboard (forUI.txt §2).
                    ImGui::SameLine(0, 4);
                    if (ImGui::Button(ICON_FA_COPY " Copy")) {
                        ImGui::SetClipboardText(pos);
                    }
                } else {
                    ImGui::TextColored(theme.textDim, "--");
                }

                ImGui::TableNextColumn();
                if (snap.isValid) {
                    char acc[32];
                    snprintf(acc, sizeof(acc), "±%.1f", snap.accuracy);
                    ImGui::TextUnformatted(acc);
                }

                ImGui::TableNextColumn();
                if (snap.hasPrediction) {
                    ImGui::TextColored(theme.accentGreen, "%.0f%%", snap.predictionConfidence * 100);
                } else {
                    ImGui::TextColored(theme.textDim, "--");
                }

                // Right-click any tracked row -> Teleport the editor camera to
                // the entity's WGS84-converted world position.
                if (ImGui::BeginPopupContextItem("##geo_teleport")) {
                    if (ImGui::MenuItem(ICON_FA_MAP_LOCATION_DOT " Teleport Camera Here") && snap.isValid) {
                        flyCamera* cam = g_editor.camera();
                        if (cam) {
                            glm::vec3 local = geoApi.getConverter().geospatialToLocal(
                                snap.latitude, snap.longitude, 0.0);
                            cam->Target = local;
                            cam->Position = local + glm::vec3(0.0f, 3.0f, 8.0f);
                        }
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    ImGui::Spacing();

    // Visualization toggles
    ImGui::TextColored(theme.accent, ICON_FA_EYE " VISUALIZATION");
    ImGui::Separator();

    ImGui::Checkbox(ICON_FA_CIRCLE_DOT " Points", &state.showPoints);
    ImGui::SameLine();
    ImGui::Checkbox(ICON_FA_ROUTE " Trajectory", &state.showTrajectory);
    ImGui::SameLine();
    ImGui::Checkbox(ICON_FA_ARROW_TREND_UP " Prediction", &state.showPrediction);
    ImGui::SameLine();
    ImGui::Checkbox(ICON_FA_CLOUD_RAIN " Uncertainty", &state.showUncertainty);
    ImGui::SameLine();
    ImGui::Checkbox(ICON_FA_DICE " Monte Carlo", &state.showMonteCarlo);

    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat(ICON_FA_FONT " Size", &state.pointSize, 2.0f, 20.0f, "%.0f px");
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat(ICON_FA_DROPLET " Opacity", &state.trajectoryOpacity, 0.1f, 1.0f, "%.2f");
    ImGui::SetNextItemWidth(120);
    ImGui::SliderInt(ICON_FA_CLOCK " Max History", &state.maxHistoryPoints, 50, 2000);
}

void RenderGeoFeedsTab(geo::GeoAPI& geoApi, GeoPanelState& state) {
    const auto& theme = EditorTheme::Get();
    ImGui::TextColored(theme.text, ICON_FA_CLOUD " DATA FEEDS");
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
                ImGui::Text(ICON_FA_TAG " Feed %zu", i);
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
        ImGui::TextColored(theme.textDim, ICON_FA_INBOX " No feeds configured");
    }

    ImGui::Separator();
    ImGui::TextColored(theme.accentBright, ICON_FA_PLUS " ADD FEED");
    ImGui::Separator();

    ImGui::SetNextItemWidth(-180);
    ImGui::InputTextWithHint("##FeedURL", "http://...", state.feedUrlBuffer, sizeof(state.feedUrlBuffer));
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLUS " Add", ImVec2(60, 0))) {
        if (strlen(state.feedUrlBuffer) > 0) {
            geoApi.addFeed(state.feedUrlBuffer, state.feedTypeBuffer);
            EditorConsole::Log(std::string("Added feed: ") + state.feedUrlBuffer);
            memset(state.feedUrlBuffer, 0, sizeof(state.feedUrlBuffer));
        }
    }
}

void RenderGeoPredictionTab(geo::GeoAPI& geoApi, GeoPanelState& state) {
    const auto& theme = EditorTheme::Get();
    ImGui::TextColored(theme.text, ICON_FA_BRAIN " PREDICTION MODEL");
    ImGui::Separator();

    const auto& predictions = geoApi.getCachedPredictions();

    ImGui::Text(ICON_FA_DATABASE " Cached Predictions: %zu points", predictions.size());
    if (!predictions.empty()) {
    ImGui::Text(ICON_FA_CHART_LINE " Confidence (midpoint): %.2f", predictions[predictions.size() / 2].confidence);
    }

    ImGui::Separator();
    ImGui::TextColored(theme.accentBright, ICON_FA_GEAR " CONFIGURATION");
    ImGui::Separator();

    if (ImGui::SliderFloat(ICON_FA_CLOCK " Interval", &state.predictionInterval, 0.5f, 10.0f, "%.1f s")) {
        geoApi.setPredictionInterval(state.predictionInterval);
    }

    float horizonFloat = static_cast<float>(state.predictionHorizon);
    if (ImGui::SliderFloat(ICON_FA_EYE " Horizon", &horizonFloat, 10.0f, 300.0f, "%.0f s")) {
        state.predictionHorizon = static_cast<double>(horizonFloat);
        geoApi.setPredictionHorizon(state.predictionHorizon, state.predictionPoints);
    }

    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderInt(ICON_FA_CIRCLE_DOT " Points", &state.predictionPoints, 10, 200)) {
        geoApi.setPredictionHorizon(state.predictionHorizon, state.predictionPoints);
    }

    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_BOLT " Request Prediction Now")) {
        geoApi.requestPrediction(state.predictionHorizon, state.predictionPoints);
    }
}

void RenderGeoStorageTab(geo::GeoAPI& geoApi, GeoPanelState& state) {
    const auto& theme = EditorTheme::Get();
    ImGui::TextColored(theme.text, ICON_FA_SERVER " DATA STORAGE");
    ImGui::Separator();

    auto stats = geoApi.getStats();

    ImGui::Text(ICON_FA_USER " Tracked Entities: %zu", stats.trackedEntityCount);
    ImGui::Text(ICON_FA_DATABASE " Points Stored: %zu", stats.totalPointsStored);
    ImGui::Text(ICON_FA_CLOCK " Prediction Interval: %.1f s", stats.predictionInterval);
    ImGui::Text(ICON_FA_CLOCK " System Uptime: %.0f s", stats.lastUpdateTime);

    ImGui::Separator();
    ImGui::TextColored(theme.accentBright, ICON_FA_MAP_LOCATION " ORIGIN");
    ImGui::Separator();

    glm::dvec3 origin = geoApi.getConverter().getOriginWGS84();
    ImGui::Text(ICON_FA_LOCATION_DOT " Lat: %.8f", origin.x);
    ImGui::Text(ICON_FA_LOCATION_DOT " Lon: %.8f", origin.y);
    ImGui::Text(ICON_FA_ARROW_UP " Alt: %.2f m", origin.z);
}

void RenderGeoStatusBar(geo::GeoAPI& geoApi) {
    const auto& theme = EditorTheme::Get();
    auto gps = geoApi.getGPSStatus();
    auto stats = geoApi.getStats();

    if (gps.isValid) {
        char buf[96];
        snprintf(buf, sizeof(buf), ICON_FA_SATELLITE " GPS: %.6f, %.6f | " ICON_FA_USERS " Entities: %zu | " ICON_FA_DATABASE " Pts: %zu",
                 gps.latitude, gps.longitude, stats.trackedEntityCount, stats.totalPointsStored);
        ImGui::TextColored(theme.accentGreen, "%s", buf);
    } else {
        ImGui::TextColored(theme.textDim, ICON_FA_MAP " Geo: No GPS fix | " ICON_FA_USERS " Entities: %zu", stats.trackedEntityCount);
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
