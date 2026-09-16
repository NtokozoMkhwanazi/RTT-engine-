/**
 * EditorApplication world-update camera position tests.
 *
 * The terrain streams chunks and computes LOD around the position fed to
 * WorldManager::update(). That position must follow the play-mode follow
 * camera, not the world origin - otherwise the world appears to slide/pop
 * under the character as it walks away from the origin. Pure logic - no GL.
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <cmath>

#include "editor/editor_application.h"
#include "editor/PlayModeController.h"
#include "editor/AnimatedCharacter.h"
#include "cameraSystem/ThirdPersonCamera.h"

namespace {

TEST(EditorApplication, WorldUpdateUsesOriginWhenCharacterNotLoaded) {
    // Before the character is loaded there is no follow camera to track, so
    // the world centers on the origin (previous behavior).
    ThirdPersonCamera cam;
    cam.setPosition(glm::vec3(12.0f, 3.0f, -5.0f));
    EXPECT_EQ(Editor::EditorApplication::worldUpdateCameraPos(false, cam),
              glm::vec3(0.0f));
}

TEST(EditorApplication, WorldUpdateTracksFollowCameraWhenLoaded) {
    // With the character loaded, terrain streaming/LOD must center on the
    // follow camera's position instead of the world origin.
    ThirdPersonCamera cam;
    cam.setPosition(glm::vec3(12.0f, 3.0f, -5.0f));
    EXPECT_EQ(Editor::EditorApplication::worldUpdateCameraPos(true, cam),
              cam.position);
}

TEST(EditorApplication, WorldUpdateTracksLiveCameraPosition) {
    // The position must be read live each frame (the camera moved last frame
    // and its new position is what the terrain should stream around).
    ThirdPersonCamera cam;
    cam.setPosition(glm::vec3(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(Editor::EditorApplication::worldUpdateCameraPos(true, cam),
              cam.position);

    cam.setPosition(glm::vec3(-40.0f, 7.5f, 120.0f));
    EXPECT_EQ(Editor::EditorApplication::worldUpdateCameraPos(true, cam),
              cam.position);
}

TEST(EditorApplication, FollowCameraTracksFinalSnappedPositionWhileWalking) {
    // Simulates the editor's play-mode frame order exactly as the fixed
    // pipeline prescribes: (1) character update with physics/terrain-snap,
    // then (2) the follow camera reads the character's FINAL position.
    // Verifies while the bot walks over sloped terrain that:
    //   - the bot's feet stay glued to the terrain surface every frame
    //   - the camera's look target converges to the final snapped position
    //   - the camera keeps a stable follow distance (no rubber-banding)
    Editor::PlayModeController ctrl;
    ASSERT_TRUE(ctrl.load("assets/bot.fbx", "assets"));

    // Sloped ground: height rises with +X so walking exercises terrain snap.
    auto terrain = [](float x, float) -> float { return 3.0f + 0.01f * x; };

    CharacterInput in;
    in.moveDirection = glm::vec2(0.0f, 1.0f);
    in.moveMagnitude = 1.0f;
    in.grounded = true;

    const float dt = 1.0f / 60.0f;
    ThirdPersonCamera cam;
    cam.groundHeightFn = terrain;

    float lastTargetErr = 1e9f;
    float lastDistErr = 1e9f;
    float maxTargetErrLast60 = 0.0f;
    for (int i = 0; i < 240; ++i) {  // 4 simulated seconds of walking
        ctrl.update(dt, in, terrain);
        const AnimatedCharacter& cc = ctrl.character();

        // Feet glued to the terrain surface (the world never slides under
        // the bot: its vertical position IS the floor under it).
        EXPECT_NEAR(cc.position.y, terrain(cc.position.x, cc.position.z), 1e-3f);

        // Phase 2: camera reads the FINAL position, not a pre-physics one.
        CameraInput cin;
        cin.characterPosition = cc.position;
        cin.characterVelocity = cc.velocity;
        cin.moveMagnitude = cc.currentSpeed();
        cin.isGrounded = cc.grounded;
        cin.animState = CameraState::WALK;
        cam.update(dt, cin, 16.0f / 9.0f);

        // Camera look target = bot chest at its final snapped position.
        lastTargetErr = glm::length(
            cam.target - (cc.position + glm::vec3(0.0f, cam.config.pivotHeight, 0.0f)));
        // Camera holds the configured follow distance.
        lastDistErr = std::abs(glm::length(cam.position - cc.position) - cam.config.distance);
        if (i >= 180) maxTargetErrLast60 = std::max(maxTargetErrLast60, lastTargetErr);
    }

    // An exponential follow camera chasing a moving target carries the
    // designed smoothing lag (speed / pivotSmooth) - NOT a stale-position
    // read. The error must be bounded by that model, stable over the last
    // second (no rubber-banding oscillation), and the follow distance must
    // hold steady.
    const float speed = ctrl.character().currentSpeed();
    const float chaseLag = speed / cam.config.pivotSmooth;
    EXPECT_LT(lastTargetErr, chaseLag + 0.15f)
        << "Camera must track the final snapped position within the designed "
           "chase lag";
    EXPECT_LT(maxTargetErrLast60, chaseLag + 0.15f)
        << "Chase lag must stay bounded while walking (no rubber-banding)";
    EXPECT_LT(lastDistErr, 0.5f) << "Camera follow distance must stay stable";
}

TEST(EditorApplication, FollowCameraStaysBehindDuringTurnaround) {
    // The user-facing contract: the follow camera must always sit BEHIND the
    // bot. Walk forward, then flip the input 180 degrees so the bot turns
    // around and walks back toward the camera - the camera must orbit around
    // to the new back (never stay in front of it).
    Editor::PlayModeController ctrl;
    ASSERT_TRUE(ctrl.load("assets/bot.fbx", "assets"));
    auto terrain = [](float, float) -> float { return 0.0f; };

    const float dt = 1.0f / 60.0f;
    ThirdPersonCamera cam;
    cam.config.orientToCharacterForward = true;
    cam.groundHeightFn = terrain;

    auto frame = [&](const glm::vec2& dir) {
        CharacterInput in;
        in.moveDirection = dir;
        in.moveMagnitude = 1.0f;
        in.grounded = true;
        ctrl.update(dt, in, terrain);
        const AnimatedCharacter& cc = ctrl.character();
        CameraInput cin;
        cin.characterPosition = cc.position;
        cin.characterVelocity = cc.velocity;
        cin.moveMagnitude = cc.currentSpeed();
        cin.isGrounded = cc.grounded;
        cin.characterForward =
            glm::vec3(-std::sin(cc.heading), 0.0f, -std::cos(cc.heading));
        cin.animState = CameraState::RUN;
        cam.update(dt, cin, 16.0f / 9.0f);
    };

    // Camera->bot direction dotted with the bot's forward: +1 = perfectly
    // behind, -1 = perfectly in front.
    auto behindness = [&]() {
        const AnimatedCharacter& cc = ctrl.character();
        const glm::vec3 fwd(-std::sin(cc.heading), 0.0f, -std::cos(cc.heading));
        glm::vec3 camToBot = cc.position - cam.position;
        camToBot.y = 0.0f;
        if (glm::length(camToBot) < 1e-4f) return 0.0f;
        return glm::dot(glm::normalize(camToBot), fwd);
    };

    // Phase 1: walk forward (faces -Z); camera settles behind the bot's +Z
    // side (relative position - the bot itself moves).
    for (int i = 0; i < 180; ++i) frame(glm::vec2(0.0f, -1.0f));
    EXPECT_GT(cam.position.z - ctrl.character().position.z, 2.0f)
        << "Camera must sit behind the walking bot";

    float minBehindWalk = 1.0f;
    for (int i = 0; i < 180; ++i) {
        frame(glm::vec2(0.0f, -1.0f));
        if (i >= 60) minBehindWalk = std::min(minBehindWalk, behindness());
    }
    EXPECT_GT(minBehindWalk, 0.5f) << "Camera must stay behind while walking";

    // Phase 2: turn 180 degrees (walk back toward the camera). The camera
    // must swing around and settle behind the bot again - it may pass around
    // the side mid-swing, but must end up behind and never stay in front.
    for (int i = 0; i < 240; ++i) frame(glm::vec2(0.0f, 1.0f));
    EXPECT_GT(ctrl.character().position.z - cam.position.z, 2.0f)
        << "Camera must end behind the turned bot (opposite side)";

    float minBehindTurn = 1.0f;
    for (int i = 0; i < 180; ++i) {
        frame(glm::vec2(0.0f, 1.0f));
        if (i >= 60) minBehindTurn = std::min(minBehindTurn, behindness());
    }
    EXPECT_GT(minBehindTurn, 0.5f)
        << "Camera must stay behind the bot after the turnaround";
}

TEST(EditorApplication, FollowCameraTracksCorneringPath) {
    // Scripted L-shaped path with two 90-degree turns: the camera must orbit
    // through every corner and never end up in front of the bot, staying
    // locked behind it on each straight leg.
    Editor::PlayModeController ctrl;
    ASSERT_TRUE(ctrl.load("assets/bot.fbx", "assets"));
    auto terrain = [](float, float) -> float { return 0.0f; };

    const float dt = 1.0f / 60.0f;
    ThirdPersonCamera cam;
    cam.config.orientToCharacterForward = true;
    cam.groundHeightFn = terrain;

    auto frame = [&](const glm::vec2& dir) {
        CharacterInput in;
        in.moveDirection = dir;
        in.moveMagnitude = 1.0f;
        in.grounded = true;
        ctrl.update(dt, in, terrain);
        const AnimatedCharacter& cc = ctrl.character();
        CameraInput cin;
        cin.characterPosition = cc.position;
        cin.characterVelocity = cc.velocity;
        cin.moveMagnitude = cc.currentSpeed();
        cin.isGrounded = cc.grounded;
        cin.characterForward =
            glm::vec3(-std::sin(cc.heading), 0.0f, -std::cos(cc.heading));
        cin.animState = CameraState::RUN;
        cam.update(dt, cin, 16.0f / 9.0f);
    };

    auto behindness = [&]() {
        const AnimatedCharacter& cc = ctrl.character();
        const glm::vec3 fwd(-std::sin(cc.heading), 0.0f, -std::cos(cc.heading));
        glm::vec3 camToBot = cc.position - cam.position;
        camToBot.y = 0.0f;
        if (glm::length(camToBot) < 1e-4f) return 0.0f;
        return glm::dot(glm::normalize(camToBot), fwd);
    };

    // Three legs with two 90-degree left turns: faces -Z, then -X, then +Z.
    const glm::vec2 legs[3] = {glm::vec2(0.0f, -1.0f),
                               glm::vec2(-1.0f, 0.0f),
                               glm::vec2(0.0f, 1.0f)};
    float minBehind = 1.0f;
    float minBehindStraight = 1.0f;
    for (int leg = 0; leg < 3; ++leg) {
        for (int i = 0; i < 150; ++i) {
            frame(legs[leg]);
            if (i >= 30) minBehind = std::min(minBehind, behindness());
            if (i >= 60) minBehindStraight = std::min(minBehindStraight, behindness());
        }
    }

    EXPECT_GT(minBehindStraight, 0.5f)
        << "Camera must stay behind the bot on every straight leg";
    EXPECT_GT(minBehind, 0.3f)
        << "Camera must never pass in front of the bot through the corners";
}

} // namespace
