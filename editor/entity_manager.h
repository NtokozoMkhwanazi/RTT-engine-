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
void DuplicateEntity(ecs::EntityID id);

// Get entity name
std::string GetEntityName(ecs::EntityID id);

// Set entity name
void SetEntityName(ecs::EntityID id, const std::string& name);

} // namespace EntityManager

#endif // EDITOR_ENTITY_MANAGER_H