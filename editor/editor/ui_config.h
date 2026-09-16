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
inline int gGridSize = 20;
inline float gGridSpacing = 1.0f;
inline std::string gContentBrowserPath = "assets/";

// Theme mode persisted across restarts: 0 = Dark, 1 = Light. Mirrors
// EditorTheme::ThemeMode; kept as a plain int so ui_config.h stays free of
// an ImGui dependency.
inline int gThemeMode = 0;

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
    std::string line;
    int lineIdx = 0;
    while (std::getline(f, line) && lineIdx < 6) {
        if (lineIdx == 0) size = std::atoi(line.c_str());
        else if (lineIdx == 1) spacing = static_cast<float>(std::atof(line.c_str()));
        else if (lineIdx == 2) outl = std::atoi(line.c_str());
        else if (lineIdx == 3) det = std::atoi(line.c_str());
        else if (lineIdx == 4) path = line;
        else if (lineIdx == 5) theme = std::atoi(line.c_str());
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

    return true;
}

} // namespace UIConfig