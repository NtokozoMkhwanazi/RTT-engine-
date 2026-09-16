#include "LightingEnvironment.h"
#include <cmath>

void LightingEnvironment::applyCvars() {
    // --- Directional key sun -------------------------------------------------
    // Resolve (sun_direction) with the live value as the fallback so absent
    // cvars are no-ops, and normalise the artist's raw vector. Stored into the
    // private target snapshot — the LIVE field is only touched here on the very
    // first apply (a snap), so a reload leaves the current live value for
    // update() to blend away from.
    glm::vec3 sd = CVar::Instance().getVec3("sun_direction", sunDirection);
    m_targets.sunDirection = (glm::length(sd) > 1e-4f) ? glm::normalize(sd) : sd;
    m_targets.sunColor        = CVar::Instance().getVec3("sun_color", sunColor);
    m_targets.sunIntensity    = CVar::Instance().getFloat("sun_intensity", sunIntensity);

    // --- Ambient / sky-light ------------------------------------------------
    m_targets.skyLightColor     = CVar::Instance().getVec3("sky_light_color", skyLightColor);
    m_targets.groundBounce      = CVar::Instance().getVec3("ground_bounce", groundBounce);
    m_targets.ambientStrength   = CVar::Instance().getFloat("ambient_strength", ambientStrength);
    m_targets.skyLightFillColor = CVar::Instance().getVec3("sky_light_fill_color", skyLightFillColor);
    m_targets.skyLightStrength  = CVar::Instance().getFloat("sky_light_strength", skyLightStrength);

    // --- Fog ----------------------------------------------------------------
    // fogEnabled / shadowsEnabled are discrete (bools) — applied immediately so
    // a toggle doesn't half-fade into invisibility.
    fogEnabled       = CVar::Instance().getString("fog_enabled", "true") != "false";
    m_targets.fogHorizon    = CVar::Instance().getVec3("fog_horizon", fogHorizon);
    m_targets.fogZenith     = CVar::Instance().getVec3("fog_zenith", fogZenith);
    m_targets.fogNear       = CVar::Instance().getFloat("fog_near", fogNear);
    m_targets.fogFar        = CVar::Instance().getFloat("fog_far", fogFar);
    m_targets.fogHeight     = CVar::Instance().getFloat("fog_height", fogHeight);
    m_targets.fogHeightFalloff = CVar::Instance().getFloat("fog_height_falloff", fogHeightFalloff);
    m_targets.fogMaxOpacity = CVar::Instance().getFloat("fog_max_opacity", fogMaxOpacity);

    m_targets.detailNormalStrength = CVar::Instance().getFloat("detail_normal_strength",
                                                                detailNormalStrength);

    shadowsEnabled = CVar::Instance().getString("shadows_enabled", "true") != "false";

    // First apply on a (fresh) environment: snap live == target so startup is
    // instant and the init log / first frame see the configured look. Later
    // reloads only touch `m_targets`, leaving update() to fade.
    if (!m_envApplied) {
        sunDirection   = m_targets.sunDirection;
        sunColor       = m_targets.sunColor;
        sunIntensity   = m_targets.sunIntensity;
        skyLightColor  = m_targets.skyLightColor;
        groundBounce   = m_targets.groundBounce;
        ambientStrength = m_targets.ambientStrength;
        skyLightFillColor = m_targets.skyLightFillColor;
        skyLightStrength  = m_targets.skyLightStrength;
        fogHorizon     = m_targets.fogHorizon;
        fogZenith      = m_targets.fogZenith;
        fogNear        = m_targets.fogNear;
        fogFar         = m_targets.fogFar;
        fogHeight      = m_targets.fogHeight;
        fogHeightFalloff = m_targets.fogHeightFalloff;
        fogMaxOpacity  = m_targets.fogMaxOpacity;
        detailNormalStrength = m_targets.detailNormalStrength;
        m_envApplied = true;
    }
}

void LightingEnvironment::update(float dt) {
    if (dt <= 0.0f) return;
    // Exponential approach: t = 1 - exp(-speed * dt) is frame-rate
    // independent and asymptotes at the target (no overshoot, no oscillation).
    const float t = 1.0f - std::exp(-m_lerpSpeed * dt);
    // Snap threshold: once within epsilon, lock to target to avoid endless
    // sub-ULP drift.
    if (t >= 1.0f) {
        sunDirection   = m_targets.sunDirection;
        sunColor       = m_targets.sunColor;
        sunIntensity   = m_targets.sunIntensity;
        skyLightColor  = m_targets.skyLightColor;
        groundBounce   = m_targets.groundBounce;
        ambientStrength = m_targets.ambientStrength;
        skyLightFillColor = m_targets.skyLightFillColor;
        skyLightStrength  = m_targets.skyLightStrength;
        fogHorizon     = m_targets.fogHorizon;
        fogZenith      = m_targets.fogZenith;
        fogNear        = m_targets.fogNear;
        fogFar         = m_targets.fogFar;
        fogHeight      = m_targets.fogHeight;
        fogHeightFalloff = m_targets.fogHeightFalloff;
        fogMaxOpacity  = m_targets.fogMaxOpacity;
        detailNormalStrength = m_targets.detailNormalStrength;
        return;
    }
    auto lerp = [t](auto& live, const auto& tgt) {
        live = glm::mix(live, tgt, t);
    };
    lerp(sunDirection,   m_targets.sunDirection);
    lerp(sunColor,       m_targets.sunColor);
    lerp(sunIntensity,   m_targets.sunIntensity);
    lerp(skyLightColor,  m_targets.skyLightColor);
    lerp(groundBounce,   m_targets.groundBounce);
    lerp(ambientStrength, m_targets.ambientStrength);
    lerp(skyLightFillColor, m_targets.skyLightFillColor);
    lerp(skyLightStrength,  m_targets.skyLightStrength);
    lerp(fogHorizon,     m_targets.fogHorizon);
    lerp(fogZenith,      m_targets.fogZenith);
    lerp(fogNear,        m_targets.fogNear);
    lerp(fogFar,         m_targets.fogFar);
    lerp(fogHeight,      m_targets.fogHeight);
    lerp(fogHeightFalloff, m_targets.fogHeightFalloff);
    lerp(fogMaxOpacity,  m_targets.fogMaxOpacity);
    lerp(detailNormalStrength, m_targets.detailNormalStrength);
    // Re-normalise the sun direction each frame (lerp does not preserve length).
    if (glm::length(sunDirection) > 1e-4f)
        sunDirection = glm::normalize(sunDirection);
}
