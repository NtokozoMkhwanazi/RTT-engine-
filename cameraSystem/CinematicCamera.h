#pragma once
/**
 * CinematicCamera.h
 *
 * Pure math behind the Cinematic camera mode's forever-orbit:
 *   - AdvanceAngle  : angle advancement with 360-degree wrap
 *   - OrbitPosition : camera position on the orbit circle at a given angle
 *   - IntroHeight   : eased height descent during the mode-intro
 *   - At            : full pose builder
 */

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace Cinematic {

struct Pose {
    glm::vec3 position{0.0f};
    glm::vec3 target{0.0f};
    Pose() = default;
    Pose(const glm::vec3& p, const glm::vec3& t) : position(p), target(t) {}
};

// Advance an angle by degrees*dt, wrapping the result into [0, 360).
inline double AdvanceAngle(double currentDeg, double degreesPerSec, float dt) {
    double next = currentDeg + degreesPerSec * dt;
    next = std::fmod(next, 360.0);
    if (next < 0.0) next += 360.0;
    return next;
}

// Camera position on a circle of radius R at height h for an angle in degrees.
// Angle 0 sits on +X; 90 deg on +Z; 180 on -X; 270 on -Z.
inline glm::vec3 OrbitPosition(double angleDeg, float radius, float height) {
    const double rad = glm::radians(angleDeg);
    return glm::vec3(static_cast<float>(radius * std::cos(rad)),
                     height,
                     static_cast<float>(radius * std::sin(rad)));
}

// Smoothstep ease-in-out helper (shared with the height ease).
inline float EaseInOut(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Intro height: eases from startHeight to endHeight over introDuration seconds,
// holding at endHeight afterwards.
inline float IntroHeight(float elapsed, float introDuration,
                         float startHeight, float endHeight) {
    if (introDuration <= 0.0f) return endHeight;
    const float t = std::clamp(elapsed / introDuration, 0.0f, 1.0f);
    return startHeight + (endHeight - startHeight) * EaseInOut(t);
}

// Full pose at a given orbit angle, radius and height, looking at target.
inline Pose At(double angleDeg, float radius, float height, const glm::vec3& target) {
    return Pose(OrbitPosition(angleDeg, radius, height), target);
}

} // namespace Cinematic