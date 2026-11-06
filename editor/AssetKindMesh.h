#pragma once

/**
 * AssetKindMesh - map a geo::AssetKind twin classification to the render mesh
 * used for that twin in the editor's play mode (Todo 7).
 *
 * Standalone, GL-free and unit-testable: the editor's renderPlayCharacter
 * consults this mapping and falls back to assets/bot.fbx (the human stand-in)
 * whenever the chosen mesh file is missing - so every kind always renders
 * something while the dedicated excavator/utility_car/miner/sensor_node assets
 * are authored.
 */
#include "../geospatial/GeoAPI.h"   // geo::AssetKind
#include <filesystem>              // pack-dir enumeration (resolvePlayCharacterModelFromPack)
#include <vector>
#include <cctype>

namespace Editor {

// Returns a mesh path for the given twin kind. The path may not exist on disk
// yet; renderPlayCharacter handles a failed load gracefully.
inline const char* meshPathForAssetKind(geo::AssetKind kind) {
    switch (kind) {
        case geo::AssetKind::Personnel:      return "assets/bot.fbx";       // humanoid stand-in (6.9 MB)
        case geo::AssetKind::HeavyMachinery: return "assets/excavator.fbx"; // real tracked asset
        case geo::AssetKind::LightVehicle:   return "assets/utility_car.fbx";
        case geo::AssetKind::StaticSensor:   return "assets/sensor_node.fbx";
        case geo::AssetKind::Unknown:
        default:                             return "assets/bot.fbx";
    }
}

// The universal fallback mesh used when a kind's asset is unavailable on disk.
inline const char* fallbackMeshPath() { return "assets/bot.fbx"; }

// forUI.txt §1 fix (pack load "falls to bot.fbx"): the Unified Pack Loader /
// Model-path inspector sets playCharacterModel (the artist's FBX). The render
// path must prefer that user-chosen path so the viewport mesh matches the
// Animator (loaded from the SAME path, so skeleton and mesh never mismatch),
// instead of silently falling back to the AssetKind stand-in (bot.fbx). Only
// fall back to the kind-based path when no explicit model has been loaded.
// GL-free + unit-testable.
inline std::string resolvePlayCharacterMeshPath(const std::string& modelPath,
                                                geo::AssetKind kind) {
    if (!modelPath.empty()) return modelPath;
    return std::string(meshPathForAssetKind(kind));
}

// forUI.txt §2 fix (pack load "falls to bot.fbx"): the Unified Pack Loader
// used to hardcode packPath+"/bot.fbx" (Shaw.fbx fallback), so only packs named
// bot/Shaw ever loaded. Now the pack dir is enumerated for ANY skinned model
// (.fbx/.gltf/.glb) - .fbx first (skeleton mesh conventions), then glTF/GLB -
// with the legacy bot.fbx/Shaw.fbx kept as a last-resort fallback so old packs
// still boot and the Model loader handles a missing file gracefully.
// GL-free + unit-testable (no Editor/Render dependency).
inline std::string resolvePlayCharacterModelFromPack(const std::string& packPath) {
    static const std::vector<std::string> extensions = {".fbx", ".gltf", ".glb"};
    std::error_code ec;
    for (const auto& ext : extensions) {
        for (const auto& ent :
                 std::filesystem::directory_iterator(packPath,
                     std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) { ec.clear(); continue; }
            if (!ent.is_regular_file(ec) || ec) { ec.clear(); continue; }
            std::string fe = ent.path().extension().string();
            for (auto& c : fe) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (fe == ext) return ent.path().string();
        }
    }
    // Legacy fallback for packs still using the bot.fbx/Shaw.fbx convention.
    if (std::filesystem::exists(packPath + "/bot.fbx"))  return packPath + "/bot.fbx";
    return packPath + "/Shaw.fbx";  // loader handles a missing file gracefully
}

} // namespace Editor
