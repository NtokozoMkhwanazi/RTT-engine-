#ifndef EDITOR_ENTITY_MANAGER_H
#define EDITOR_ENTITY_MANAGER_H

#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "modelSystem/ModelHandle.h"  // A: ModelSystem::ModelHandle (by value) + ModelSystem::ModelRegistry
#include <glm/glm.hpp>
#include <string>

class AnimatedCharacter;  // A: unique_ptr param only (full def in AnimatedCharacter.h)

// Forward-declare the AssetKind enum (defined in geospatial/GeoAPI.h) so the
// editor entity-creation API can tag twins without dragging GeoAPI.h into
// this widely-included header.
namespace geo { enum class AssetKind : uint8_t; }

// ============================================================================
// Entity Creation and Management
// ============================================================================
namespace EntityManager {

// Spawn a geospatial AssetKind twin: transform + GeospatialComponent (with a
// WGS84 seed + AssetKind classification) + PredictionComponent, registered with
// the GeospatialSystem so it tracks on the dashboard (physics[] + geofence +
// prediction cone). External sim data later updates it by entityId.
ecs::Entity CreateGeospatialTwin(geo::AssetKind kind,
                                 const glm::vec3& pos = glm::vec3(0, 0, 0),
                                 double lat = -33.8568, double lon = 151.2153,
                                 double alt = 50.0);

// Primitives disabled - use models via CreateModel() instead
// Kept in primitives.h for future use
// ecs::Entity CreateCube(...);
// ecs::Entity CreateSphere(...);
// ecs::Entity CreatePlane(...);
// ecs::Entity CreateCylinder(...);
// ecs::Entity CreateCone(...);
// ecs::Entity CreateTorus(...);

ecs::Entity CreateLight(const glm::vec3& pos = glm::vec3(5, 10, 5),
                         const glm::vec3& color = glm::vec3(1, 1, 0.9f),
                         float intensity = 1.0f);

ecs::Entity CreateCamera(const glm::vec3& pos = glm::vec3(0, 5, 10),
                          const glm::vec3& target = glm::vec3(0, 0, 0));

// 3D Model loading functions
ecs::Entity CreateModel(const std::string& modelPath,
                         const glm::vec3& pos = glm::vec3(0, 0, 0),
                         const glm::vec3& scale = glm::vec3(1.0f),
                         const glm::vec3& rotation = glm::vec3(0.0f));

// Spawn a character entity from a model file. If the model has a skeleton
// and animations, it gets a ModelAnimatorComponent and is placed on the
// Characters layer. Static character meshes still get ModelComponent only.
// The entity name is derived from the model file basename for clarity in
// the Outliner. Spawned at `pos` (defaults to origin, or camera-forward if
// the editor provides a spawn ray).
ecs::Entity CreateCharacterEntity(const std::string& modelPath,
                                   const glm::vec3& pos = glm::vec3(0, 0, 0),
                                   const glm::vec3& scale = glm::vec3(1.0f),
                                   const glm::vec3& rotation = glm::vec3(0.0f));

ecs::Entity CreateAnimatedModel(const std::string& modelPath,
                                 const glm::vec3& pos = glm::vec3(0, 0, 0),
                                 const glm::vec3& scale = glm::vec3(1.0f),
                                 const glm::vec3& rotation = glm::vec3(0.0f),
                                 int animationIndex = 0);

// Spawn an NPC / character that SHARES the play character's full AnimatedCharacter
// (locomotion FSM + motion-matching + foot IK) instead of a single static-clip
// Animator. The entity carries a TransformComponent twin (pose-synced every frame
  // by ModelRenderSystem::update) so the selection-driven camera / focus system /
// gizmo can target it; it is rendered by ModelRenderSystem::renderAnimated() via the
// skin shader (mirrors RenderPlayCharacter). Scale is normalized to the play
// character's 1.26 m parity so NPCs match the bot regardless of source-asset height.
// `clipsDir` holds the locomotion clips (e.g. "assets"); `motionContext` selects the
// pose-search database ("Locomotion" | "Crouch" | "Combat" | "Capoeira" | "Dance").
ecs::Entity CreateAnimatedCharacterEntity(const std::string& modelPath,
                                          const std::string& clipsDir,
                                          const glm::vec3& pos = glm::vec3(0, 0, 0),
                                          const glm::vec3& scale = glm::vec3(1.0f),
                                          const glm::vec3& rotation = glm::vec3(0.0f),
                                          const std::string& motionContext = "Locomotion");

// A: async overload (forUI.txt §3). Attaches a worker-built AnimatedCharacter
// to a pre-registered GL Model (ModelSystem::ModelHandle `handle`) without
// re-parsing the FBX on the main thread. `handle` was registered from the
// Model::CreateFromAsync output (GL upload only); `character` was load +
// loadLocomotion + setMotionContext-built on the worker (all GL-free). The
// entity otherwise mirrors the sync path above.
ecs::Entity CreateAnimatedCharacterEntity(const glm::vec3& pos,
                                          const glm::vec3& scale,
                                          const glm::vec3& rotation,
                                          ModelSystem::ModelHandle handle,
                                          std::unique_ptr<AnimatedCharacter> character,
                                          const std::string& modelPath,
                                          const std::string& clipsDir,
                                          const std::string& motionContext = "Locomotion");

// Entity operations
void DeleteEntity(ecs::EntityID id);
ecs::EntityID DuplicateEntity(ecs::EntityID id);

// Primitive creation (transform + mesh component)
ecs::Entity CreatePrimitive(ecs::MeshType type,
                            const glm::vec3& pos = glm::vec3(0.0f));

// Blueprint create / spawn
bool CreateBlueprintFromSelected(ecs::EntityID id, const std::string& name);
ecs::EntityID SpawnBlueprint(const std::string& name,
                              const glm::vec3& pos = glm::vec3(0.0f));

// Get entity name
std::string GetEntityName(ecs::EntityID id);

// Derive a human-friendly entity name from an asset path (e.g.
// "assets/characters/bot7.fbx" → "bot7"). Used by CreateCharacterEntity so
// the Outliner shows a meaningful name instead of "Entity 42".
inline std::string DeriveEntityNameFromAsset(const std::string& assetPath) {
    namespace fs = std::filesystem;
    fs::path p(assetPath);
    std::string stem = p.stem().string();
    // Strip parenthetical suffixes like "bot7 (copy)" → "bot7"
    auto paren = stem.find('(');
    if (paren != std::string::npos) stem = stem.substr(0, paren);
    // Trim trailing whitespace
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '\t'))
        stem.pop_back();
    return stem.empty() ? "Character" : stem;
}

// Set entity name
void SetEntityName(ecs::EntityID id, const std::string& name);

// ============================================================================
// Entity Validation Helpers (UI Safety)
// ============================================================================

// Cheap syntactic check: id is not the sentinel invalid value.
inline bool IsValidEntityID(ecs::EntityID id) {
    return id != ecs::INVALID_ENTITY_ID;
}

// Full existence check: id is syntactically valid AND the world reports the entity as alive.
bool EntityExists(ecs::EntityID id);

// Returns id if it exists in the world, otherwise returns ecs::INVALID_ENTITY_ID.
// Useful for sanitising cached or stale handles before use.
ecs::EntityID ValidateOrClear(ecs::EntityID id);

// Returns the entity's name when available, or `fallback` for invalid/missing entities.
// Guaranteed not to crash for INVALID_ENTITY_ID or destroyed entities.
std::string GetSafeEntityName(ecs::EntityID id, const char* fallback = "Invalid Entity");

} // namespace EntityManager

#endif // EDITOR_ENTITY_MANAGER_H