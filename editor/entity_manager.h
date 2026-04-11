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

// Entity creation functions
ecs::Entity CreateCube(const glm::vec3& pos = glm::vec3(0, 1, 0), 
                       const glm::vec3& scale = glm::vec3(1),
                       const glm::vec3& color = glm::vec3(0.8f));

ecs::Entity CreateSphere(const glm::vec3& pos = glm::vec3(0, 1, 0),
                         float radius = 0.5f,
                         const glm::vec3& color = glm::vec3(0.8f));

ecs::Entity CreatePlane(const glm::vec3& pos = glm::vec3(0, 0, 0),
                        const glm::vec2& size = glm::vec2(10),
                        const glm::vec3& color = glm::vec3(0.5f));

ecs::Entity CreateCylinder(const glm::vec3& pos = glm::vec3(0, 1, 0),
                           float radius = 0.5f,
                           float height = 1.0f,
                           const glm::vec3& color = glm::vec3(0.8f));

ecs::Entity CreateCone(const glm::vec3& pos = glm::vec3(0, 1, 0),
                       float radius = 0.5f,
                       float height = 1.0f,
                       const glm::vec3& color = glm::vec3(0.8f));

ecs::Entity CreateTorus(const glm::vec3& pos = glm::vec3(0, 1, 0),
                        float majorRadius = 0.35f,
                        float minorRadius = 0.15f,
                        const glm::vec3& color = glm::vec3(0.8f));

ecs::Entity CreateLight(const glm::vec3& pos = glm::vec3(5, 10, 5),
                        const glm::vec3& color = glm::vec3(1, 1, 0.9f),
                        float intensity = 1.0f);

ecs::Entity CreateCamera(const glm::vec3& pos = glm::vec3(0, 5, 10),
                         const glm::vec3& target = glm::vec3(0, 0, 0));

// Entity operations
void DeleteEntity(ecs::EntityID id);
void DuplicateEntity(ecs::EntityID id);

// Get entity name
std::string GetEntityName(ecs::EntityID id);

// Set entity name
void SetEntityName(ecs::EntityID id, const std::string& name);

} // namespace EntityManager

#endif // EDITOR_ENTITY_MANAGER_H
