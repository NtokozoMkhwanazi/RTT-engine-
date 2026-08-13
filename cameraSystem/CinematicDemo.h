#pragma once
/**
 * CinematicDemo.h
 *
 * Pure math behind the Cinematic camera mode's auto-play character demo:
 * an idle -> walk -> run -> jump -> return loop. Produces CharacterInput
 * structs for each point in time.
 */

#include <glm/glm.hpp>

namespace CinematicDemo {

// Character input for the scripted demo.
struct Input {
    glm::vec2 moveDirection{0.0f};  // 0 = forward is -Z (character local)
    bool sprint = false;
    bool jump = false;
};

// Total loop duration in seconds (5 phases of 4s).
inline constexpr double kLoopDuration = 20.0;

// Map a time point to a phase index in [0, 5]:
//   0: Idle          (0-2s)
//   1: Walk forward  (2-4s, -Z)
//   2: Run forward   (4-8s, sprint, jump on entry)
//   3: Return run    (8-12s, +Z, sprint)
//   4: Walk back     (12-15s, +Z)
//   5: Idle          (15-20s)
inline int PhaseAt(double t) {
    t = std::fmod(t, kLoopDuration);
    if (t < 0.0) t += kLoopDuration;
    if (t < 2.0) return 0;
    if (t < 4.0) return 1;
    if (t < 8.0) return 2;
    if (t < 12.0) return 3;
    if (t < 15.0) return 4;
    return 5;
}

// Input at time t (seconds). firstFrame signals the phase-edge so the jump
// fires only on the Run-phase entry.
inline Input At(double t, bool firstFrame = false) {
    Input in;
    const int phase = PhaseAt(t);
    switch (phase) {
        case 0: // Idle
            in.moveDirection = glm::vec2(0.0f);
            break;
        case 1: // Walk forward (-Z)
            in.moveDirection = glm::vec2(0.0f, -1.0f);
            break;
        case 2: // Run forward, jump on entry
            in.moveDirection = glm::vec2(0.0f, -1.0f);
            in.sprint = true;
            in.jump = firstFrame;
            break;
        case 3: // Return run (+Z)
            in.moveDirection = glm::vec2(0.0f, 1.0f);
            in.sprint = true;
            break;
        case 4: // Walk back (+Z)
            in.moveDirection = glm::vec2(0.0f, 1.0f);
            break;
        default: // Idle
            in.moveDirection = glm::vec2(0.0f);
            break;
    }
    return in;
}

} // namespace CinematicDemo