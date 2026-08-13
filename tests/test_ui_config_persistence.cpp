/**
 * UI Config Persistence Tests
 *
 * Verifies UIConfig::SaveConfig() / LoadConfig(): a save/load round-trip
 * restores the grid size/spacing and the Outliner/Details panel toggles, and
 * invalid file values are ignored (falling back to current/default values).
 *
 * Run with: make test
 */

#include <gtest/gtest.h>
#include "editor/ui_config.h"
#include <fstream>
#include <cstdio>
#include <string>
#include <iterator>

namespace {

const char* kCfg = UIConfig::kConfigFile;

// Same RAII backup/restore guard as test_camera_mode_persistence.cpp so the
// tests never clobber the user's real ui_panel.cfg (the editor writes it to
// the project root).
class UIConfigCfgGuard {
public:
    UIConfigCfgGuard() {
        std::ifstream f(kCfg);
        if (f) {
            m_hadFile = true;
            m_saved.assign(std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>());
        }
    }
    ~UIConfigCfgGuard() {
        std::remove(kCfg);
        if (m_hadFile) {
            std::ofstream out(kCfg);
            out << m_saved;
        }
    }
private:
    bool m_hadFile = false;
    std::string m_saved;
};

} // namespace

TEST(UIConfigPersistence, SaveThenLoadRoundTrip) {
    UIConfigCfgGuard guard;

    std::remove(kCfg);
    UIConfig::gGridSize = 42;
    UIConfig::gGridSpacing = 2.5f;
    UIConfig::SaveConfig(true, false);

    // Mutate everything, then restore from disk.
    UIConfig::gGridSize = 7;
    UIConfig::gGridSpacing = 0.5f;
    bool outliner = false, details = true;
    UIConfig::LoadConfig(&outliner, &details);

    EXPECT_EQ(UIConfig::gGridSize, 42);
    EXPECT_FLOAT_EQ(UIConfig::gGridSpacing, 2.5f);
    EXPECT_TRUE(outliner);
    EXPECT_FALSE(details);

    std::remove(kCfg);
}

TEST(UIConfigPersistence, InvalidFileValuesAreIgnored) {
    UIConfigCfgGuard guard;

    {
        std::ofstream f(kCfg);
        f << "999999 50.0 2 2";  // out-of-range size/spacing, invalid toggles
    }
    UIConfig::gGridSize = 12;
    UIConfig::gGridSpacing = 1.0f;
    bool outliner = false, details = false;
    UIConfig::LoadConfig(&outliner, &details);

    // Clamped: size stays in [2,200], spacing in [0.1,20]; invalid toggles keep
    // the caller's current values.
    EXPECT_EQ(UIConfig::gGridSize, 12);
    EXPECT_FLOAT_EQ(UIConfig::gGridSpacing, 1.0f);
    EXPECT_FALSE(outliner);
    EXPECT_FALSE(details);

    std::remove(kCfg);
}

TEST(UIConfigPersistence, MissingFileLeavesValuesUnchanged) {
    UIConfigCfgGuard guard;

    std::remove(kCfg);
    UIConfig::gGridSize = 20;
    UIConfig::gGridSpacing = 1.0f;
    bool outliner = true, details = true;
    UIConfig::LoadConfig(&outliner, &details);

    EXPECT_EQ(UIConfig::gGridSize, 20);
    EXPECT_FLOAT_EQ(UIConfig::gGridSpacing, 1.0f);
    EXPECT_TRUE(outliner);
    EXPECT_TRUE(details);
}

TEST(UIConfigPersistence, NullTogglePointersAreSafe) {
    UIConfigCfgGuard guard;

    std::remove(kCfg);
    UIConfig::gGridSize = 30;
    UIConfig::gGridSpacing = 3.0f;
    UIConfig::SaveConfig(true, true);

    // LoadConfig with null toggle pointers must not crash and must still
    // restore the grid values.
    UIConfig::gGridSize = 1;
    UIConfig::gGridSpacing = 0.1f;
    UIConfig::LoadConfig(nullptr, nullptr);

    EXPECT_EQ(UIConfig::gGridSize, 30);
    EXPECT_FLOAT_EQ(UIConfig::gGridSpacing, 3.0f);

    std::remove(kCfg);
}

TEST(UIConfigPersistence, ContentBrowserPathRoundTrip) {
    UIConfigCfgGuard guard;

    std::remove(kCfg);
    UIConfig::gGridSize = 16;
    UIConfig::gGridSpacing = 1.5f;
    UIConfig::gContentBrowserPath = "models/";
    UIConfig::SaveConfig(true, false);

    UIConfig::gContentBrowserPath = "assets/";
    bool outliner = false, details = true;
    UIConfig::LoadConfig(&outliner, &details);

    EXPECT_EQ(UIConfig::gContentBrowserPath, "models/");
    EXPECT_TRUE(outliner);
    EXPECT_FALSE(details);

    std::remove(kCfg);
}

TEST(UIConfigPersistence, OldFourLineFileKeepsDefaultPath) {
    // Backward compatibility: a file without the path line (previous format)
    // restores grid/toggles and leaves the path at its default.
    UIConfigCfgGuard guard;

    std::remove(kCfg);
    {
        std::ofstream f(kCfg);
        f << "25\n";
        f << "2.0\n";
        f << "1\n";
        f << "0\n";
    }
    UIConfig::gContentBrowserPath = "assets/";
    UIConfig::gGridSize = 1;
    bool outliner = false, details = true;
    UIConfig::LoadConfig(&outliner, &details);

    EXPECT_EQ(UIConfig::gGridSize, 25);
    EXPECT_EQ(UIConfig::gContentBrowserPath, "assets/");
    EXPECT_TRUE(outliner);
    EXPECT_FALSE(details);

    std::remove(kCfg);
}
