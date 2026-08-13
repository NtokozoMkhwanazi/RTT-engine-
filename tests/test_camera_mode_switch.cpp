/**
 * Camera Mode Switch Transition Tests
 *
 * Verifies CameraSwitch::DestinationPose - the math that glides the camera to
 * each mode's destination pose when switching modes (hotkeys 1-5, toolbar,
 * Camera menu). Pure math tests, no GL or ECS required.
 *
 * Run with: make test
 */

#include <gtest/gtest.h>
#include "cameraSystem/CameraModeSwitch.h"
#include <glm/glm.hpp>

using CameraSwitch::Pose;

namespace {

constexpr float kEps = 1e-4f;

// Mode ids match Render::CameraMode: 0 FreeFly, 1 ThirdPerson, 2 FirstPerson,
// 3 Orbit, 4 Cinematic.
constexpr int kFreeFly = 0;
constexpr int kThirdPerson = 1;
constexpr int kFirstPerson = 2;
constexpr int kOrbit = 3;
constexpr int kCinematic = 4;

} // namespace

TEST(CameraModeSwitch, FreeFlyKeepsCurrentPose) {
    const Pose current{glm::vec3(3.0f, 7.0f, -2.0f), glm::vec3(1.0f, 1.0f, 1.0f)};
    const Pose dest = CameraSwitch::DestinationPose(kFreeFly, current, -90.0f, 20.0f,
                                                    glm::vec3(0.0f), false, Pose{}, 1.0f, 50.0f);
    EXPECT_EQ(dest.position, current.position);
    EXPECT_EQ(dest.target, current.target);
}

TEST(CameraModeSwitch, FreeFlyReaimsAtTargetWhenProvided) {
    // With a focus target (character/selection), FreeFly keeps its eye but
    // re-aims the view at it, so the free camera enters focused on the subject.
    const Pose current{glm::vec3(3.0f, 7.0f, -2.0f), glm::vec3(1.0f, 1.0f, 1.0f)};
    const glm::vec3 entityPos(2.0f, 1.0f, 2.0f);
    const Pose dest = CameraSwitch::DestinationPose(kFreeFly, current, -90.0f, 20.0f,
                                                    entityPos, true, Pose{}, 1.0f, 50.0f);
    EXPECT_EQ(dest.position, current.position);  // the eye stays put
    EXPECT_EQ(dest.target, entityPos);           // the view re-aims at the target
}

TEST(CameraModeSwitch, OrbitKeepsCurrentPose) {
    const Pose current{glm::vec3(10.0f, 5.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)};
    const Pose dest = CameraSwitch::DestinationPose(kOrbit, current, -90.0f, 0.0f,
                                                    glm::vec3(0.0f), false, Pose{}, 1.0f, 50.0f);
    EXPECT_EQ(dest.position, current.position);
    EXPECT_EQ(dest.target, current.target);
}

TEST(CameraModeSwitch, FirstPersonLooksAlongYawPitch) {
    const Pose current{glm::vec3(5.0f, 5.0f, 5.0f), glm::vec3(0.0f)};
    // yaw=0, pitch=0 -> forward = (0, 0, 1); target must sit in front of the eye.
    const Pose dest = CameraSwitch::DestinationPose(kFirstPerson, current, 0.0f, 0.0f,
                                                    glm::vec3(0.0f), false, Pose{}, 1.0f, 50.0f);
    EXPECT_EQ(dest.position, current.position);
    ASSERT_NEAR(dest.target.x, 5.0f, kEps);
    ASSERT_NEAR(dest.target.y, 5.0f, kEps);
    ASSERT_NEAR(dest.target.z, 6.0f, kEps);
}

TEST(CameraModeSwitch, FirstPersonPitchTiltsLookDirection) {
    const Pose current{glm::vec3(0.0f), glm::vec3(0.0f)};
    // pitch=45, yaw=0 -> forward = (0, sin45, cos45)
    const Pose dest = CameraSwitch::DestinationPose(kFirstPerson, current, 0.0f, 45.0f,
                                                    glm::vec3(0.0f), false, Pose{}, 1.0f, 50.0f);
    glm::vec3 fwd = dest.target - dest.position;
    ASSERT_NEAR(glm::length(fwd), 1.0f, kEps);
    ASSERT_NEAR(fwd.x, 0.0f, kEps);
    ASSERT_NEAR(fwd.y, glm::sin(glm::radians(45.0f)), kEps);
    ASSERT_NEAR(fwd.z, glm::cos(glm::radians(45.0f)), kEps);
}

TEST(CameraModeSwitch, ThirdPersonRetargetsSelectedEntity) {
    const Pose current{glm::vec3(10.0f, 5.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)};
    const glm::vec3 entityPos(2.0f, 1.0f, 2.0f);
    const Pose dest = CameraSwitch::DestinationPose(kThirdPerson, current, -90.0f, 0.0f,
                                                    entityPos, true, Pose{}, 1.0f, 50.0f);
    EXPECT_EQ(dest.target, entityPos);

    // Direction from target to camera must match the old pose's direction.
    const glm::vec3 oldDir = glm::normalize(current.position - current.target);
    const glm::vec3 newDir = glm::normalize(dest.position - dest.target);
    ASSERT_NEAR(glm::length(oldDir - newDir), 0.0f, kEps);

    // Distance must be preserved (and clamped within [min, max]).
    const float oldDist = glm::length(current.position - current.target);
    ASSERT_NEAR(glm::length(dest.position - dest.target), oldDist, 1e-3f);
}

TEST(CameraModeSwitch, ThirdPersonWithoutEntityKeepsTargetAndClampsDistance) {
    const Pose current{glm::vec3(100.0f, 0.0f, 0.0f), glm::vec3(0.0f)};  // dist = 100
    const Pose dest = CameraSwitch::DestinationPose(kThirdPerson, current, 0.0f, 0.0f,
                                                    glm::vec3(0.0f), false, Pose{}, 1.0f, 50.0f);
    EXPECT_EQ(dest.target, current.target);  // no entity -> keep current target
    ASSERT_NEAR(glm::length(dest.position - dest.target), 50.0f, kEps);  // clamped to max
}

TEST(CameraModeSwitch, ThirdPersonClampsToMinDistance) {
    // Camera right next to the target (dist = 0.2) but minDist = 5: must push out to 5.
    const Pose current{glm::vec3(0.2f, 0.0f, 0.0f), glm::vec3(0.0f)};
    const Pose dest = CameraSwitch::DestinationPose(kThirdPerson, current, 0.0f, 0.0f,
                                                    glm::vec3(0.0f), false, Pose{}, 5.0f, 50.0f);
    EXPECT_EQ(dest.target, current.target);
    ASSERT_NEAR(glm::length(dest.position - dest.target), 5.0f, kEps);
}

TEST(CameraModeSwitch, FirstPersonNegativePitchLooksDown) {
    const Pose current{glm::vec3(0.0f), glm::vec3(0.0f)};
    // pitch=-45 -> forward = (0, -sin45, cos45): the eye looks downward.
    const Pose dest = CameraSwitch::DestinationPose(kFirstPerson, current, 0.0f, -45.0f,
                                                    glm::vec3(0.0f), false, Pose{}, 1.0f, 50.0f);
    glm::vec3 fwd = dest.target - dest.position;
    ASSERT_NEAR(glm::length(fwd), 1.0f, kEps);
    ASSERT_NEAR(fwd.y, glm::sin(glm::radians(-45.0f)), kEps);
    ASSERT_NEAR(fwd.z, glm::cos(glm::radians(-45.0f)), kEps);
}

TEST(CameraModeSwitch, ThirdPersonDegeneratePoseFallsBack) {
    // Camera sits exactly on the target: must not produce NaN.
    const Pose current{glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(2.0f, 2.0f, 2.0f)};
    const Pose dest = CameraSwitch::DestinationPose(kThirdPerson, current, 0.0f, 0.0f,
                                                    glm::vec3(0.0f), true, Pose{}, 1.0f, 50.0f);
    EXPECT_TRUE(std::isfinite(dest.position.x) && std::isfinite(dest.position.y) && std::isfinite(dest.position.z));
    EXPECT_TRUE(std::isfinite(dest.target.x) && std::isfinite(dest.target.y) && std::isfinite(dest.target.z));
    ASSERT_NEAR(glm::length(dest.position - dest.target), 8.0f, kEps);
}

TEST(CameraModeSwitch, CinematicGlidesToOrbitStart) {
    const Pose current{glm::vec3(1.0f, 1.0f, 1.0f), glm::vec3(0.0f)};
    const Pose orbitStart{glm::vec3(-32.0f, 21.0f, 0.0f), glm::vec3(0.0f, 1.5f, 0.0f)};
    const Pose dest = CameraSwitch::DestinationPose(kCinematic, current, 0.0f, 0.0f,
                                                    glm::vec3(0.0f), false, orbitStart, 1.0f, 50.0f);
    EXPECT_EQ(dest.position, orbitStart.position);
    EXPECT_EQ(dest.target, orbitStart.target);
}

TEST(CameraModeSwitch, EaseInOutEndpointsAndMonotonic) {
    EXPECT_FLOAT_EQ(CameraSwitch::EaseInOut(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(CameraSwitch::EaseInOut(1.0f), 1.0f);
    EXPECT_NEAR(CameraSwitch::EaseInOut(0.5f), 0.5f, kEps);
    // Monotonic and symmetric around t=0.5.
    EXPECT_LT(CameraSwitch::EaseInOut(0.25f), CameraSwitch::EaseInOut(0.5f));
    EXPECT_LT(CameraSwitch::EaseInOut(0.5f), CameraSwitch::EaseInOut(0.75f));
    EXPECT_NEAR(CameraSwitch::EaseInOut(0.25f), 1.0f - CameraSwitch::EaseInOut(0.75f), kEps);
    // Out-of-range inputs clamp.
    EXPECT_FLOAT_EQ(CameraSwitch::EaseInOut(-1.0f), 0.0f);
    EXPECT_FLOAT_EQ(CameraSwitch::EaseInOut(2.0f), 1.0f);
}
