/**
 * Animated Character Tests (GL-free)
 *
 * Exercises the pure-logic surface of AnimatedCharacter - model/clip loading,
 * terrain snap, movement, jump physics and the locomotion FSM. render() touches
 * GL and is intentionally never called here.
 *
 * Run with: make test
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cmath>

#include "../editor/AnimatedCharacter.h"

namespace {

constexpr float kFlatTerrainY = 12.5f;

float FlatTerrain(float, float) { return kFlatTerrainY; }

CharacterInput IdleInput() {
    CharacterInput ci;
    ci.reset();
    return ci;
}

}  // namespace

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

TEST(AnimatedCharacter, LoadsBotAndNormalizesScale) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx")) << "bot.fbx must load";
    EXPECT_GT(cc.boneCount(), 0u);
    EXPECT_TRUE(std::isfinite(cc.scale));
    EXPECT_GT(cc.scale, 0.001f);
    EXPECT_LT(cc.scale, 100.0f);
    EXPECT_TRUE(cc.ready());
    // Scale should bring the raw model height down to ~1.8 world units.
    EXPECT_NEAR(cc.scale * cc.rawSizeY(), cc.targetHeight(), cc.targetHeight() * 0.1f)
        << "scaled character should be close to target height";
}

TEST(AnimatedCharacter, LoadsLocomotionClips) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    EXPECT_GE(cc.loadLocomotion("assets"), 1) << "at least one clip should register";
    EXPECT_GT(cc.clipCount(), 0);
    // With Idle registered, initialize() must land on a concrete state.
    EXPECT_NE(cc.state(), AnimationState::NONE);
    // The long 16.6s Idle ambient must be trimmed to ~5s so the motion
    // database doesn't waste ~1200 near-identical poses on it.
    if (cc.idleClipDuration() > 0.0f) {
        EXPECT_LE(cc.idleClipDuration(), 5.1f);
    }
}

TEST(AnimatedCharacter, MotionMatchingJumpDrivesAirborneArc) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");
    cc.motionMatchingEnabled = true;  // default; explicit for clarity
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);

    const float dt = 1.0f / 60.0f;

    // Grounded jump goes airborne; the legacy one-shot override must NOT
    // engage because the DB has Jump/Fall poses - the pose search drives the
    // arc instead (UE-style). The airborne flag is the new contract.
    CharacterInput jump = IdleInput();
    jump.jump = true;
    cc.update(dt, jump, FlatTerrain);
    EXPECT_TRUE(cc.mmAirborne()) << "grounded jump must leave the ground";
    EXPECT_FALSE(cc.mmJumping()) << "pose search must drive airborne (no timer override)";

    // Hold the arc: still airborne, matcher active, state stays finite.
    for (int i = 0; i < 30; ++i) cc.update(dt, IdleInput(), FlatTerrain);
    EXPECT_TRUE(cc.mmAirborne());
    EXPECT_TRUE(std::isfinite(cc.position.y));
    EXPECT_TRUE(std::isfinite(cc.animatorTime()));

    // The pose search must be driving an airborne clip (Jump/Fall), not
    // snapping back to Idle - the UE-style state gate is what makes this work.
    {
        const std::string clip = cc.mmActiveClip();
        EXPECT_TRUE(clip == "Jump" || clip == "Fall")
            << "airborne matcher must select a Jump/Fall pose, got '" << clip << "'";
    }

    // The arc resolves and the character lands back on the terrain.
    for (int i = 0; i < 240; ++i) cc.update(dt, IdleInput(), FlatTerrain);
    EXPECT_FALSE(cc.mmAirborne()) << "character must land";
    EXPECT_TRUE(std::isfinite(cc.position.y));
    EXPECT_TRUE(std::isfinite(cc.animatorTime()));
}

TEST(AnimatedCharacter, CrouchCapsSpeed) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");
    cc.motionMatchingEnabled = false;  // deterministic FSM speed path
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);

    CharacterInput ci = IdleInput();
    ci.moveDirection = glm::vec2(0.0f, 1.0f);
    ci.moveMagnitude = 1.0f;
    ci.crouch = true;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 120; ++i) cc.update(dt, ci, FlatTerrain);
    EXPECT_TRUE(cc.crouchState());
    EXPECT_LE(cc.currentSpeed(), 1.55f) << "crouch must cap top speed (~1.5)";
    EXPECT_GT(cc.currentSpeed(), 1.0f) << "crouch-walk should still move";
}

TEST(AnimatedCharacter, RestOverrideIdlesInMotionMatchingMode) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");
    cc.motionMatchingEnabled = true;  // default: MM drives, rest override applies
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);
    const float dt = 1.0f / 60.0f;

    // Standing still must play Idle (clip 0) - the pose search's root-speed-only
    // features tie Idle and Crouch at rest, so the rest override guarantees the
    // character starts (and stays) idle.
    for (int i = 0; i < 30; ++i) cc.update(dt, IdleInput(), FlatTerrain);
    EXPECT_EQ(cc.activeClipIndex(), 0) << "standing still must play Idle";
    EXPECT_EQ(cc.activeClipName(), "Idle");
}

TEST(AnimatedCharacter, ActiveClipIndexTracksFsmState) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");
    cc.motionMatchingEnabled = false;
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);
    const float dt = 1.0f / 60.0f;

    // Standing still: Idle (clip 0).
    for (int i = 0; i < 15; ++i) cc.update(dt, IdleInput(), FlatTerrain);
    EXPECT_EQ(cc.activeClipIndex(), 0);
    EXPECT_EQ(cc.activeClipName(), "Idle");

    // Walking: WALK (clip 1).
    CharacterInput ci = IdleInput();
    ci.moveDirection = glm::vec2(0.0f, 1.0f);
    ci.moveMagnitude = 1.0f;
    for (int i = 0; i < 60; ++i) cc.update(dt, ci, FlatTerrain);
    EXPECT_EQ(cc.activeClipIndex(), 1);
    EXPECT_EQ(cc.activeClipName(), "Walk");
}

TEST(AnimatedCharacter, PreviewClipPlaysAndMovementCancels) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");
    cc.motionMatchingEnabled = false;
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);
    const float dt = 1.0f / 60.0f;

    cc.playPreview(2, 2.0f);  // preview the Run clip
    for (int i = 0; i < 10; ++i) cc.update(dt, IdleInput(), FlatTerrain);
    EXPECT_EQ(cc.activeClipIndex(), 2) << "preview must play the requested clip";

    // Manual movement cancels the preview back to normal locomotion.
    CharacterInput ci = IdleInput();
    ci.moveDirection = glm::vec2(0.0f, 1.0f);
    ci.moveMagnitude = 1.0f;
    cc.update(dt, ci, FlatTerrain);
    EXPECT_NE(cc.activeClipIndex(), 2) << "movement must cancel the preview";
}

TEST(AnimatedCharacter, LoadClipFromFileLoadsAnimation) {
    Animation* clip = AnimatedCharacter::LoadClipFromFile("assets/Idle.fbx");
    if (!clip) {
        GTEST_SKIP() << "assets/Idle.fbx not present - skipping";
    }
    EXPECT_GT(clip->duration, 0.0f);
    // Regression guard: the loader used to store Assimp's tick count as the
    // duration (e.g. "499s" for a ~16s Idle clip). Real seconds must be well
    // under the tick values, so assert an upper bound. This is what lets the
    // MotionDatabase accept the clips (its guard rejects > 60s).
    EXPECT_LT(clip->duration, 30.0f);
    // Pin the KEY-TIME conversion too: the sampler interpolates against these
    // times, so they must be real seconds like the duration. A regression that
    // converts only one of the two would pass the duration check above while
    // silently breaking playback (all samples clamp to the first keyframe).
    const BoneAnimation* bone = clip->GetBoneAnimation("rightupleg");
    if (bone && !bone->rotationTimes.empty()) {
        EXPECT_NEAR(bone->rotationTimes.back(), clip->duration, 1.0f)
            << "last rotation key time must be ~duration (real seconds)";
        EXPECT_LT(bone->rotationTimes.back(), 30.0f)
            << "key times must not be raw tick counts";
    }
    EXPECT_FALSE(clip->name.empty());
    delete clip;
}

// ---------------------------------------------------------------------------
// Movement / physics
// ---------------------------------------------------------------------------

TEST(AnimatedCharacter, FeetSnapToTerrainHeight) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");

    cc.position = glm::vec3(10.0f, 0.0f, 10.0f);
    cc.update(1.0f / 60.0f, IdleInput(), FlatTerrain);
    EXPECT_NEAR(cc.position.y, kFlatTerrainY, 0.01f);
    EXPECT_TRUE(cc.grounded);
    // Standing still must not slide.
    EXPECT_NEAR(cc.position.x, 10.0f, 1e-3f);
    EXPECT_NEAR(cc.position.z, 10.0f, 1e-3f);
}

TEST(AnimatedCharacter, MovesForwardWithInput) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);

    CharacterInput ci = IdleInput();
    ci.moveDirection = glm::vec2(0.0f, -1.0f);  // forward (local -Z)
    ci.moveMagnitude = 1.0f;

    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 60; ++i) cc.update(dt, ci, FlatTerrain);

    EXPECT_LT(cc.position.z, -0.5f) << "character should move -Z";
    EXPECT_LT(std::fabs(cc.position.x), 0.1f) << "no lateral drift";
    EXPECT_NEAR(cc.position.y, kFlatTerrainY, 0.01f) << "feet stay on the terrain";
    // The character must face its movement direction. Local -Z is forward, so
    // facing -Z means heading settles near 0.
    const glm::vec3 fwd(-std::sin(cc.heading), 0.0f, -std::cos(cc.heading));
    EXPECT_LT(fwd.z, -0.9f) << "should face -Z (forward vector z)";
    EXPECT_LT(std::fabs(fwd.x), 0.1f) << "no lateral facing";
}

TEST(AnimatedCharacter, ModelMatrixFacesLogicalForward) {
    // The bot asset is authored facing +Z (native forward), while the
    // character logic treats local -Z as forward. The world model matrix must
    // rotate by heading + 180 degrees so the RENDERED model faces the logical
    // forward: the follow camera sits behind the logical forward, so the
    // model's face (local +Z) must point away from the camera. Without the
    // flip the visible model faces the camera, back-to-front.
    const float heading = 0.7f;
    const glm::vec3 pos(1.0f, 2.0f, 3.0f);
    const glm::mat4 m = AnimatedCharacter::modelMatrix(pos, heading, 0.01f);

    // Logical forward for this heading: local -Z in the character's frame.
    const glm::vec3 logicalFwd(-std::sin(heading), 0.0f, -std::cos(heading));

    // The model's native face (local +Z) must map to the logical forward.
    const glm::vec3 renderedFwd =
        glm::normalize(glm::vec3(m * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));
    EXPECT_NEAR(glm::length(renderedFwd - logicalFwd), 0.0f, 1e-4f)
        << "Rendered model must face the logical forward (away from the camera)";

    // The model's back (local -Z) must map AWAY from the logical forward,
    // i.e. toward the camera that sits behind the bot.
    const glm::vec3 renderedBack =
        glm::normalize(glm::vec3(m * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
    EXPECT_NEAR(glm::length(renderedBack - (-logicalFwd)), 0.0f, 1e-4f);

    // Translation is preserved (origin maps to the character position).
    const glm::vec3 origin = glm::vec3(m * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(glm::length(origin - pos), 0.0f, 1e-4f);
}

TEST(AnimatedCharacter, RootDoesNotSnapBackAtClipLoop) {
    // The locomotion clips carry baked root translation. With the root
    // LOCKED to its bind pose (AnimatedCharacter enables this on load), the
    // root bone must never jump backward when a clip wraps - the character's
    // velocity-driven position provides all forward motion, so over several
    // walk cycles the visual root must move forward monotonically.
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);

    CharacterInput ci = IdleInput();
    ci.moveDirection = glm::vec2(0.0f, -1.0f);  // forward (-Z)
    ci.moveMagnitude = 1.0f;

    const float dt = 1.0f / 60.0f;
    const Skeleton* skel = cc.skeleton();
    ASSERT_NE(skel, nullptr);
    ASSERT_GE(skel->rootBoneIndex, 0);

    float maxBackStep = 0.0f;
    glm::vec3 prevRoot(0.0f);
    bool havePrev = false;
    for (int i = 0; i < 240; ++i) {  // > 2 walk cycles
        cc.update(dt, ci, FlatTerrain);
        const glm::vec3 rootPos =
            cc.animator()->GetBoneWorldPosition(skel->rootBoneIndex, cc.modelMatrix());
        if (havePrev) {
            const float fwd = prevRoot.z - rootPos.z;  // walking -Z (forward)
            maxBackStep = std::max(maxBackStep, -fwd);
        }
        prevRoot = rootPos;
        havePrev = true;
    }

    // A clip-loop snap would appear as a large backward jump (a fraction of a
    // stride). With the root locked there must be none.
    EXPECT_LT(maxBackStep, 0.02f)
        << "Root must never snap backward at a clip loop boundary";
}

TEST(AnimatedCharacter, JumpArcReturnsToGround) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);

    const float dt = 1.0f / 60.0f;

    // Hold jump for 5 frames -> airborne.
    CharacterInput jump = IdleInput();
    jump.jump = true;
    for (int i = 0; i < 5; ++i) cc.update(dt, jump, FlatTerrain);
    EXPECT_GT(cc.position.y, kFlatTerrainY + 0.3f) << "character should be airborne";
    EXPECT_FALSE(cc.grounded);

    // Release jump, let gravity bring it back down (3 simulated seconds).
    CharacterInput airborne = IdleInput();
    for (int i = 0; i < 180; ++i) cc.update(dt, airborne, FlatTerrain);
    EXPECT_NEAR(cc.position.y, kFlatTerrainY, 0.05f) << "lands back on the terrain";
    EXPECT_TRUE(cc.grounded);
}

// ---------------------------------------------------------------------------
// Animation FSM
// ---------------------------------------------------------------------------

TEST(AnimatedCharacter, FsmIdleWhenStanding) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");

    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 15; ++i) cc.update(dt, IdleInput(), FlatTerrain);
    EXPECT_EQ(cc.state(), AnimationState::IDLE);
}

TEST(AnimatedCharacter, MotionMatchingFallsBackToFsmGracefully) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");
    cc.motionMatchingEnabled = true;
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);

    const float dt = 1.0f / 60.0f;
    // Must not crash and must keep animating (via the FSM fallback when the
    // database rejected the clips, or via motion matching when it didn't).
    for (int i = 0; i < 30; ++i) cc.update(dt, IdleInput(), FlatTerrain);
    EXPECT_TRUE(std::isfinite(cc.position.y));
    EXPECT_EQ(cc.state(), AnimationState::IDLE);
    if (!cc.motionMatchingReady()) {
        // Database rejected the clips: must fall back to the FSM.
        EXPECT_FALSE(cc.isMotionMatchingActive())
            << "with an empty database the character must fall back to the FSM";
    } else {
        // Clips loaded (after the tick->seconds fix): MM must be active.
        EXPECT_TRUE(cc.isMotionMatchingActive());
    }
}

TEST(AnimatedCharacter, TerrainSlopeDoesNotBreakMovement) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);
    cc.terrainNormal = glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f));

    CharacterInput ci = IdleInput();
    ci.moveDirection = glm::vec2(0.0f, 1.0f);
    ci.moveMagnitude = 1.0f;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 30; ++i) cc.update(dt, ci, FlatTerrain);
    EXPECT_TRUE(std::isfinite(cc.position.x) && std::isfinite(cc.position.z));
    EXPECT_GT(cc.position.z, 0.1f) << "should still move with a sloped normal";
}

TEST(AnimatedCharacter, PlaybackSpeedScalesAnimationTime) {
    AnimatedCharacter fast;
    ASSERT_TRUE(fast.load("assets/bot.fbx"));
    fast.loadLocomotion("assets");
    fast.playbackSpeed = 2.0f;
    AnimatedCharacter slow;
    ASSERT_TRUE(slow.load("assets/bot.fbx"));
    slow.loadLocomotion("assets");
    slow.playbackSpeed = 0.5f;

    // Playback speed is an FSM-only feature: once the motion-matching database
    // accepts these clips (it does, after the tick->seconds loader fix), the
    // matcher drives the animator and ignores playbackSpeed. Disable it here so
    // this test measures the FSM's time scaling deterministically.
    fast.motionMatchingEnabled = false;
    slow.motionMatchingEnabled = false;

    // Interleaving two instances is safe: blend-space playback tracking is now
    // per-FSM-instance (was a process-global static).
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 60; ++i) {
        fast.update(dt, IdleInput(), FlatTerrain);
        slow.update(dt, IdleInput(), FlatTerrain);
    }
    EXPECT_GT(fast.animatorTime(), slow.animatorTime());
    EXPECT_GT(fast.animatorTime(), 0.5f);  // ~2x of 1 simulated second
    EXPECT_LT(slow.animatorTime(), 1.5f);  // ~0.5x of 1 simulated second
}

TEST(AnimatedCharacter, FsmBlendsIdleWalkRun) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");
    // Force the FSM path: with the clips now loading into the motion-matching
    // database, MM would drive the animator and the FSM state would stay IDLE.
    // This test is specifically about FSM blend-band state selection.
    cc.motionMatchingEnabled = false;
    const float dt = 1.0f / 60.0f;

    // Walking (no sprint) -> WALK band.
    CharacterInput walk = IdleInput();
    walk.moveDirection = glm::vec2(0.0f, 1.0f);
    walk.moveMagnitude = 1.0f;
    for (int i = 0; i < 60; ++i) cc.update(dt, walk, FlatTerrain);
    EXPECT_EQ(cc.state(), AnimationState::WALK);

    // Sprinted run -> RUN band.
    CharacterInput run = IdleInput();
    run.moveDirection = glm::vec2(0.0f, 1.0f);
    run.moveMagnitude = 1.0f;
    run.sprint = true;
    for (int i = 0; i < 60; ++i) cc.update(dt, run, FlatTerrain);
    EXPECT_EQ(cc.state(), AnimationState::RUN);

    // Stop -> back to IDLE.
    for (int i = 0; i < 60; ++i) cc.update(dt, IdleInput(), FlatTerrain);
    EXPECT_EQ(cc.state(), AnimationState::IDLE);
}

TEST(AnimatedCharacter, FsmLandingCrossfadesInsteadOfHardCut) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");
    cc.motionMatchingEnabled = false;  // deterministic FSM path
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);
    const float dt = 1.0f / 60.0f;

    // Take off.
    CharacterInput jump = IdleInput();
    jump.jump = true;
    cc.update(dt, jump, FlatTerrain);
    ASSERT_TRUE(cc.mmAirborne()) << "grounded jump must leave the ground";

    // Fall while holding forward input until we touch down again.
    CharacterInput move = IdleInput();
    move.moveDirection = glm::vec2(0.0f, 1.0f);
    move.moveMagnitude = 1.0f;
    int frames = 0;
    while (cc.mmAirborne() && frames < 240) {
        cc.update(dt, move, FlatTerrain);
        ++frames;
    }
    ASSERT_FALSE(cc.mmAirborne()) << "must land within the fall budget";
    ASSERT_EQ(cc.state(), AnimationState::WALK) << "must land into the walk state";

    // Landing is a crossfade (2+ animator layers: outgoing Jump + incoming
    // Walk), not the old hard Play() cut which left exactly one layer.
    // (Fall clip temporarily removed — Jump covers the entire arc.)
    EXPECT_GE(cc.animator()->GetActiveAnimationLayerCount(), 2)
        << "Jump->Walk landing must crossfade instead of hard-cutting";

    // The blend completes and the outgoing Jump layer is pruned.
    for (int i = 0; i < 40; ++i) cc.update(dt, move, FlatTerrain);
    EXPECT_EQ(cc.animator()->GetActiveAnimationLayerCount(), 1)
        << "finished crossfade must collapse back to a single layer";
    EXPECT_EQ(cc.state(), AnimationState::WALK);
    EXPECT_EQ(cc.activeClipName(), "Walk");
}

// ---------------------------------------------------------------------------
// Motion-matching clip selection while moving (regression for the KD-tree
// representative-pose bug, the world-vs-clip velocity frame mismatch, and the
// sprint speed band). With all three fixed: walking matches Walk in either
// direction, sprinting reaches Run, stopping returns to Idle, crouching shows
// the crouch clips, and airborne poses never play while grounded.
// ---------------------------------------------------------------------------

TEST(AnimatedCharacter, MotionMatchingSelectsCorrectClipsWhileMoving) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");
    cc.motionMatchingEnabled = true;
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);

    const float dt = 1.0f / 60.0f;
    const auto groundedClip = [&]() { return cc.activeClipName(); };

    // Standing still -> Idle (rest override).
    for (int i = 0; i < 30; ++i) cc.update(dt, IdleInput(), FlatTerrain);
    EXPECT_EQ(groundedClip(), "Idle");

    // Walking forward (-Z) at full walk speed -> Walk clip, and NEVER an
    // airborne (Jump/Fall) pose while grounded (the state gate).
    CharacterInput walk = IdleInput();
    walk.moveDirection = glm::vec2(0.0f, -1.0f);
    walk.moveMagnitude = 1.0f;
    for (int i = 0; i < 90; ++i) cc.update(dt, walk, FlatTerrain);
    EXPECT_GE(cc.currentSpeed(), 1.4f) << "should be at walk speed";
    EXPECT_EQ(groundedClip(), "Walk");
    EXPECT_TRUE(cc.mmAirborne() == false);
    EXPECT_NE(groundedClip(), "Jump");
    EXPECT_NE(groundedClip(), "Fall");

    // Walking BACKWARD (+Z, opposite the -Z forward axis) must also match
    // Walk - the query velocity is expressed in the clip frame, so the
    // selection is heading-independent (regression: the old world-space
    // velocity flipped sign and the matcher re-selected Idle/Crouch,
    // causing footskate).
    CharacterInput back = IdleInput();
    back.moveDirection = glm::vec2(0.0f, 1.0f);
    back.moveMagnitude = 1.0f;
    for (int i = 0; i < 90; ++i) cc.update(dt, back, FlatTerrain);
    EXPECT_EQ(groundedClip(), "Walk")
        << "backward movement must still match the Walk clip";
    EXPECT_NE(groundedClip(), "Jump");
    EXPECT_NE(groundedClip(), "Fall");

    // Sprinting -> the Run clip (regression: the old query scale peaked below
    // the Run clip's feature range, so the matcher sprinted in CrouchWalk).
    CharacterInput run = IdleInput();
    run.moveDirection = glm::vec2(0.0f, 1.0f);
    run.moveMagnitude = 1.0f;
    run.sprint = true;
    for (int i = 0; i < 120; ++i) cc.update(dt, run, FlatTerrain);
    EXPECT_GE(cc.currentSpeed(), 5.9f) << "should reach sprint speed";
    EXPECT_EQ(groundedClip(), "Run")
        << "full sprint must select the Run clip";
    EXPECT_NE(groundedClip(), "CrouchWalk");

    // Stopping -> back to Idle (rest override).
    for (int i = 0; i < 90; ++i) cc.update(dt, IdleInput(), FlatTerrain);
    EXPECT_EQ(groundedClip(), "Idle");

    // Crouching while moving -> CrouchWalk (deterministic override).
    CharacterInput crouch = IdleInput();
    crouch.moveDirection = glm::vec2(0.0f, 1.0f);
    crouch.moveMagnitude = 1.0f;
    crouch.crouch = true;
    for (int i = 0; i < 90; ++i) cc.update(dt, crouch, FlatTerrain);
    EXPECT_TRUE(cc.crouchState());
    EXPECT_GT(cc.currentSpeed(), 1.0f);
    EXPECT_EQ(groundedClip(), "CrouchWalk");

    // Everything stayed finite throughout.
    EXPECT_TRUE(std::isfinite(cc.position.y));
    EXPECT_TRUE(std::isfinite(cc.animatorTime()));
}

// ---------------------------------------------------------------------------
// Visual-animation contract: the clip LOG being correct is not enough - the
// character must PHYSICALLY move and the final bone matrices (what the skinned
// mesh renders with) must CHANGE across frames while walking. This locks the
// "bot is frozen in one pose" regression where the matcher selected the right
// clip but nothing visibly animated.
// ---------------------------------------------------------------------------

TEST(AnimatedCharacter, PoseAndPositionChangeWhileWalking) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");
    cc.motionMatchingEnabled = true;
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);

    const float dt = 1.0f / 60.0f;

    // Pose fingerprint: sum of all bone translations. If this stays constant
    // across frames the mesh is frozen regardless of the clip log.
    auto poseSum = [](const std::vector<glm::mat4>& bones) -> glm::vec3 {
        glm::vec3 s(0.0f);
        for (const auto& b : bones) s += glm::vec3(b[3]);
        return s;
    };

    // Settle into idle (MM rest override plays Idle).
    for (int i = 0; i < 30; ++i) cc.update(dt, IdleInput(), FlatTerrain);
    const glm::vec3 idleSum = poseSum(cc.debugFinalBones());

    // Walk for 90 frames, sampling the pose every 15 frames.
    CharacterInput walk = IdleInput();
    walk.moveDirection = glm::vec2(0.0f, 1.0f);
    walk.moveMagnitude = 1.0f;
    glm::vec3 prevWalkSum = idleSum;
    float maxDelta = 0.0f;
    for (int i = 0; i < 90; ++i) {
        cc.update(dt, walk, FlatTerrain);
        if (i % 15 == 0) {
            const glm::vec3 s = poseSum(cc.debugFinalBones());
            maxDelta = std::max(maxDelta, glm::length(s - prevWalkSum));
            prevWalkSum = s;
        }
    }

    // The matcher must be driving the Walk clip at full walk speed.
    // (speed cap was lowered 2.0 -> 1.5 m/s to stop the matcher from
    // selecting jog/run clips; see AnimatedCharacter::update)
    EXPECT_EQ(cc.activeClipName(), "Walk");
    EXPECT_GE(cc.currentSpeed(), 1.4f) << "should be at walk speed";

    // The character must physically move forward (camera-follow in play mode
    // keeps the bot centered on screen, so this is what proves motion).
    EXPECT_GT(cc.position.z, 1.0f) << "walking must advance the position";

    // The pose must CHANGE between frames - any locomotion cycle moves bones.
    // A frozen bot (bind pose) would keep this delta at ~0.
    EXPECT_GT(maxDelta, 0.05f)
        << "final bone matrices must change across frames while walking";
}

// ---------------------------------------------------------------------------
// Footplanting regression: the user reported footskating ("animation works
// only for the idle pose but other poses default to idle pose animation") and
// proposed footplanting - detect whether a leg was lifted to simulate walk.
// This locks the full gait contract: the animator's dominant layer must be the
// matcher's clip (pointer identity - no Idle-while-walking), the feet must
// LIFT and PLANT in an alternating cycle (a real gait), the foot-IK must
// engage its plant lock, and the Walk clip must play at a rate scaled to match
// the physics speed (anti-footskate).
// ---------------------------------------------------------------------------

TEST(AnimatedCharacter, FootCycleWhileWalkingMatchesClipsAndPlants) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    cc.loadLocomotion("assets");
    cc.motionMatchingEnabled = true;
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);
    const float dt = 1.0f / 60.0f;

    // Settle into idle first (as a user would - start standing, then walk).
    for (int i = 0; i < 30; ++i) cc.update(dt, IdleInput(), FlatTerrain);
    const auto idle = cc.debugPoseDiag();
    printf("DIAG idle: matcher=%s domMatch=%d w=%.2f t=%.2f L=%.1f R=%.1f idleClipSpeed=%.2f\n",
           idle.matcherClip.c_str(), idle.dominantMatchesMatcher ? 1 : 0,
           idle.dominantWeight, idle.dominantTime, idle.leftFoot.y, idle.rightFoot.y,
           cc.debugClipSpeed(0));

    CharacterInput walk = IdleInput();
    walk.moveDirection = glm::vec2(0.0f, 1.0f);
    walk.moveMagnitude = 1.0f;

    float minLY = 1e9f, maxLY = -1e9f, minRY = 1e9f, maxRY = -1e9f;
    int dominantMismatch = 0;     // frames (steady-state tail only) where animator dominant != matcher clip
    int sawIdleDominant = 0;       // frames where dominant clip was Idle while moving
    int liftEvents = 0;            // foot Y rising while other foot planted-ish
    bool prevLeftAbove = false, prevRightAbove = false;
    std::string lastPrint;

    constexpr int kTailFrom = 120;  // steady-state tail: last 60 of 180 frames.
    // The Idle->Walk blend-in window is excluded by using a TAIL window, not a
    // fixed prefix skip - the assertion then holds for any blend duration.
    for (int i = 0; i < 180; ++i) {
        cc.update(dt, walk, FlatTerrain);
        const auto d = cc.debugPoseDiag();
        if (i >= kTailFrom) {
            if (!d.dominantMatchesMatcher) ++dominantMismatch;
            if (d.dominantRawName.find("Idle") != std::string::npos) ++sawIdleDominant;
        }
        minLY = std::min(minLY, d.leftFoot.y); maxLY = std::max(maxLY, d.leftFoot.y);
        minRY = std::min(minRY, d.rightFoot.y); maxRY = std::max(maxRY, d.rightFoot.y);

        // Leg-lift signal: a foot is "lifted" when it rises above the other
        // (the walk cycle alternates stance/swing - both feet must never be
        // glued to the ground). Track when the relative height flips.
        const bool leftAbove = (d.leftFoot.y > d.rightFoot.y + 1.0f);
        const bool rightAbove = (d.rightFoot.y > d.leftFoot.y + 1.0f);
        if (leftAbove != prevLeftAbove) ++liftEvents;
        if (rightAbove != prevRightAbove) ++liftEvents;
        prevLeftAbove = leftAbove;
        prevRightAbove = rightAbove;

        if (i % 12 == 0) {
            printf("DIAG f=%d pos=(%.1f,%.1f) matcher=%s domMatch=%d(w=%.2f,t=%.2f) L=(%.1f,%.1f,%.1f)%s R=(%.1f,%.1f,%.1f)%s walkSpeed=%.2f\n",
                   i, cc.position.x, cc.position.z,
                   d.matcherClip.c_str(), d.dominantMatchesMatcher ? 1 : 0,
                   d.dominantWeight, d.dominantTime,
                   d.leftFoot.x, d.leftFoot.y, d.leftFoot.z, d.leftLocked ? "[LK]" : "",
                   d.rightFoot.x, d.rightFoot.y, d.rightFoot.z, d.rightLocked ? "[LK]" : "",
                   cc.debugClipSpeed(1));
        }
    }
    printf("DIAG SUMMARY: pos=(%.1f,%.1f,%.1f) speed=%.2f clip=%s walkClipSpeed=%.2f\n",
           cc.position.x, cc.position.y, cc.position.z, cc.currentSpeed(),
           cc.activeClipName().c_str(), cc.debugClipSpeed(1));
    printf("DIAG SUMMARY: leftFootY range [%.1f, %.1f] (%.1f) rightFootY range [%.1f, %.1f] (%.1f)\n",
           minLY, maxLY, maxLY - minLY, minRY, maxRY, maxRY - minRY);
    printf("DIAG SUMMARY: dominantMismatch=%d frames sawIdleDominant=%d liftEvents=%d\n",
           dominantMismatch, sawIdleDominant, liftEvents);

    // The pose search must select Walk AND the animator must actually be
    // blending it as the dominant layer (pointer identity - the "idle pose
    // while walking" hypothesis).
    EXPECT_EQ(cc.activeClipName(), "Walk");
    EXPECT_EQ(dominantMismatch, 0)
        << "animator dominant layer must match the matcher's selected clip";
    EXPECT_EQ(sawIdleDominant, 0) << "Idle must never dominate while walking";

    // The feet must cycle (lift/plant) - a real gait, not idle breathing.
    EXPECT_GT(maxLY - minLY, 5.0f) << "left foot must lift/plant during walk";
    EXPECT_GT(maxRY - minRY, 5.0f) << "right foot must lift/plant during walk";

    // Anti-footskate: the Walk clip rate is physicsSpeed/clipStrideSpeed so a
    // stride's ground coverage matches the character's actual movement. With
    // the walk speed cap at ~1.5 m/s the Walk clip's natural stride is slightly
    // faster, so the rate lands just below real-time (within the [0.6,1.5]
    // clamp) rather than above it.
    EXPECT_GT(cc.debugClipSpeed(1), 0.8f)
        << "Walk clip must play at an active anti-footskate rate within the clamp";
    EXPECT_LT(cc.debugClipSpeed(1), 1.5f);
    EXPECT_NEAR(cc.debugClipSpeed(0), 1.0f, 0.01f)
        << "Idle clip must play at real time";

    // Anti-footskate must hold across the OTHER locomotion bands too - a clip
    // whose stride over- or under-shoots the physics speed gets a rate in the
    // [0.6, 1.5] clamp, never left at a stale value (regression: only Walk was
    // scaled, so Run/CrouchWalk kept real-time playback and skated).
    CharacterInput run = IdleInput();
    run.moveDirection = glm::vec2(0.0f, 1.0f);
    run.moveMagnitude = 1.0f;
    run.sprint = true;
    for (int i = 0; i < 120; ++i) cc.update(dt, run, FlatTerrain);
    EXPECT_EQ(cc.activeClipName(), "Run");
    EXPECT_GE(cc.debugClipSpeed(2), 0.6f) << "Run clip rate must stay in the clamp";
    EXPECT_LE(cc.debugClipSpeed(2), 1.5f) << "Run clip rate must stay in the clamp";

    CharacterInput crouch = IdleInput();
    crouch.moveDirection = glm::vec2(0.0f, 1.0f);
    crouch.moveMagnitude = 1.0f;
    crouch.crouch = true;
    for (int i = 0; i < 120; ++i) cc.update(dt, crouch, FlatTerrain);
    EXPECT_EQ(cc.activeClipName(), "CrouchWalk");
    EXPECT_GE(cc.debugClipSpeed(6), 0.6f) << "CrouchWalk clip rate must stay in the clamp";
    EXPECT_LE(cc.debugClipSpeed(6), 1.5f) << "CrouchWalk clip rate must stay in the clamp";
}

// ---------------------------------------------------------------------------
// Contextual database switching (the suggestions.txt architecture): the
// character owns a dedicated MotionDatabase per context and the matcher
// searches ONLY the active context's clips. Combat swings can never bleed
// into a locomotion walk, the capoeira set loads lazily, and crouch input
// auto-switches Locomotion<->Crouch.
// ---------------------------------------------------------------------------

TEST(AnimatedCharacter, LoadsContextDatabases) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    ASSERT_GE(cc.loadLocomotion("assets"), 1);

    // The enriched locomotion context carries extra clips (Walk/Run/Jump)
    // beyond the 7 fixed FSM slots, and is built eagerly as the default
    // search domain.  (Jog, Catwalk, turns, RunLookBack, and Fall are
    // temporarily pruned — see ensureContextBuilt.
    EXPECT_GE(cc.contextClipCount(AnimatedCharacter::MotionContext::LOCOMOTION), 4)
        << "locomotion context must hold the core locomotion clips";
    EXPECT_EQ(cc.motionContext(), AnimatedCharacter::MotionContext::LOCOMOTION);
    EXPECT_GT(cc.contextPoseCount(AnimatedCharacter::MotionContext::LOCOMOTION), 0u);

    // Every other context is LAZY: not loaded at startup, built on first
    // request (the suggestions' "only load what the active context needs").
    EXPECT_EQ(cc.contextClipCount(AnimatedCharacter::MotionContext::CROUCH), 0);
    EXPECT_EQ(cc.contextClipCount(AnimatedCharacter::MotionContext::COMBAT), 0);
    EXPECT_EQ(cc.contextClipCount(AnimatedCharacter::MotionContext::DANCE), 0);
    EXPECT_EQ(cc.contextPoseCount(AnimatedCharacter::MotionContext::CAPOEIRA), 0u);

    // Requesting a context builds its database on demand.
    cc.setMotionContext(AnimatedCharacter::MotionContext::COMBAT, 0.0f);
    EXPECT_EQ(cc.motionContext(), AnimatedCharacter::MotionContext::COMBAT);
    EXPECT_GE(cc.contextClipCount(AnimatedCharacter::MotionContext::COMBAT), 3)
        << "Boxing/BodyBlock/BoxTurn/Defeated must load on request";
}

TEST(AnimatedCharacter, MotionContextSwitchingIsolatesSearchDatabase) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    ASSERT_GE(cc.loadLocomotion("assets"), 1);
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);
    const float dt = 1.0f / 60.0f;

    // Settle in the default locomotion context.
    for (int i = 0; i < 30; ++i) cc.update(dt, IdleInput(), FlatTerrain);

    // Switch to combat instantly and run the pose search: every clip the
    // matcher selects must come from the combat database - never a locomotion
    // clip (the non-owning SetDatabase path + effective-database routing).
    cc.setMotionContext(AnimatedCharacter::MotionContext::COMBAT, 0.0f);
    EXPECT_EQ(cc.motionContext(), AnimatedCharacter::MotionContext::COMBAT);
    for (int i = 0; i < 60; ++i) {
        cc.update(dt, IdleInput(), FlatTerrain);
        const std::string clip = cc.mmActiveClip();
        ASSERT_FALSE(clip.empty()) << "matcher must select a combat clip";
        EXPECT_TRUE(clip == "Boxing" || clip == "BodyBlock" ||
                    clip == "BoxTurn" || clip == "Defeated")
            << "combat context must only select combat clips, got '" << clip << "'";
    }

    // Switch back to locomotion: the search domain returns to the locomotion
    // clips (and the rest override idles the animator while standing still).
    cc.setMotionContext(AnimatedCharacter::MotionContext::LOCOMOTION, 0.0f);
    EXPECT_EQ(cc.motionContext(), AnimatedCharacter::MotionContext::LOCOMOTION);
    for (int i = 0; i < 30; ++i) cc.update(dt, IdleInput(), FlatTerrain);
    EXPECT_EQ(cc.activeClipName(), "Idle");
    const std::string clip = cc.mmActiveClip();
    EXPECT_FALSE(clip.empty());
    EXPECT_NE(clip, "Boxing") << "locomotion context must not play combat clips";
}

TEST(AnimatedCharacter, MotionContextRequestViaInput) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    ASSERT_GE(cc.loadLocomotion("assets"), 1);
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);
    const float dt = 1.0f / 60.0f;

    // A one-shot explicit request (as the editor's 1-5 keys send).
    CharacterInput ci = IdleInput();
    ci.motionContext = (int)AnimatedCharacter::MotionContext::DANCE;
    cc.update(dt, ci, FlatTerrain);
    EXPECT_EQ(cc.motionContext(), AnimatedCharacter::MotionContext::DANCE);

    // Later frames without a request keep the context (no yanking).
    for (int i = 0; i < 10; ++i) cc.update(dt, IdleInput(), FlatTerrain);
    EXPECT_EQ(cc.motionContext(), AnimatedCharacter::MotionContext::DANCE);

    // Requesting the same context again is a no-op (no restart of the blend).
    ci.motionContext = (int)AnimatedCharacter::MotionContext::DANCE;
    cc.update(dt, ci, FlatTerrain);
    EXPECT_EQ(cc.motionContext(), AnimatedCharacter::MotionContext::DANCE);
}

TEST(AnimatedCharacter, CrouchInputAutoSwitchesToCrouchContext) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    ASSERT_GE(cc.loadLocomotion("assets"), 1);
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);
    const float dt = 1.0f / 60.0f;

    EXPECT_EQ(cc.motionContext(), AnimatedCharacter::MotionContext::LOCOMOTION);

    // Crouch edge -> dedicated crouch database drives.
    CharacterInput crouch = IdleInput();
    crouch.moveDirection = glm::vec2(0.0f, 1.0f);
    crouch.moveMagnitude = 1.0f;
    crouch.crouch = true;
    cc.update(dt, crouch, FlatTerrain);
    EXPECT_EQ(cc.motionContext(), AnimatedCharacter::MotionContext::CROUCH);

    // Sustained crouch stays in the crouch context (edge-triggered switch).
    for (int i = 0; i < 30; ++i) cc.update(dt, crouch, FlatTerrain);
    EXPECT_EQ(cc.motionContext(), AnimatedCharacter::MotionContext::CROUCH);
    EXPECT_EQ(cc.activeClipName(), "CrouchWalk");

    // Release -> back to locomotion.
    for (int i = 0; i < 5; ++i) cc.update(dt, IdleInput(), FlatTerrain);
    EXPECT_EQ(cc.motionContext(), AnimatedCharacter::MotionContext::LOCOMOTION);
}

TEST(AnimatedCharacter, CapoeiraContextLazilyBuildsOnDemand) {
    AnimatedCharacter cc;
    ASSERT_TRUE(cc.load("assets/bot.fbx"));
    ASSERT_GE(cc.loadLocomotion("assets"), 1);
    cc.position = glm::vec3(0.0f, kFlatTerrainY, 0.0f);
    const float dt = 1.0f / 60.0f;

    // Not loaded until requested.
    EXPECT_EQ(cc.contextPoseCount(AnimatedCharacter::MotionContext::CAPOEIRA), 0u);

    cc.setMotionContext(AnimatedCharacter::MotionContext::CAPOEIRA, 0.0f);
    EXPECT_EQ(cc.motionContext(), AnimatedCharacter::MotionContext::CAPOEIRA);
    EXPECT_GE(cc.contextClipCount(AnimatedCharacter::MotionContext::CAPOEIRA), 30)
        << "the capoeira folder must yield its clips";
    EXPECT_GT(cc.contextPoseCount(AnimatedCharacter::MotionContext::CAPOEIRA), 0u);

    // The pose search now runs over capoeira clips only.
    for (int i = 0; i < 30; ++i) cc.update(dt, IdleInput(), FlatTerrain);
    const std::string clip = cc.mmActiveClip();
    EXPECT_FALSE(clip.empty());
    EXPECT_NE(clip, "Walk") << "capoeira context must not select locomotion clips";
    EXPECT_NE(clip, "Boxing");
}

// Note: main() is in test_main.cpp - don't duplicate
