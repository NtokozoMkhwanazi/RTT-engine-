#pragma once
/**
 * ui_config.h
 *
 * UI config persistence (ui_panel.cfg): grid size/spacing, Outliner/Details
 * panel toggles, and the content browser path. Save/load round-trip with
 * clamping of out-of-range values and null-safe toggle pointers.
 */

#include <string>
#include <fstream>
#include <cstdio>

namespace UIConfig {

inline const char* kConfigFile = "ui_panel.cfg";

// Grid config globals.
inline int gGridSize = 100;  // was: 20 — 20-unit grid was a small dot under the bot, not a floor
inline float gGridSpacing = 1.0f;
inline std::string gContentBrowserPath = "assets/";

// Theme mode persisted across restarts: 0 = Dark, 1 = Light. Mirrors
// EditorTheme::ThemeMode; kept as a plain int so ui_config.h stays free of
// an ImGui dependency.
inline int gThemeMode = 0;

// UI zoom/scale factor (0.5–2.0, default 1.0). Persisted so the chosen zoom
// level survives editor restarts.
inline float gUIScale = 1.0f;

// Panel dimensions persisted across restarts (verify.txt: splitter positions
// serialize to ui_panel.cfg so layout doesn't snap back on every reboot).
inline float gLeftPanelWidth   = 280.0f;
inline float gRightPanelWidth  = 300.0f;
inline float gBottomPanelHeight = 180.0f;

// Save the current config (outliner/details toggles) to disk.
inline bool SaveConfig(bool outlinerVisible, bool detailsVisible) {
    std::ofstream f(kConfigFile);
    if (!f) return false;
    f << gGridSize << "\n";
    f << gGridSpacing << "\n";
    f << (outlinerVisible ? 1 : 0) << "\n";
    f << (detailsVisible ? 1 : 0) << "\n";
    f << gContentBrowserPath << "\n";
    f << gThemeMode << "\n";
    f << gUIScale << "\n";
    f << gLeftPanelWidth << "\n";
    f << gRightPanelWidth << "\n";
    f << gBottomPanelHeight << "\n";
    return f.good();
}

// Load config from disk into the globals (clamped). Toggle pointers may be
// null; invalid file values are ignored (caller's current values kept).
inline bool LoadConfig(bool* outlinerVisible, bool* detailsVisible) {
    std::ifstream f(kConfigFile);
    if (!f) return false;

    int size = -1;
    float spacing = -1.0f;
    int outl = -1, det = -1;
    std::string path;

    int theme = -1;
    float uiScale = -1.0f;
    float leftW = -1.0f, rightW = -1.0f, bottomH = -1.0f;
    std::string line;
    int lineIdx = 0;
    while (std::getline(f, line) && lineIdx < 11) {
        if (lineIdx == 0) size = std::atoi(line.c_str());
        else if (lineIdx == 1) spacing = static_cast<float>(std::atof(line.c_str()));
        else if (lineIdx == 2) outl = std::atoi(line.c_str());
        else if (lineIdx == 3) det = std::atoi(line.c_str());
        else if (lineIdx == 4) path = line;
        else if (lineIdx == 5) theme = std::atoi(line.c_str());
        else if (lineIdx == 6) uiScale = static_cast<float>(std::atof(line.c_str()));
        else if (lineIdx == 7) leftW = static_cast<float>(std::atof(line.c_str()));
        else if (lineIdx == 8) rightW = static_cast<float>(std::atof(line.c_str()));
        else if (lineIdx == 9) bottomH = static_cast<float>(std::atof(line.c_str()));
        ++lineIdx;
    }

    // Grid size clamped to [2, 200], spacing to [0.1, 20].
    if (size >= 2 && size <= 200) gGridSize = size;
    if (spacing >= 0.1f && spacing <= 20.0f) gGridSpacing = spacing;

    if (outlinerVisible && (outl == 0 || outl == 1)) *outlinerVisible = (outl == 1);
    if (detailsVisible && (det == 0 || det == 1)) *detailsVisible = (det == 1);

    if (!path.empty()) gContentBrowserPath = path;

    // Theme line is optional (older configs have only 5 lines): missing or
    // invalid values keep the current mode.
    if (theme == 0 || theme == 1) gThemeMode = theme;

    // UI scale line is optional (older configs don't have it): missing or
    // out-of-range values keep the current scale.
    if (uiScale >= 0.5f && uiScale <= 2.0f) gUIScale = uiScale;

    // Panel dimensions: clamp to sane ranges to avoid a corrupted .cfg
    // collapsing the viewport to nothing.
    if (leftW >= 100.0f && leftW <= 2000.0f)     gLeftPanelWidth   = leftW;
    if (rightW >= 100.0f && rightW <= 2000.0f)   gRightPanelWidth  = rightW;
    if (bottomH >= 50.0f && bottomH <= 1000.0f)  gBottomPanelHeight = bottomH;

    return true;
}

} // namespace UIConfig