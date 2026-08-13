/**
 * Camera NaN Regression Tests
 *
 * Regression coverage for a production bug where the editor created the
 * flyCamera with up=(0,0,0). A zero WorldUp makes normalize(cross(front, up))
 * produce NaN, poisoning the view matrix and silently discarding every 3D
 * draw (skybox, terrain, models) with no GL error while ImGui kept rendering
 * (the "skybox replaced by the clear color" symptom).
 *
 * flyCamera is header-only, so these tests need no engine objects.
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <limits>

#include "../cameraSystem/flyCamera.h"

namespace {

bool hasNaN(const glm::mat4& m) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::isnan(m[c][r])) return true;
    return false;
}

bool allFinite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace

// The exact scenario that caused the bug: up vector passed as (0,0,0).
// The guard in flyCamera must fall back to +Y so the view matrix stays finite.
TEST(CameraNanRegression, ZeroUpVector_ViewMatrixIsFinite) {
    flyCamera cam(glm::vec3(0.0f, 5.0f, 10.0f), glm::vec3(0.0f, 0.0f, 0.0f), -90.0f, -20.0f, 10.0f);

    glm::mat4 view = cam.GetViewMatrix();
    EXPECT_FALSE(hasNaN(view)) << "Zero up vector must not produce a NaN view matrix";

    // Camera basis vectors must also be finite (they feed the view matrix).
    EXPECT_TRUE(allFinite(cam.Up));
    EXPECT_TRUE(allFinite(cam.Right));
    EXPECT_TRUE(allFinite(cam.Position));
}

// Same bug via direct WorldUp assignment (simulates external mutation).
TEST(CameraNanRegression, ZeroWorldUpAfterAssignment_ViewMatrixIsFinite) {
    flyCamera cam(glm::vec3(0.0f, 2.0f, 10.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    cam.WorldUp = glm::vec3(0.0f, 0.0f, 0.0f);
    cam.ProcessMouseMovement(10.0f, 5.0f);  // triggers updateCameraVectors

    EXPECT_FALSE(hasNaN(cam.GetViewMatrix())) << "Degenerate WorldUp must be self-healed";
    EXPECT_TRUE(allFinite(cam.Up));
}

// A normal camera must keep producing finite matrices across many mouse updates
// (rotation must never degrade into NaN through cumulative updates).
TEST(CameraNanRegression, ManyMouseUpdates_NeverNaN) {
    flyCamera cam(glm::vec3(0.0f, 5.0f, 10.0f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, -20.0f, 10.0f);
    for (int i = 0; i < 1000; ++i) {
        cam.ProcessMouseMovement((i % 3) * 0.5f - 0.5f, (i % 5) * 0.3f - 0.6f);
        EXPECT_FALSE(hasNaN(cam.GetViewMatrix())) << "NaN appeared at iteration " << i;
        if (HasFatalFailure()) return;
    }
    // Pitch clamping must keep the camera from flipping over the poles.
    for (int i = 0; i < 200; ++i) cam.ProcessMouseMovement(0.0f, 50.0f);
    EXPECT_FALSE(hasNaN(cam.GetViewMatrix()));
    EXPECT_GE(cam.Pitch, -89.5f);
    EXPECT_LE(cam.Pitch, 89.5f);
}

// The RepositionOrbit helper (used by Orbit/ThirdPerson modes) must also
// produce finite state after panning the target.
TEST(CameraNanRegression, RepositionOrbit_AfterTargetPan_IsFinite) {
    flyCamera cam(glm::vec3(0.0f, 5.0f, 10.0f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, -20.0f, 10.0f);
    cam.Target += glm::vec3(3.0f, 1.0f, -2.0f);
    cam.RepositionOrbit();
    EXPECT_FALSE(hasNaN(cam.GetViewMatrix()));
    EXPECT_TRUE(allFinite(cam.Position));
}
