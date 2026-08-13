/**
 * Tests for GeoPanelState persistence (geo_panel.cfg): a full save/load
 * round-trip restores every field, a missing file keeps current values, and
 * corrupt/out-of-range values are clamped to the UI-valid ranges.
 *
 * Run with: make test
 */

#include <gtest/gtest.h>
#include "editor/geo_config_panel.h"

#include <cstdio>
#include <fstream>

namespace {

// Wipes geo_panel.cfg before and after each test so the test never leaks
// state into the real app's cfg file.
class CfgGuard {
public:
    CfgGuard() { std::remove("geo_panel.cfg"); }
    ~CfgGuard() { std::remove("geo_panel.cfg"); }
};

// Fills every persistable field with a distinctive non-default value.
void FillState(UI::GeoPanelState& s) {
    s.activeSubTab = 2;
    s.gpsModeIndex = 3;
    s.gpsSpeed = 2.5f;
    s.gpsNoise = 12.0f;
    s.predictionInterval = 3.0f;
    s.predictionHorizon = 120.0;
    s.predictionPoints = 75;
    s.trackBot = true;
    s.showPoints = false;
    s.showTrajectory = true;
    s.showPrediction = false;
    s.showUncertainty = true;
    s.showMonteCarlo = true;
    s.pointSize = 14.0f;
    s.trajectoryOpacity = 0.55f;
    s.maxHistoryPoints = 900;
    s.originLat = -33.8568;
    s.originLon = 151.2153;
    s.originAlt = 50.0;
}

}  // namespace

TEST(GeoPanelPersistence, FullRoundTrip) {
    CfgGuard guard;

    UI::GeoPanelState src;
    FillState(src);
    ASSERT_TRUE(UI::SaveGeoPanelState(src));

    UI::GeoPanelState dst;  // defaults
    ASSERT_TRUE(UI::LoadGeoPanelState(dst));

    EXPECT_EQ(dst.activeSubTab, src.activeSubTab);
    EXPECT_EQ(dst.gpsModeIndex, src.gpsModeIndex);
    EXPECT_FLOAT_EQ(dst.gpsSpeed, src.gpsSpeed);
    EXPECT_FLOAT_EQ(dst.gpsNoise, src.gpsNoise);
    EXPECT_FLOAT_EQ(dst.predictionInterval, src.predictionInterval);
    EXPECT_DOUBLE_EQ(dst.predictionHorizon, src.predictionHorizon);
    EXPECT_EQ(dst.predictionPoints, src.predictionPoints);
    EXPECT_EQ(dst.trackBot, src.trackBot);
    EXPECT_EQ(dst.showPoints, src.showPoints);
    EXPECT_EQ(dst.showTrajectory, src.showTrajectory);
    EXPECT_EQ(dst.showPrediction, src.showPrediction);
    EXPECT_EQ(dst.showUncertainty, src.showUncertainty);
    EXPECT_EQ(dst.showMonteCarlo, src.showMonteCarlo);
    EXPECT_FLOAT_EQ(dst.pointSize, src.pointSize);
    EXPECT_FLOAT_EQ(dst.trajectoryOpacity, src.trajectoryOpacity);
    EXPECT_EQ(dst.maxHistoryPoints, src.maxHistoryPoints);
    EXPECT_DOUBLE_EQ(dst.originLat, src.originLat);
    EXPECT_DOUBLE_EQ(dst.originLon, src.originLon);
    EXPECT_DOUBLE_EQ(dst.originAlt, src.originAlt);
}

TEST(GeoPanelPersistence, MissingFileKeepsCurrentValues) {
    CfgGuard guard;

    UI::GeoPanelState s;
    s.pointSize = 19.0f;
    EXPECT_FALSE(UI::LoadGeoPanelState(s));  // no file -> false
    EXPECT_FLOAT_EQ(s.pointSize, 19.0f);     // untouched
}

TEST(GeoPanelPersistence, OutOfRangeValuesAreClamped) {
    CfgGuard guard;

    {
        std::ofstream f("geo_panel.cfg");
        f << "gpsModeIndex=99\n";         // clamp to 4 (Aircraft)
        f << "maxHistoryPoints=99999\n";  // clamp to 2000
        f << "pointSize=0\n";             // clamp to 2
        f << "garbage line without key\n"; // ignored
    }

    UI::GeoPanelState s;  // defaults
    EXPECT_TRUE(UI::LoadGeoPanelState(s));
    EXPECT_EQ(s.gpsModeIndex, 4);
    EXPECT_EQ(s.maxHistoryPoints, 2000);
    EXPECT_FLOAT_EQ(s.pointSize, 2.0f);
    // Unlisted fields keep defaults.
    EXPECT_FLOAT_EQ(s.trajectoryOpacity, 0.8f);
}

TEST(GeoPanelPersistence, CorruptValuesKeepDefaults) {
    CfgGuard guard;

    {
        std::ofstream f("geo_panel.cfg");
        f << "pointSize=not-a-number\n";  // parse fail -> keep default
        f << "trackBot=maybe\n";          // not 1/0/true/false -> keep default
    }

    UI::GeoPanelState s;  // defaults
    EXPECT_TRUE(UI::LoadGeoPanelState(s));
    EXPECT_FLOAT_EQ(s.pointSize, 8.0f);
    EXPECT_FALSE(s.trackBot);
}
