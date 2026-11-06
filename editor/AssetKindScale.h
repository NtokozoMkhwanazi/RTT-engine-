#pragma once

/**
 * AssetKindScale - shared viewport scale parity between the animated play
 * character and geo twins (Todo 7 / forGeo.txt §"forward buffers" scale bug).
 *
 * The animated play character (AnimatedCharacter) normalizes its authored FBX
 * height to targetHeight(1.8 m) * visualScale(0.7) = 1.26 m on load, so Shaw.fbx
 * always renders at 1.26 m regardless of each FBX's baked scale. Geo twins
 * used to render at raw authored meters (excavator.fbx huge, bot.fbx ~1.8 m),
 * producing a jarring size mismatch with the bot. Both the engine twin-spawn
 * path (CreateGeospatialTwin) and the editor live-telemetry path
 * (SynchronizeDigitalTwinTelemetryActors) now apply the SAME normalization so
 * twins match the play bot.
 *
 * NOTE: both render paths are the SAME forward skin-shader pass
 *   (modelRenderSystem.render / RenderPlayCharacter -> getModelShader); there
 *   is no forward/deferred buffer split. The scale parity is the sole fix.
 */
#include "../modelSystem/Model.h"   // Model, BoundingBox
#include "../geospatial/GeoAPI.h"    // geo::AssetKind
#include <glm/glm.hpp>

namespace Editor {

// Per-AssetKind visual size multiplier on top of the 1.26 m character-parity
// base, so each class reads at its real-world size (Personnel == play bot).
inline float assetKindSizeMultiplier(geo::AssetKind kind) {
    switch (kind) {
        case geo::AssetKind::HeavyMachinery:  return 2.0f;  // excavators ~2.5 m
        case geo::AssetKind::LightVehicle:    return 2.2f;  // cars / utility
        case geo::AssetKind::StaticSensor:    return 0.25f; // geophones / nodes
        case geo::AssetKind::Personnel:       return 1.0f;  // 1.26 m, matches play bot
        default:                              return 1.0f;
    }
}

// Shrink/expand a model to the same 1.26 m base the play bot renders at.
// `scale` is a caller multiplier (1.0 = match the character, 1.5 = 50% taller).
// Implemented with the model's baked axis-aligned bbox height (Size().y), the
// same metric AnimatedCharacter uses to compute its load-time scale, so the
// parity is consistent between the two render paths.
inline glm::vec3 characterParityScale(const Model* model, const glm::vec3& scale) {
    if (!model) return scale;
    const auto bb = model->GetBoundingBox();
    if (!bb.IsValid()) return scale;
    const float rawSizeY = bb.Size().y;
    if (rawSizeY <= 0.001f) return scale;
    constexpr float kCharTargetHeight = 1.8f;   // AnimatedCharacter targetHeight_
    constexpr float kCharVisualScale  = 0.7f;   // AnimatedCharacter visualScale
    const float parity = (kCharTargetHeight * kCharVisualScale) / rawSizeY;
    return scale * parity;
}

} // namespace Editor
