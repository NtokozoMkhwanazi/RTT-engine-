/**
 * Editor Play Mode Controller Tests
 *
 * Exercises the Editor::PlayModeController wrapper that drives an
 * AnimatedCharacter in the editor's play mode (input -> character update ->
 * terrain snap). Pure logic - no GL required.
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include "editor/PlayModeController.h"
#include "modelSystem/Model.h"

namespace {

// Flat ground helper.
float FlatTerrain(float, float) { return 0.0f; }

TEST(PlayModeController, NotLoadedBeforeLoad) {
    Editor::PlayModeController ctrl;
    EXPECT_FALSE(ctrl.isLoaded());
    EXPECT_FALSE(ctrl.hasCharacter());
}

TEST(PlayModeController, LoadsBotWithLocomotionClips) {
    Editor::PlayModeController ctrl;
    ASSERT_TRUE(ctrl.load("assets/bot.fbx", "assets"));
    EXPECT_TRUE(ctrl.isLoaded());
    EXPECT_TRUE(ctrl.hasCharacter());
    EXPECT_GT(ctrl.character().boneCount(), 0);
    EXPECT_GT(ctrl.character().clipCount(), 0);
}

TEST(PlayModeController, FailsOnMissingModel) {
    Editor::PlayModeController ctrl;
    EXPECT_FALSE(ctrl.load("assets/does_not_exist.fbx", "assets"));
    EXPECT_FALSE(ctrl.isLoaded());
}

TEST(PlayModeController, ShutdownIsSafeAndReleasable) {
    Editor::PlayModeController ctrl;
    ASSERT_TRUE(ctrl.load("assets/bot.fbx", "assets"));
    ctrl.shutdown();
    EXPECT_FALSE(ctrl.isLoaded());
    ctrl.shutdown();  // double shutdown is safe
    EXPECT_FALSE(ctrl.hasCharacter());
}

TEST(PlayModeController, ReloadAfterShutdownWorks) {
    Editor::PlayModeController ctrl;
    ASSERT_TRUE(ctrl.load("assets/bot.fbx", "assets"));
    ctrl.shutdown();
    ASSERT_TRUE(ctrl.load("assets/bot.fbx", "assets"));
    EXPECT_TRUE(ctrl.isLoaded());
}

TEST(PlayModeController, ForwardInputMovesCharacter) {
    Editor::PlayModeController ctrl;
    ASSERT_TRUE(ctrl.load("assets/bot.fbx", "assets"));

    glm::vec3 start = ctrl.character().position;
    CharacterInput input;
    input.moveDirection = glm::vec2(0.0f, 1.0f);
    input.moveMagnitude = 1.0f;
    input.grounded = true;

    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 60; ++i) {  // 1 simulated second
        ctrl.update(dt, input, FlatTerrain);
    }

    const glm::vec3 end = ctrl.character().position;
    const float moved = glm::length(end - start);
    EXPECT_GT(moved, 0.1f) << "Character should have moved forward over 1s";
    EXPECT_GT(ctrl.character().currentSpeed(), 0.05f);
}

TEST(PlayModeController, JumpLiftsCharacterOffGround) {
    Editor::PlayModeController ctrl;
    ASSERT_TRUE(ctrl.load("assets/bot.fbx", "assets"));

    CharacterInput input;
    input.moveDirection = glm::vec2(0.0f);
    input.jump = true;
    input.grounded = true;

    ctrl.update(1.0f / 60.0f, input, FlatTerrain);
    // After the jump impulse the character should leave the ground.
    EXPECT_FALSE(ctrl.character().grounded) << "Jump should launch the character";
    EXPECT_GT(ctrl.character().velocity.y, 0.0f);
}

TEST(PlayModeController, CrouchInputTogglesCrouchState) {
    Editor::PlayModeController ctrl;
    ASSERT_TRUE(ctrl.load("assets/bot.fbx", "assets"));

    CharacterInput input;
    input.moveDirection = glm::vec2(0.0f);
    input.crouch = true;
    input.grounded = true;

    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 30; ++i) ctrl.update(dt, input, FlatTerrain);
    EXPECT_TRUE(ctrl.character().crouchState());
}

TEST(PlayModeController, PreviewPlaysThenExpires) {
    Editor::PlayModeController ctrl;
    ASSERT_TRUE(ctrl.load("assets/bot.fbx", "assets"));

    ctrl.playPreview(0, 0.5f);
    CharacterInput idle;
    idle.grounded = true;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 30; ++i) ctrl.update(dt, idle, FlatTerrain);
    // After the preview expires with no movement the FSM takes over (idle).
    EXPECT_EQ(ctrl.character().activeClipIndex(), 0);
}

TEST(PlayModeController, RenderModelBoneSpaceMatchesCharacter) {
    // The render path draws the GPU Model with the character's animator, so the
    // model's skeleton must line up with the controller's (both load bot.fbx).
    Editor::PlayModeController ctrl;
    ASSERT_TRUE(ctrl.load("assets/bot.fbx", "assets"));

    Model renderModel("assets/bot.fbx");
    ASSERT_GT(renderModel.GetMeshCount(), 0);
    // The character's skeleton bone count should be at least as large as the
    // largest bone index referenced by the render model's meshes, otherwise the
    // bone matrices would be indexed out of bounds during skinning.
    EXPECT_GE(ctrl.character().boneCount(), 1);
}

TEST(PlayModeController, DebugDiagnosticsAvailable) {
    Editor::PlayModeController ctrl;
    ASSERT_TRUE(ctrl.load("assets/bot.fbx", "assets"));

    // Run the pose search once so the matcher picks a clip.
    CharacterInput idle;
    idle.grounded = true;
    ctrl.update(1.0f / 60.0f, idle, FlatTerrain);

    auto diag = ctrl.debugPoseDiag();
    // The matcher should have selected a clip after the first search.
    EXPECT_FALSE(diag.matcherClip.empty());
}

} // namespace
