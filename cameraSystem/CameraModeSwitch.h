#pragma once
/**
 * CameraModeSwitch.h
 *
 * Pure-math helpers for camera mode transitions. Given the current camera
 * pose and the requested mode, computes the destination pose the camera
 * glides to (via a smooth EaseInOut interpolation).
 *
 * Mode ids match Render::CameraMode: 0 FreeFly, 1 ThirdPerson, 2 FirstPerson,
 * 3 Orbit, 4 Cinematic.
 */

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace CameraSwitch {

// A camera pose: eye position + look-at target.
struct Pose {
    glm::vec3 position{0.0f};
    glm::vec3 target{0.0f};

    Pose() = default;
    Pose(const glm::vec3& p, const glm::vec3& t) : position(p), target(t) {}
};

// Smoothstep-style ease-in-out (clamped to [0, 1]).
inline float EaseInOut(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Compute the destination pose for switching into the given camera mode.
//
//   mode          - Render::CameraMode id (0..4)
//   current       - current camera pose
//   yawDeg, pitch - current look angles (degrees)
//   entityPos     - focus entity position (character / selection)
//   hasEntity     - whether a focus entity was provided
//   orbitStart    - start pose for the Cinematic mode's glide
//   minDist/maxDist - clamp range for the third-person follow distance
inline Pose DestinationPose(int mode, const Pose& current,
                            float yawDeg, float pitchDeg,
                            const glm::vec3& entityPos, bool hasEntity,
                            const Pose& orbitStart,
                            float minDist = 1.0f, float maxDist = 50.0f) {
    switch (mode) {
        case 0: { // FreeFly
            // Keep the eye; re-aim at the focus entity if provided.
            Pose dest = current;
            if (hasEntity) dest.target = entityPos;
            return dest;
        }
        case 3: { // Orbit
            // Keep current pose (orbit is controlled by the camera itself).
            return current;
        }
        case 2: { // FirstPerson
            // Place the target in front of the eye along the yaw/pitch heading.
            Pose dest = current;
            const float yaw = glm::radians(yawDeg);
            const float pitch = glm::radians(pitchDeg);
            glm::vec3 front;
            front.x = std::cos(pitch) * std::sin(yaw);
            front.y = std::sin(pitch);
            front.z = std::cos(pitch) * std::cos(yaw);
            if (glm::length(front) < 1e-6f) front = glm::vec3(0.0f, 0.0f, 1.0f);
            front = glm::normalize(front);
            dest.target = dest.position + front;
            return dest;
        }
        case 1: { // ThirdPerson
            // Keep the camera's existing viewing direction (from the OLD
            // target) and re-aim at the focus entity: the camera orbits around
            // the new target at the same angle and distance as before.
            Pose dest = current;
            const glm::vec3 oldTarget = current.target;
            if (hasEntity) dest.target = entityPos;
            glm::vec3 toCam = current.position - oldTarget;
            const float dist = glm::length(toCam);
            if (dist < 1e-5f) {
                // Degenerate: camera sits on the target. Fall back to a
                // fixed 8-unit offset along -Z so the view is never NaN.
                dest.position = dest.target + glm::vec3(0.0f, 0.0f, 8.0f);
                return dest;
            }
            const float clamped = std::clamp(dist, minDist, maxDist);
            dest.position = dest.target + glm::normalize(toCam) * clamped;
            return dest;
        }
        case 4: { // Cinematic
            // Glide to the fixed orbit start pose.
            return orbitStart;
        }
        default:
            return current;
    }
}

} // namespace CameraSwitch