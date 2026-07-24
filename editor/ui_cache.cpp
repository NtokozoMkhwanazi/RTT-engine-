#include "ui_cache.h"

#include "ecs/components/Components.h"
#include <GLFW/glfw3.h>

void EntityCache::clear() {
    entries.clear();
    dirty = true;
}

void EntityCache::markDirty() {
    dirty = true;
}

const EntityCache::Entry* EntityCache::get(std::size_t index) const {
    if (index >= entries.size()) {
        return nullptr;
    }
    return &entries[index];
}

const EntityCache::Entry* EntityCache::find(ecs::EntityID id) const {
    if (id == ecs::INVALID_ENTITY_ID) {
        return nullptr;
    }
    for (const auto& entry : entries) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

bool EntityCache::contains(ecs::EntityID id) const {
    return find(id) != nullptr;
}

void EntityCache::reserve(std::size_t capacity) {
    entries.reserve(capacity);
}

namespace UI {

void RebuildEntityCache(EntityCache& cache, const ecs::World& world) {
    cache.entries.clear();

    // Reserve to avoid reallocations
    std::size_t estimatedCount = world.getEntityCount();
    cache.entries.reserve(estimatedCount);

    // forEach is logically const (read-only iteration) but not marked const
    // in ecs::World; cast away const-ness for this read-only traversal.
    ecs::World& mutWorld = const_cast<ecs::World&>(world);

    // Single pass: collect all entities with TransformComponent
    // Use archetype iteration which is faster than forEach
    mutWorld.forEach<ecs::TransformComponent>([&](ecs::EntityID id, ecs::TransformComponent&) {
        EntityCache::Entry entry;
        entry.id = id;
        entry.hasGeo = false;

        // Determine icon type (minimal checks)
        if (world.hasComponent<ecs::CameraComponent>(ecs::Entity{id})) {
            entry.icon = "C";
        } else if (world.hasComponent<ecs::LightComponent>(ecs::Entity{id})) {
            entry.icon = "L";
        } else if (world.hasComponent<ecs::ModelComponent>(ecs::Entity{id})) {
            entry.icon = "M";
        } else {
            entry.icon = "[]";
        }

        if (world.hasComponent<ecs::GeospatialComponent>(ecs::Entity{id})) {
            entry.hasGeo = true;
        }

        cache.entries.push_back(entry);
    });

    cache.dirty = false;
    cache.lastRebuildTime = (float)glfwGetTime();
}

} // namespace UI
