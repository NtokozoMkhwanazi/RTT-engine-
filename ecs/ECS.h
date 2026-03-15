#pragma once

// ECS Core
#include "Entity.h"
#include "Component.h"
#include "EntityManager.h"
#include "ComponentManager.h"
#include "System.h"
#include "World.h"

/**
 * ECS (Entity Component System) Framework
 * 
 * Usage:
 * 
 * 1. Create a world:
 *    ecs::World world;
 *    world.init();
 * 
 * 2. Create entities and add components:
 *    auto entity = world.createEntity();
 *    auto& transform = world.addComponent<Transform>(entity, glm::vec3(0, 0, 0));
 *    auto& mesh = world.addComponent<Mesh>(entity, meshAsset);
 * 
 * 3. Add systems:
 *    auto& renderSystem = world.addSystem<RenderSystem>(renderer);
 *    auto& physicsSystem = world.addSystem<PhysicsSystem>(physicsWorld);
 * 
 * 4. Update and render:
 *    world.update(deltaTime);
 *    world.render();
 * 
 * 5. Query entities with specific components:
 *    world.forEach<Transform, Mesh>([&](Entity entity, Transform& t, Mesh& m) {
 *        // Process entity
 *    });
 */

namespace ecs {

// Re-export all core types
using EntityID = uint32_t;
using ComponentTypeID = size_t;

} // namespace ecs
