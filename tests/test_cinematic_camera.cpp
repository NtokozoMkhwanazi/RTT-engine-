/**
 * Cinematic Camera Math Tests
 *
 * Verifies CinematicCamera.h - the pure math behind the Cinematic mode's
 * forever-orbit: angle advancement (with 360 deg wrap), orbit position on the
 * circle, and the intro height ease. No GL or ECS required.
 *
 * Run with: make test
 */

#include <gtest/gtest.h>
#include "cameraSystem/CinematicCamera.h"
#include <glm/glm.hpp>
#include <cmath>

namespace {
constexpr float kEps = 1e-4f;
} // namespace

// ---------------------------------------------------------------------------
// AdvanceAngle
// ---------------------------------------------------------------------------

TEST(CinematicCamera, AdvanceAngleAdvances) {
    EXPECT_NEAR(Cinematic::AdvanceAngle(0.0, 18.0, 1.0f), 18.0, 1e-9);
    EXPECT_NEAR(Cinematic::AdvanceAngle(180.0, 18.0, 0.5f), 189.0, 1e-9);
    EXPECT_NEAR(Cinematic::AdvanceAngle(90.0, -18.0, 1.0f), 72.0, 1e-9);
}

TEST(CinematicCamera, AdvanceAngleWrapsAt360) {
    EXPECT_NEAR(Cinematic::AdvanceAngle(350.0, 18.0, 1.0f), 8.0, 1e-9);
    EXPECT_NEAR(Cinematic::AdvanceAngle(355.0, 10.0, 1.0f), 5.0, 1e-9);
    EXPECT_NEAR(Cinematic::AdvanceAngle(359.0, 2.0, 1.0f), 1.0, 1e-9);
    // Result always lands in [0, 360).
    const double a = Cinematic::AdvanceAngle(0.0, 18.0, 1.0f);
    EXPECT_GE(a, 0.0);
    EXPECT_LT(a, 360.0);
}

TEST(CinematicCamera, AdvanceAngleClampsNegativeResults) {
    // Starting below zero must still land in [0, 360) (fmod keeps the sign).
    EXPECT_NEAR(Cinematic::AdvanceAngle(-10.0, 0.0, 1.0f), 350.0, 1e-9);
    const double a = Cinematic::AdvanceAngle(-370.0, 0.0, 1.0f);
    EXPECT_GE(a, 0.0);
    EXPECT_LT(a, 360.0);
}

// ---------------------------------------------------------------------------
// OrbitPosition
// ---------------------------------------------------------------------------

TEST(CinematicCamera, OrbitPositionCardinalAngles) {
    const float R = 32.0f, h = 14.0f;
    const glm::vec3 p0 = Cinematic::OrbitPosition(0.0, R, h);
    EXPECT_NEAR(p0.x, R, kEps);  EXPECT_NEAR(p0.y, h, kEps);  EXPECT_NEAR(p0.z, 0.0f, kEps);

    const glm::vec3 p90 = Cinematic::OrbitPosition(90.0, R, h);
    EXPECT_NEAR(p90.x, 0.0f, kEps);  EXPECT_NEAR(p90.y, h, kEps);  EXPECT_NEAR(p90.z, R, kEps);

    const glm::vec3 p180 = Cinematic::OrbitPosition(180.0, R, h);
    EXPECT_NEAR(p180.x, -R, kEps);  EXPECT_NEAR(p180.y, h, kEps);  EXPECT_NEAR(p180.z, 0.0f, kEps);

    const glm::vec3 p270 = Cinematic::OrbitPosition(270.0, R, h);
    EXPECT_NEAR(p270.x, 0.0f, kEps);  EXPECT_NEAR(p270.y, h, kEps);  EXPECT_NEAR(p270.z, -R, kEps);
}

TEST(CinematicCamera, OrbitPositionStaysOnCircleAndFinite) {
    const float R = 32.0f, h = 8.4f;
    // 360 deg matches 0 deg.
    const glm::vec3 p0 = Cinematic::OrbitPosition(0.0, R, h);
    const glm::vec3 p360 = Cinematic::OrbitPosition(360.0, R, h);
    EXPECT_NEAR(p0.x, p360.x, kEps);
    EXPECT_NEAR(p0.z, p360.z, kEps);

    // Arbitrary angle: stays exactly on the circle at height h, never NaN.
    for (double deg : { 17.0, 123.0, 271.5, -45.0 }) {
        const glm::vec3 p = Cinematic::OrbitPosition(deg, R, h);
        EXPECT_NEAR(std::sqrt(p.x * p.x + p.z * p.z), R, 1e-3f);
        EXPECT_NEAR(p.y, h, kEps);
        EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
    }
}

// ---------------------------------------------------------------------------
// IntroHeight
// ---------------------------------------------------------------------------

TEST(CinematicCamera, IntroHeightEndpointsAndHolds) {
    const float start = 21.0f, end = 8.4f;
    EXPECT_NEAR(Cinematic::IntroHeight(0.0, 6.0, start, end), start, kEps);
    EXPECT_NEAR(Cinematic::IntroHeight(6.0, 6.0, start, end), end, kEps);
    // Holds at endHeight after the intro finishes (never keeps drifting).
    EXPECT_NEAR(Cinematic::IntroHeight(100.0, 6.0, start, end), end, kEps);
    // Midpoint: EaseInOut(0.5) == 0.5 -> exactly (start+end)/2.
    EXPECT_NEAR(Cinematic::IntroHeight(3.0, 6.0, start, end), (start + end) * 0.5f, kEps);
}

TEST(CinematicCamera, IntroHeightMonotonicAndBounded) {
    const float start = 21.0f, end = 8.4f;
    float prev = start;
    for (int i = 1; i <= 12; ++i) {
        const float h = Cinematic::IntroHeight(i * 0.5, 6.0, start, end);
        EXPECT_LE(h, prev + kEps);  // non-increasing as the intro settles
        prev = h;
    }
    EXPECT_GE(prev, end - kEps);  // never undershoots the target height
}

// ---------------------------------------------------------------------------
// At (full pose)
// ---------------------------------------------------------------------------

TEST(CinematicCamera, AtBuildsFullPose) {
    const glm::vec3 tgt(0.0f, 1.5f, 0.0f);
    // Angle 180 deg is the mode-transition glide endpoint: (-R, height, 0).
    const Cinematic::Pose pose = Cinematic::At(180.0, 32.0f, 21.0f, tgt);
    EXPECT_NEAR(pose.position.x, -32.0f, kEps);
    EXPECT_NEAR(pose.position.y, 21.0f, kEps);
    EXPECT_NEAR(pose.position.z, 0.0f, kEps);
    EXPECT_EQ(pose.target, tgt);
}
