#ifndef EDITOR_ENTITY_MANAGER_H
#define EDITOR_ENTITY_MANAGER_H

#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include <glm/glm.hpp>
#include <string>

// ============================================================================
// Entity Creation and Management
// ============================================================================
namespace EntityManager {

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

ecs::Entity CreateAnimatedModel(const std::string& modelPath,
                                 const glm::vec3& pos = glm::vec3(0, 0, 0),
                                 const glm::vec3& scale = glm::vec3(1.0f),
                                 const glm::vec3& rotation = glm::vec3(0.0f),
                                 int animationIndex = 0);

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