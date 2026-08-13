/**
 * Camera Mode Persistence Tests
 *
 * Verifies RenderPipeline::saveCameraModeToDisk() / loadCameraModeFromDisk():
 * a save/load round-trip restores the mode, and an invalid file value is
 * ignored (falling back to the current/default mode).
 *
 * Run with: make test
 */

#include <gtest/gtest.h>
#include "editor/render_pipeline.h"
#include "editor/camera_controls.h"
#include <fstream>
#include <cstdio>
#include <string>
#include <iterator>

namespace {

// The persistence file is fixed (kCameraModeConfigFile) in the working dir.
const char* kCfg = Render::RenderPipeline::kCameraModeConfigFile;

// These tests deliberately delete/recreate kCfg in the project root. If a real
// camera_mode.cfg exists (the editor writes it there), the tests would clobber
// the user's saved camera settings on every `make test`. This RAII guard backs
// up the file's original content and restores it on destruction (even on test
// failure), so the suite never destroys user state.
class CameraModeCfgGuard {
public:
    CameraModeCfgGuard() {
        std::ifstream f(kCfg);
        if (f) {
            m_hadFile = true;
            m_saved.assign(std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>());
        }
    }
    ~CameraModeCfgGuard() {
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

TEST(CameraModePersistence, SaveThenLoadRoundTrip) {
    CameraModeCfgGuard guard;
    auto& rp = Render::RenderPipeline::getInstance();

    std::remove(kCfg);
    rp.setCameraMode(Render::CameraMode::FreeFly);
    rp.saveCameraModeToDisk();

    // Change the mode away, then restore from disk.
    rp.setCameraMode(Render::CameraMode::Cinematic);
    rp.loadCameraModeFromDisk();

    EXPECT_EQ(rp.getCameraMode(), Render::CameraMode::FreeFly);

    std::remove(kCfg);
}

TEST(CameraModePersistence, PersistsAnyValidMode) {
    CameraModeCfgGuard guard;
    auto& rp = Render::RenderPipeline::getInstance();

    for (int mode = static_cast<int>(Render::CameraMode::FreeFly);
         mode <= static_cast<int>(Render::CameraMode::Cinematic); ++mode) {
        rp.setCameraMode(static_cast<Render::CameraMode>(mode));
        rp.saveCameraModeToDisk();
        rp.setCameraMode(Render::CameraMode::Cinematic);
        rp.loadCameraModeFromDisk();
        EXPECT_EQ(rp.getCameraMode(), static_cast<Render::CameraMode>(mode));
    }

    std::remove(kCfg);
}

TEST(CameraModePersistence, InvalidFileValueIsIgnored) {
    CameraModeCfgGuard guard;
    auto& rp = Render::RenderPipeline::getInstance();

    {
        std::ofstream f(kCfg);
        f << "99";  // out of range
    }
    rp.setCameraMode(Render::CameraMode::Orbit);
    rp.loadCameraModeFromDisk();
    EXPECT_EQ(rp.getCameraMode(), Render::CameraMode::Orbit);

    std::remove(kCfg);
}

TEST(CameraModePersistence, MissingFileLeavesModeUnchanged) {
    CameraModeCfgGuard guard;
    auto& rp = Render::RenderPipeline::getInstance();

    std::remove(kCfg);
    rp.setCameraMode(Render::CameraMode::ThirdPerson);
    rp.loadCameraModeFromDisk();
    EXPECT_EQ(rp.getCameraMode(), Render::CameraMode::ThirdPerson);
}

TEST(CameraModePersistence, TogglesRoundTrip) {
    CameraModeCfgGuard guard;
    auto& rp = Render::RenderPipeline::getInstance();

    std::remove(kCfg);
    rp.setCameraMode(Render::CameraMode::Orbit);
    CameraControls::gAutoOrbit = false;
    CameraControls::gCursorLock = true;
    rp.saveCameraModeToDisk();

    // Mutate everything, then restore.
    rp.setCameraMode(Render::CameraMode::Cinematic);
    CameraControls::gAutoOrbit = true;
    CameraControls::gCursorLock = false;
    rp.loadCameraModeFromDisk();

    EXPECT_EQ(rp.getCameraMode(), Render::CameraMode::Orbit);
    EXPECT_FALSE(CameraControls::gAutoOrbit);
    EXPECT_TRUE(CameraControls::gCursorLock);

    std::remove(kCfg);
}

TEST(CameraModePersistence, OldSingleValueFileLeavesTogglesUnchanged) {
    // Backward compatibility: a file with only the mode (previous format)
    // restores the mode and leaves the toggles untouched.
    CameraModeCfgGuard guard;
    auto& rp = Render::RenderPipeline::getInstance();

    std::remove(kCfg);
    {
        std::ofstream f(kCfg);
        f << static_cast<int>(Render::CameraMode::FirstPerson) << "\n";
    }
    rp.setCameraMode(Render::CameraMode::Cinematic);
    const bool oldAuto = CameraControls::gAutoOrbit;
    const bool oldLock = CameraControls::gCursorLock;
    rp.loadCameraModeFromDisk();

    EXPECT_EQ(rp.getCameraMode(), Render::CameraMode::FirstPerson);
    EXPECT_EQ(CameraControls::gAutoOrbit, oldAuto);
    EXPECT_EQ(CameraControls::gCursorLock, oldLock);

    std::remove(kCfg);
}
