// ============================================================================
// PhysicalSky implementation — analytic Rayleigh + Mie + sun disk (suggestions
// #4). Pure C++: no GL/Vulkan. See tests/test_physical_sky.cpp.
// ============================================================================
#include "lighting/PhysicalSky.h"

#include <algorithm>
#include <cmath>

namespace PhysicalSky {

static const glm::vec3 UP(0.0f, 1.0f, 0.0f);
static const glm::vec3 K_DAYLIGHT_SUN(1.10f, 1.05f, 0.90f); // warm direct sun
static const glm::vec3 K_TINT_BLUE(0.20f, 0.42f, 1.00f);    // Rayleigh tint
static const glm::vec3 K_STARFIELD(0.0010f, 0.0012f, 0.0020f);

static inline float clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

glm::vec3 evaluate(const glm::vec3& viewDir,
                   const glm::vec3& sunDir,
                   float turbidity,
                   float sunIntensity) {
    const glm::vec3 V = glm::normalize(viewDir);
    const glm::vec3 S = glm::normalize(sunDir);
    const float mu_s = glm::dot(S, UP);               // sun zenith cosine
    // Sharp(ish) cutoff at the horizon: slightly below -> no direct sun term.
    float sunContrib = (mu_s + 0.02f) / 0.04f;         // rise over [−0.02, +0.02]
    sunContrib = std::clamp(sunContrib, 0.0f, 1.0f);
    sunContrib = sunContrib * sunContrib * (3.0f - 2.0f * sunContrib); // smoothstep

    const float cosGamma = clamp01(glm::dot(V, S));   // view-sun angle cos
    const float cosTheta = clamp01(glm::dot(V, UP));  // view-zenith cos
    const float theta    = std::acos(cosTheta);

    // Atmospheric path length (longer near the horizon).
    const float rayDepth = 1.0f / (cosTheta + 0.15f);
    const float mieDepth = 1.0f / (cosTheta + 0.35f);

    // --- Rayleigh (blue) scattering: phase (3/4)(1 + cos^2 gamma) ---
    const float rayPhase = 0.75f * (1.0f + cosGamma * cosGamma);
    const glm::vec3 rayleigh = rayPhase * rayDepth * 0.30f
                             * sunIntensity * sunContrib * K_TINT_BLUE;

    // --- Mie (haze) sunward glow: Henyey-Greenstein g~0.8, scales w/ turbidity
    const float g = 0.80f;
    const float hh = 1.0f + g * g - 2.0f * g * cosGamma;
    const float miePhase = (1.0f - g * g) / (hh * std::sqrt(hh));
    const glm::vec3 mie = miePhase * mieDepth * 0.05f
                        * turbidity * sunIntensity * sunContrib
                        * glm::vec3(1.0f);

    // --- Sun disk: narrow lobe peaked at gamma=0 (direct sun) ---
    const float sunDisk = sunContrib * std::pow(cosGamma, 64.0f);
    const glm::vec3 sun = sunDisk * sunIntensity * K_DAYLIGHT_SUN;

    // --- Star field when the sun is down (keeps midnight from being pure black)
    const float stars = 1.0f - sunContrib;
    const glm::vec3 starfield = stars * K_STARFIELD;

    (void)theta;
    return rayleigh + mie + sun + starfield;
}

glm::vec3 skyColor(const glm::vec3& viewDir,
                   const glm::vec3& sunDir,
                   float turbidity,
                   float sunIntensity) {
    const glm::vec3 hdr = evaluate(viewDir, sunDir, turbidity, sunIntensity);
    // Simple Reinhard: 1 - exp(-hdr)  -> [0,1)-ish.
    return glm::vec3(1.0f) - glm::exp(-hdr);
}

float brightness(const glm::vec3& radiance) {
    return std::max({radiance.r, radiance.g, radiance.b});
}

} // namespace PhysicalSky
