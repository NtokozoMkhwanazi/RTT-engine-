#pragma once

// ============================================================================
// ECS Framework - Enhanced with Archetype Storage, Multithreading, and More
// ============================================================================
// 
// New Features:
// - Archetype-based storage for cache-coherent iteration (like Unity DOTS)
// - Multi-threaded system execution with job system
// - Entity relationship queries (parent/child hierarchies)
// - Event system for component changes
// - Serialization/deserialization (JSON and binary)
// - Blueprint/prefab system for reusable entity templates
//
// Usage:
//
// 1. Create a world with enhanced features:
//    ecs::World world;
//    world.init();
//
// 2. Use archetype-based queries for better performance:
//    world.forEach<Transform, Mesh>([&](Entity entity, Transform& t, Mesh& m) {
//        // Process entity - cache-coherent iteration
//    });
//
// 3. Use multi-threaded system execution:
//    world.setParallelExecution(true);
//
// 4. Use entity hierarchies:
//    world.setParent(childEntity, parentEntity);
//    auto children = world.getChildren(parentEntity);
//
// 5. Listen to component events:
//    world.eventSystem().subscribe<ComponentAddedEvent<Transform>>(
//        [](const auto& event) { /* handle event */ }
//    );
//
// 6. Save/load worlds:
//    world.saveToFile("savegame.json");
//    world.loadFromFile("savegame.json");
//
// 7. Use blueprints/prefabs:
//    world.registerBlueprint(myBlueprint);
//    auto instance = world.instantiateBlueprint("MyPrefab", position);
// ============================================================================

// Core ECS (legacy signature-based, kept for compatibility)
#include "Entity.h"
#include "Component.h"
#include "EntityManager.h"
#include "ComponentManager.h"
#include "System.h"
#include "World.h"

// New archetype-based storage
#include "Archetype.h"
#include "ArchetypeManager.h"

// Multi-threading
#include "JobSystem.h"

// Entity relationships
#include "RelationshipManager.h"

// Event system
#include "EventSystem.h"

// Serialization
#include "Serialization.h"

// Blueprints
#include "Blueprint.h"

namespace ecs {

// Re-export all types
using EntityID = uint32_t;
using ComponentTypeID = size_t;

} // namespace ecs
