#pragma once
// ============================================================================
// PhysicalSky — compact analytic sky (suggestions.txt #4).
// ============================================================================
// Pure-C++ sky *radiance* model (Rayleigh + Mie + sun disk). No GL/Vulkan:
// this is the deterministic color math a scattering skybox shader would
// consume. The precompute (scattering LUT / cubemap) + the GLSL tile that
// renders it is the deferred follow-up — this header is the unit-tested core.
//
// `evaluate(...)` returns linear radiance in the same space as `viewDir`
// and `sunDir` (caller picks world or view space, consistent for both).
// ============================================================================
#include <glm/glm.hpp>

namespace PhysicalSky {

// Evaluate sky radiance (linear, ~[0, sunIntensity]) for a view direction.
//   viewDir  : normalized view direction.
//   sunDir   : normalized direction toward the sun.
//   turbidity: 1.0 (very clear) .. ~64.0 (hazy). Scales Mie haze.
//   sunIntensity: scales all direct/circum-solar terms.
glm::vec3 evaluate(const glm::vec3& viewDir,
                   const glm::vec3& sunDir,
                   float turbidity = 2.0f,
                   float sunIntensity = 1.0f);

// Simple Reinhard-ish tone map to [0,1] for quick asserts / UI preview.
glm::vec3 skyColor(const glm::vec3& viewDir,
                   const glm::vec3& sunDir,
                   float turbidity = 2.0f,
                   float sunIntensity = 1.0f);

// max-channel "brightness" of a radiance; the sky-scattering phase functions
// are isotropic in their (non-negative) terms, so max(rgb) is a monotonic
// brightness proxy (used by the tests).
float brightness(const glm::vec3& radiance);

} // namespace PhysicalSky
