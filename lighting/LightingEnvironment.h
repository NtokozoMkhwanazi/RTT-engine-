#pragma once
// ============================================================================
// LightingEnvironment — the canonical, single-source-of-truth lighting model.
// ============================================================================
// ONE struct owns every lighting/fog/sky value the engine uses, so the terrain
// splat shader, the deferred/forward mesh lighting, the vegetation shader and
// the skybox all read from the SAME definition and can never drift apart (the
// old bug where each renderer hardcoded its own copy of the sun / fog tint).
//
// Standard workflow:
//   RenderPipeline init:  CVar::Instance().loadFromFile("config/cvars.ini");
//                         LightingEnvironment::Instance().applyCvars();
//   any renderer:         auto& L = LightingEnvironment::Instance();
//
// Artists tune config/cvars.ini and hit a hot-reload (or tweak a CVar at the
// console) to iterate on the look without recompiling.
// ============================================================================
#include <glm/glm.hpp>
#include "CVar.h"

// Canonical sun direction = the single numeric literal shared everywhere:
//   normalize(0.5, 1.0, 0.3) = (0.4320, 0.8639, 0.2592) — surface -> sun.
// Declared `inline const` (one definition across TUs) purely as the shipped
// DEFAULT. At runtime every renderer reads LightingEnvironment::Instance().
// sunDirection, which applyCvars() may override from config/cvars.ini.
inline const glm::vec3 kSunDirection =
    glm::normalize(glm::vec3(0.5f, 1.0f, 0.3f));

struct LightingEnvironment {
    // --- directional key sun (drives direct light + CSM cascades) ------------
    // Stored as surface -> sun (normalized). The shadow builder looks from the
    // sun along -sunDirection; the splat shader is handed -sunDirection as its
    // uLightDir (sunshine) and computes L = normalize(-uLightDir) = sunDirection.
    glm::vec3 sunDirection{0.5f, 1.0f, 0.3f}; // normalised on first apply
    glm::vec3 sunColor     = glm::vec3(1.00f, 0.98f, 0.92f); // warm daylight
    float     sunIntensity  = 1.0f;

    // --- ambient / sky-light -------------------------------------------------
    glm::vec3 skyLightColor   = glm::vec3(0.45f, 0.52f, 0.62f); // muted sky
    glm::vec3 groundBounce    = glm::vec3(0.35f, 0.24f, 0.12f); // warm earth
    float     ambientStrength  = 0.30f;      // ambient floor (ao lifts toward full)
    glm::vec3 skyLightFillColor = glm::vec3(0.60f, 0.52f, 0.42f); // warm shaded-side fill
    float     skyLightStrength  = 0.18f;     // strength of that warm fill

    // --- height fog ----------------------------------------------------------
    bool      fogEnabled       = true;
    glm::vec3 fogHorizon       = glm::vec3(0.45f, 0.38f, 0.32f); // warm mist
    glm::vec3 fogZenith        = glm::vec3(0.20f, 0.30f, 0.45f); // muted sky
    float     fogNear          = 150.0f;
    float     fogFar           = 900.0f;
    float     fogHeight        = 8.0f;    // mist pools below this elevation
    float     fogHeightFalloff = 0.08f;
    float     fogMaxOpacity    = 0.65f;

    // --- surface detail ------------------------------------------------------
    float detailNormalStrength = 0.45f;

    // --- shadows ------------------------------------------------------------
    bool  shadowsEnabled = true;
    int   cascadeCount   = 3;

    // --- temporal interpolation (see LightingEnvironment.cpp) ----------------
    // When an artist hot-reloads cvars.ini (or tweaks a CVar at the console),
    // applyCvars() writes the new values into a private `m_targets` snapshot.
    // update(dt) — normally driven once per frame by RenderPipeline::beginFrame
    // — then fades the live fields above toward those targets at `m_lerpSpeed`
    // units/sec, so the lighting/scene smoothly transitions instead of
    // snapping. On the very first applyCvars() the live fields are snapped to
    // target (instant, no startup fade); only *later* reloads fade.
    void update(float dt);

    // --- access -------------------------------------------------------------
    static LightingEnvironment& Instance() {
        static LightingEnvironment env;
        return env;
    }
    static LightingEnvironment Default() { LightingEnvironment e; e.applyCvars(); return e; }

    // Override fields from the CVar registry (no-op for absent keys), then
    // normalise the sun direction once.
    void applyCvars();

private:
    // Per-field targets written by applyCvars(); `update()` blends toward these.
    struct Targets {
        glm::vec3 sunDirection{0.5f, 1.0f, 0.3f}; // normalised in applyCvars
        glm::vec3 sunColor     = glm::vec3(1.00f, 0.98f, 0.92f);
        float     sunIntensity  = 1.0f;
        glm::vec3 skyLightColor   = glm::vec3(0.45f, 0.52f, 0.62f);
        glm::vec3 groundBounce    = glm::vec3(0.35f, 0.24f, 0.12f);
        float     ambientStrength  = 0.30f;
        glm::vec3 skyLightFillColor = glm::vec3(0.60f, 0.52f, 0.42f);
        float     skyLightStrength  = 0.18f;
        glm::vec3 fogHorizon       = glm::vec3(0.45f, 0.38f, 0.32f);
        glm::vec3 fogZenith        = glm::vec3(0.20f, 0.30f, 0.45f);
        float     fogNear          = 150.0f;
        float     fogFar           = 900.0f;
        float     fogHeight        = 8.0f;
        float     fogHeightFalloff = 0.08f;
        float     fogMaxOpacity    = 0.65f;
        float detailNormalStrength = 0.45f;
    } m_targets;

    bool  m_envApplied{false};     // first applyCvars() snaps (no startup fade)
    float m_lerpSpeed{8.0f};       // blend rate: ~99% reach in ~0.5s at 60Hz
};
