#pragma once
#ifndef GEO_CONFIG_PANEL_H
#define GEO_CONFIG_PANEL_H

#include "geospatial/GeoAPI.h"
#include <imgui.h>

namespace UI {

struct GeoPanelState {
    int activeSubTab = 0;  // 0=Tracking, 1=Feeds, 2=Prediction, 3=Storage

    // GPS controls
    int gpsModeIndex = 0;
    float gpsSpeed = 1.0f;
    float gpsNoise = 5.0f;

    // Prediction controls
    float predictionInterval = 2.0f;
    double predictionHorizon = 60.0;
    int predictionPoints = 50;

    // Visualization toggles
    bool showPoints = true;
    bool showTrajectory = true;
    bool showPrediction = true;
    bool showUncertainty = false;
    bool showMonteCarlo = false;
    bool trackBot = false;
    float pointSize = 8.0f;
    float trajectoryOpacity = 0.8f;
    int maxHistoryPoints = 500;

    // Feed management
    char feedUrlBuffer[256] = "";
    char feedTypeBuffer[16] = "REST";

    // Origin coordinates
    double originLat = 0.0;
    double originLon = 0.0;
    double originAlt = 0.0;

    // Dirty flag to avoid per-frame sync
    bool needsSync = true;
    float lastSyncTime = 0.0f;
};

// Main geo panel entry point
void RenderGeoPanel(geo::GeoAPI& geoApi, GeoPanelState& state);

// Sub-panels
void RenderGeoTrackingTab(geo::GeoAPI& geoApi, GeoPanelState& state);
void RenderGeoFeedsTab(geo::GeoAPI& geoApi, GeoPanelState& state);
void RenderGeoPredictionTab(geo::GeoAPI& geoApi, GeoPanelState& state);
void RenderGeoStorageTab(geo::GeoAPI& geoApi, GeoPanelState& state);

// Status bar widget (compact, called every frame from status bar)
void RenderGeoStatusBar(geo::GeoAPI& geoApi);

// Geo panel state persistence (geo_panel.cfg).
bool SaveGeoPanelState(const GeoPanelState& state);
bool LoadGeoPanelState(GeoPanelState& state);

}

#endif
