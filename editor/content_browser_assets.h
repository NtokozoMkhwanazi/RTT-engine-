#pragma once
/**
 * content_browser_assets.h
 *
 * Asset categorisation for the content browser. Each scanned model file is
 * classified into one of three groups — Character, Animation, Prop — so the
 * browser can display a clear, hierarchical tree instead of a flat file list.
 *
 * Categorisation is based on:
 *   - Directory structure (assets/characters/, assets/World_objects/, etc.)
 *   - File-name heuristics (Idle.fbx, Run.fbx → animation clips; character.fbx,
 *     Girl.fbx, Brute.fbx → character models)
 *   - Motion-context matching (capoeira clips, Male Locomotion Pack clips, etc.)
 */

#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

namespace UI {

// Mirror of EditorState::AssetCategory (kept here to avoid pulling
// editor_state.h into every translation unit that just wants to classify).
enum class AssetCategory : int8_t {
    Character = 0,
    Animation = 1,
    Prop = 2,
    Other = 3,
    Texture = 4,
};

// Motion-context groups for Animation assets. The index maps 1:1 to
// AnimatedCharacter::MotionContext (minus DANCE which has its own clips).
enum AnimationContext : int {
    CTX_LOCOMOTION = 0,
    CTX_CROUCH,
    CTX_COMBAT,
    CTX_CAPOEIRA,
    CTX_DANCE,
    CTX_COUNT,
};

struct AssetInfo {
    std::string path;          // Full filesystem path
    std::string displayName;   // File stem, cleaned up for display
    AssetCategory category;
    AnimationContext context;  // Meaningful only for Animation assets
    bool hasSkeleton = false;  // Cached: true if model load found bones
    bool hasAnimations = false;
};

// Animation clip file-name fragments mapped to their motion context.
// Checked as a substring (case-insensitive) against the file stem.
inline const char* kLocomotionClipNames[] = {
    "idle", "walk", "run", "jog", "strafe", "turn", "jumptostart",
    "land", "slide", "vault", "climb", "crouch to stand"
};
inline const char* kCrouchClipNames[] = {
    "crouch", "duck", "low walk", "crawl", "prone"
};
inline const char* kCombatClipNames[] = {
    "boxing", "punch", "kick", "block", "defeat", "taunt", "hit", "attack",
    "swing", "strike"
};
inline const char* kDanceClipNames[] = {
    "dance", "rumba", "hip hop", "taunt", "celebrate", "victory", "social"
};

// Directories whose contents are categorised as Animation clips.
inline bool IsAnimationDirectory(const std::string& dirPath) {
    namespace fs = std::filesystem;
    std::string lower = dirPath;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    // Capoeira animation clips directory
    if (lower.find("capoeira") != std::string::npos) return true;
    return false;
}

// Directories whose top-level files are character models.
inline bool IsCharacterDirectory(const std::string& dirPath) {
    namespace fs = std::filesystem;
    std::string lower = dirPath;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("characters") != std::string::npos) return true;
    if (lower.find("world_objects") != std::string::npos) return true;
    // "Male Locomotion Pack" — Shaw.fbx is the character, the rest are clips
    if (lower.find("male locomotion pack") != std::string::npos) return false;
    return false;
}

// Known standalone character model file names (root-level assets/).
inline bool IsKnownCharacterFile(const std::string& stem) {
    std::string lower = stem;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    static const char* kCharFiles[] = {
        "character", "girl", "brute", "bear_demo", "bear", "zombie",
        "bot", "bot4", "bot5", "bot7", "enemy", "npc", "actor", "hero",
        "marcus", "shaw", "male", "female", "soldier", "guard",
        "capoeira"  // capoeira.fbx is a character model, not a clip
    };
    for (const char* name : kCharFiles) {
        if (lower == name) return true;
    }
    // "bot (copy)" etc. — strip suffixes
    auto paren = lower.find('(');
    if (paren != std::string::npos) {
        std::string base = lower.substr(0, paren);
        // trim trailing spaces
        while (!base.empty() && base.back() == ' ') base.pop_back();
        for (const char* name : kCharFiles) {
            if (base == name) return true;
        }
    }
    return false;
}

// Check if a file stem matches any of the clip-name fragments for a context.
inline bool MatchesClipName(const std::string& stem, const char* const* names, size_t count) {
    std::string lower = stem;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    // Normalize: replace spaces/hyphens with nothing for matching
    std::string norm;
    for (char c : lower) {
        if (c != ' ' && c != '-') norm += c;
    }
    for (size_t i = 0; i < count; ++i) {
        std::string pattern = names[i];
        std::string patNorm;
        for (char c : pattern) {
            if (c != ' ' && c != '-') patNorm += c;
        }
        if (norm.find(patNorm) != std::string::npos) return true;
    }
    return false;
}

inline bool IsLocomotionClip(const std::string& stem) {
    return MatchesClipName(stem, kLocomotionClipNames,
        sizeof(kLocomotionClipNames) / sizeof(kLocomotionClipNames[0]));
}
inline bool IsCrouchClip(const std::string& stem) {
    return MatchesClipName(stem, kCrouchClipNames,
        sizeof(kCrouchClipNames) / sizeof(kCrouchClipNames[0]));
}
inline bool IsCombatClip(const std::string& stem) {
    return MatchesClipName(stem, kCombatClipNames,
        sizeof(kCombatClipNames) / sizeof(kCombatClipNames[0]));
}
inline bool IsDanceClip(const std::string& stem) {
    return MatchesClipName(stem, kDanceClipNames,
        sizeof(kDanceClipNames) / sizeof(kDanceClipNames[0]));
}

// Determine the animation context for an animation clip.
inline AnimationContext GetAnimationContext(const std::string& stem, const std::string& dirPath) {
    // Capoeira directory → combat/capoeira context
    if (IsAnimationDirectory(dirPath)) return CTX_CAPOEIRA;
    if (IsCrouchClip(stem)) return CTX_CROUCH;
    if (IsCombatClip(stem)) return CTX_COMBAT;
    if (IsDanceClip(stem)) return CTX_DANCE;
    return CTX_LOCOMOTION;
}

// Categorize a single asset file. Returns Character / Animation / Prop / Other / Texture.
// Classification logic:
//  1. Known texture formats (png, jpg, tga, hdr, exr, ktx, dds) → Texture
//  2. Files in assets/characters/ or assets/World_objects/ → Character
//  3. Files in "Capoeira animiation clips/" → Animation (Capoeira context)
//  4. Files in "Male " / "Male Locomotion Pack/" (non-Shaw) → Animation
//  5. Files matching known character names (character.fbx, Girl.fbx, etc.) → Character
//  6. Files matching animation clip names (Idle, Walk, Run, etc.) → Animation
//  7. Everything else → Prop
// ============================================================================
inline bool IsTextureFile(const std::string& path) {
    namespace fs = std::filesystem;
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    static const char* texExts[] = {
        ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".gif",
        ".hdr", ".exr", ".ktx", ".dds", ".tiff", ".tif",
        ".psd", ".xcf", ".qoi", ".ppm"
    };
    for (const char* e : texExts) {
        if (ext == e) return true;
    }
    return false;
}

inline AssetCategory CategorizeAsset(const std::string& fullPath) {
    namespace fs = std::filesystem;
    fs::path p(fullPath);
    std::string stem = p.stem().string();
    std::string dirPath = p.parent_path().string();

    // 0. Texture files (png, jpg, tga, hdr, exr, ktx, dds, etc.)
    if (IsTextureFile(fullPath)) return AssetCategory::Texture;

    // 1. Character directories
    if (IsCharacterDirectory(dirPath)) return AssetCategory::Character;

    // 2. Animation directories
    if (IsAnimationDirectory(dirPath)) return AssetCategory::Animation;

    // 3. "Male " directory clips (not Shaw.fbx which is a character)
    std::string lowerDir = dirPath;
    std::transform(lowerDir.begin(), lowerDir.end(), lowerDir.begin(), ::tolower);
    if (lowerDir.find("male") != std::string::npos && stem != "Shaw" && stem != "Pumpkinhulk") {
        // Files in "Male /" are walk/idle/run clips
        if (stem != "Shaw" && stem != "Pumpkinhulk L Shaw") return AssetCategory::Animation;
    }

    // 4. Known character files
    if (IsKnownCharacterFile(stem)) return AssetCategory::Character;

    // 5. Animation clip patterns
    if (IsLocomotionClip(stem) || IsCrouchClip(stem) || IsCombatClip(stem) || IsDanceClip(stem))
        return AssetCategory::Animation;

    // 6. World object directories → Prop
    if (lowerDir.find("world_objects") != std::string::npos)
        return AssetCategory::Prop;

    // 7. Environment asset directories → Prop
    static const char* kPropDirs[] = {
        "grass", "rock", "tree", "boulder", "coast", "drill", "source"
    };
    for (const char* d : kPropDirs) {
        if (lowerDir.find(d) != std::string::npos) return AssetCategory::Prop;
    }

    return AssetCategory::Other;
}

// Build a clean display name from a path.
// "assets/characters/bot7.fbx" → "bot7"
// "assets/Capoeira animiation clips/guarda.fbx" → "guarda"
inline std::string BuildDisplayName(const std::string& path) {
    namespace fs = std::filesystem;
    fs::path p(path);
    std::string stem = p.stem().string();
    // Strip "(copy)" suffix
    auto paren = stem.find('(');
    if (paren != std::string::npos) stem = stem.substr(0, paren);
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '\t'))
        stem.pop_back();
    return stem;
}

// Scan and categorise all model assets under `root`.
inline std::vector<AssetInfo> ScanAndCategorize(const std::string& root) {
    namespace fs = std::filesystem;
    std::vector<AssetInfo> assets;
    try {
        std::string scanRoot = root;
        if (scanRoot.empty()) scanRoot = "assets";
        if (!fs::exists(scanRoot)) scanRoot = ".";
        for (auto& entry : fs::recursive_directory_iterator(scanRoot,
                fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            // Lowercase comparison for extension
            std::string extLower = ext;
            std::transform(extLower.begin(), extLower.end(), extLower.begin(), ::tolower);
            if (extLower != ".fbx" && extLower != ".gltf" && extLower != ".glb" &&
                extLower != ".obj" && extLower != ".dae" && !IsTextureFile(entry.path().string()))
                continue;
            AssetInfo ai;
            ai.path = entry.path().string();
            ai.displayName = BuildDisplayName(ai.path);
            ai.category = CategorizeAsset(ai.path);
            if (ai.category == AssetCategory::Animation) {
                ai.context = GetAnimationContext(ai.displayName, entry.path().parent_path().string());
            } else {
                ai.context = CTX_LOCOMOTION;
            }
            assets.push_back(std::move(ai));
        }
    } catch (const std::exception& e) {
        // Caller handles error logging
    }
    return assets;
}

} // namespace UI
