/**
 * LightingEnvironment temporal-interpolation unit tests.
 *
 * Verifies suggestion #6: a cvar hot-reload sets a private TARGET snapshot, the
 * first applyCvars() snaps live==target (instant startup, no fade), and
 * update(dt) blends the live fields toward the target at a frame-rate-
 * independent rate. LightingEnvironment is GL-free, so these run without a
 * context.
 */
#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cmath>

#include "lighting/LightingEnvironment.h"
#include "lighting/CVar.h"

namespace {
// Snapshot/restore the handful of shared singleton cvars this test mutates, so
// it can't pollute (or be polluted by) other tests in the same process.
struct CVarSnapshot {
    bool      hadSunIntensity;
    float     sunIntensity;
    bool      hadSunDir;
    glm::vec3 sunDir;
    void save() {
        hadSunIntensity = CVar::Instance().has("sun_intensity");
        sunIntensity    = CVar::Instance().getFloat("sun_intensity", 1.0f);;
        hadSunDir       = CVar::Instance().has("sun_direction");
        sunDir          = CVar::Instance().getVec3("sun_direction",
                                            glm::vec3(0.5f, 1.0f, 0.3f));
    }
    void restore() {
        if (hadSunIntensity) CVar::Instance().setFloat("sun_intensity", sunIntensity);
        else                 CVar::Instance().setFloat("sun_intensity", 1.0f);
        CVar::Instance().setVec3("sun_direction",
            hadSunDir ? sunDir : glm::vec3(0.5f, 1.0f, 0.3f));
    }
};
} // namespace

TEST(LightingEnvTemporal, FirstApplySnapsInstantlyNoStartupFade) {
    CVarSnapshot snap; snap.save();
    CVar::Instance().setFloat("sun_intensity", 4.0f);

    LightingEnvironment env;                  // fresh: live == shipped defaults
    env.applyCvars();                         // first apply -> snap
    EXPECT_FLOAT_EQ(env.sunIntensity, 4.0f);  // live == target immediately
    EXPECT_FALSE(glm::any(glm::notEqual(
        env.sunDirection, glm::normalize(glm::vec3(0.5f, 1.0f, 0.3f)))));

    snap.restore();
}

TEST(LightingEnvTemporal, ReloadSetsTargetOnly_LiveUntouchedUntilUpdate) {
    CVarSnapshot snap; snap.save();

    LightingEnvironment env;
    CVar::Instance().setFloat("sun_intensity", 1.0f);
    env.applyCvars();                         // snap: live == 1.0
    ASSERT_FLOAT_EQ(env.sunIntensity, 1.0f);

    CVar::Instance().setFloat("sun_intensity", 9.0f);
    env.applyCvars();                         // reload: target==9, live stays 1.0
    EXPECT_FLOAT_EQ(env.sunIntensity, 1.0f);  // NOT snapped on reload

    // A single 60Hz tick should move live toward 9 but not reach it.
    env.update(1.0f / 60.0f);
    EXPECT_GT(env.sunIntensity, 1.0f);
    EXPECT_LT(env.sunIntensity, 9.0f);

    snap.restore();
}

TEST(LightingEnvTemporal, UpdateConvergesToTarget) {
    CVarSnapshot snap; snap.save();

    LightingEnvironment env;
    CVar::Instance().setFloat("sun_intensity", 0.0f);
    env.applyCvars();                         // snap: live == 0.0
    CVar::Instance().setFloat("sun_intensity", 1.0f);
    env.applyCvars();                         // target == 1.0

    // At ~8 units/sec, a full second of 60Hz ticks converges to ~0.9997 (well
    // within 1e-3 of the target) with no overshoot.
    for (int i = 0; i < 60; ++i) env.update(1.0f / 60.0f);
    EXPECT_NEAR(env.sunIntensity, 1.0f, 1e-3f);
    EXPECT_LE(env.sunIntensity, 1.0f + 1e-6f); // never overshoots

    // A giant dt snaps exactly to target (t -> 1, fast path).
    env.update(100.0f);
    EXPECT_FLOAT_EQ(env.sunIntensity, 1.0f);

    snap.restore();
}

TEST(LightingEnvTemporal, Vec3SunDirectionFadesAndStaysNormalized) {
    CVarSnapshot snap; snap.save();

    LightingEnvironment env;
    glm::vec3 a = glm::normalize(glm::vec3(0.2f, 0.9f, 0.2f));
    glm::vec3 b = glm::normalize(glm::vec3(1.0f, 0.0f, 0.0f));
    CVar::Instance().setVec3("sun_direction", a);
    env.applyCvars();                         // snap to A
    ASSERT_FLOAT_EQ(glm::length(env.sunDirection), 1.0f);

    CVar::Instance().setVec3("sun_direction", b);
    env.applyCvars();                         // target B, live stays A

    // After one tick it has begun moving from A toward B.
    env.update(1.0f / 60.0f);
    EXPECT_GT(glm::dot(env.sunDirection, b), glm::dot(a, b) - 1e-9f);

    // A full second of fading lands on B, still unit-length.
    for (int i = 0; i < 59; ++i) env.update(1.0f / 60.0f);
    EXPECT_LT(glm::distance(env.sunDirection, b), 1e-2f);
    EXPECT_FLOAT_EQ(glm::length(env.sunDirection), 1.0f);

    snap.restore();
}
