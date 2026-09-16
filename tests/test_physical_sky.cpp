/**
 * PhysicalSky tests (suggestions.txt #4) — color model only.
 *
 * These assert the *physics contracts* of the pure-C++ sky model, not pixel
 * output: daylight is bright, night is dark, brightness is monotonic in
 * scattering angle toward the sun, zenith-illuminated sky is brighter than a
 * horizon sun, and turbidity increases haze. The scattering LUT/shader that
 * actually renders the sky is a deferred, GL-dependent follow-up and is NOT
 * asserted here (no render-pixel test in this repo).
 */
#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <vector>

#include "lighting/PhysicalSky.h"

namespace {
glm::vec3 rotY(const glm::vec3& v, float deg) {
    const float r = glm::radians(deg);
    const float c = std::cos(r), s = std::sin(r);
    return glm::normalize(glm::vec3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c));
}
} // namespace

TEST(PhysicalSky, DaylightZenithViewIsBright) {
    const glm::vec3 sun(0, 1, 0);                 // sun straight up
    const glm::vec3 up(0, 1, 0);                  // look straight up
    const float b = PhysicalSky::brightness(
        PhysicalSky::evaluate(up, sun, 2.0f, 1.0f));
    EXPECT_GT(b, 0.5f) << "overhead sun at zenith must be clearly lit";
}

TEST(PhysicalSky, NightIsDark) {
    const glm::vec3 sunBelow(0, -1, 0);           // sun below the horizon
    const glm::vec3 up(0, 1, 0);
    // Looking any direction should be near-black when the sun is down.
    const float b = PhysicalSky::brightness(
        PhysicalSky::evaluate(up, sunBelow, 2.0f, 1.0f));
    EXPECT_LT(b, 0.02f) << "night (sun below horizon) must be dark";
}

TEST(PhysicalSky, BrightnessMonotonicFromSunOutward) {
    const glm::vec3 sun(0, 1, 0);
    // Sweep the view direction away from the sun (around the azimuth) and
    // assert brightness never increases as scattering angle gamma grows.
    float prev = PhysicalSky::brightness(PhysicalSky::evaluate(sun, sun, 2.0f, 1.0f));
    for (int deg = 0; deg <= 90; deg += 15) {
        const glm::vec3 v = rotY(glm::vec3(0, 1, 0), float(deg)); // rotate away in AZ
        // keep v near zenith so we stay above the horizon
        const float b = PhysicalSky::brightness(PhysicalSky::evaluate(v, sun, 2.0f, 1.0f));
        EXPECT_LE(b, prev + 1e-4f) << "brightness must not increase as gamma grows (deg=" << deg << ")";
        prev = b;
    }
}

TEST(PhysicalSky, OverheadSunBrighterThanHorizonSun) {
    const glm::vec3 up(0, 1, 0);
    const float withOverhead = PhysicalSky::brightness(
        PhysicalSky::evaluate(up, glm::vec3(0, 1, 0), 2.0f, 1.0f));   // sun up
    const float withHorizon  = PhysicalSky::brightness(
        PhysicalSky::evaluate(up, glm::vec3(1, 0, 0), 2.0f, 1.0f));   // sun on horizon
    EXPECT_GT(withOverhead, withHorizon);
}

TEST(PhysicalSky, HigherTurbidityIncreasesHazeOffSun) {
    // 30 deg off the (overhead) sun, away from the direct disk: Mie is the
    // dominant term and scales with turbidity.
    const glm::vec3 sun(0, 1, 0);
    // ~45 deg off the overhead sun, away from the direct disk (disk term is
    // pow(cosGamma,64) -> negligible at 45 deg, so Mie dominates here).
    const glm::vec3 viewOff = glm::normalize(glm::vec3(0, 1, -1));
    const float clear = PhysicalSky::brightness(PhysicalSky::evaluate(viewOff, sun, 2.0f, 1.0f));
    const float hazy  = PhysicalSky::brightness(PhysicalSky::evaluate(viewOff, sun, 32.0f, 1.0f));
    EXPECT_GT(hazy, clear) << "more turbidity must brighten the off-sun haze";
}

TEST(PhysicalSky, ToneMappedColorStaysInUnitRange) {
    const glm::vec3 sun(0, 1, 0);
    const glm::vec3 c = PhysicalSky::skyColor(glm::vec3(0, 1, -1), sun, 2.0f, 1.0f);
    EXPECT_GE(c.r, 0.0f); EXPECT_LE(c.r, 1.0f);
    EXPECT_GE(c.g, 0.0f); EXPECT_LE(c.g, 1.0f);
    EXPECT_GE(c.b, 0.0f); EXPECT_LE(c.b, 1.0f);
}
