/**
 * Cinematic Demo Script Tests
 *
 * Verifies CinematicDemo::At / CinematicDemo::PhaseAt - the pure math behind
 * Cinematic camera mode's auto-play character demo (idle -> walk -> run ->
 * jump -> return loop). Pure math tests, no GL, no ECS, no character.
 *
 * Run with: make test
 */

#include <gtest/gtest.h>
#include "cameraSystem/CinematicDemo.h"
#include <glm/glm.hpp>

TEST(CinematicDemo, IdlePhasesHaveNoInput) {
    const CinematicDemo::Input idle0 = CinematicDemo::At(1.0);   // phase 0
    EXPECT_EQ(idle0.moveDirection, glm::vec2(0.0f));
    EXPECT_FALSE(idle0.sprint);
    EXPECT_FALSE(idle0.jump);

    const CinematicDemo::Input idle5 = CinematicDemo::At(16.0);  // phase 5
    EXPECT_EQ(idle5.moveDirection, glm::vec2(0.0f));
    EXPECT_FALSE(idle5.sprint);
}

TEST(CinematicDemo, WalkPhaseMovesForwardAtWalkSpeed) {
    const CinematicDemo::Input w = CinematicDemo::At(3.0);
    // Local forward is -Z (the character's natural forward axis).
    EXPECT_EQ(w.moveDirection, glm::vec2(0.0f, -1.0f));
    EXPECT_FALSE(w.sprint);
    EXPECT_FALSE(w.jump);
}

TEST(CinematicDemo, RunPhaseSprinterAndJumpsOnEntry) {
    const CinematicDemo::Input run = CinematicDemo::At(6.0, true);  // first frame
    EXPECT_EQ(run.moveDirection, glm::vec2(0.0f, -1.0f));
    EXPECT_TRUE(run.sprint);
    EXPECT_TRUE(run.jump);

    const CinematicDemo::Input runLater = CinematicDemo::At(8.0, false);
    EXPECT_TRUE(runLater.sprint);
    EXPECT_FALSE(runLater.jump);  // the jump fires only on the phase edge
}

TEST(CinematicDemo, ReturnPhaseTurnsAroundAndRunsBack) {
    const CinematicDemo::Input back = CinematicDemo::At(10.0);
    EXPECT_EQ(back.moveDirection, glm::vec2(0.0f, 1.0f));  // +Z return leg
    EXPECT_TRUE(back.sprint);

    const CinematicDemo::Input walkBack = CinematicDemo::At(14.0);
    EXPECT_EQ(walkBack.moveDirection, glm::vec2(0.0f, 1.0f));
    EXPECT_FALSE(walkBack.sprint);  // decelerating leg
}

TEST(CinematicDemo, LoopWrapsAtDuration) {
    // t and t + kLoopDuration describe the same scripted moment.
    const CinematicDemo::Input a = CinematicDemo::At(3.0, true);
    const CinematicDemo::Input b = CinematicDemo::At(3.0 + CinematicDemo::kLoopDuration, true);
    EXPECT_EQ(a.moveDirection, b.moveDirection);
    EXPECT_EQ(a.sprint, b.sprint);
    EXPECT_EQ(a.jump, b.jump);
    EXPECT_EQ(CinematicDemo::PhaseAt(0.0),
              CinematicDemo::PhaseAt(CinematicDemo::kLoopDuration));
    // Defensive: a slightly negative time (dt jitter) must still yield a
    // valid phase index in [0, 5], never a garbage value.
    const int negPhase = CinematicDemo::PhaseAt(-0.5);
    EXPECT_GE(negPhase, 0);
    EXPECT_LE(negPhase, 5);
}
