#include "ui_cache.h"

#include "ecs/components/Components.h"
#include <cstring>
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

        const ecs::Entity e{id};
        entry.hasCamera = world.hasComponent<ecs::CameraComponent>(e);
        entry.hasLight = world.hasComponent<ecs::LightComponent>(e);
        entry.hasModel = world.hasComponent<ecs::ModelComponent>(e);
        entry.hasMesh = world.hasComponent<ecs::MeshComponent>(e);
        entry.hasGeo = world.hasComponent<ecs::GeospatialComponent>(e);

        // Per-type Phosphor icon + legacy letter label.
        using Kind = EntityCache::Kind;
        if (entry.hasCamera) {
            entry.kind = Kind::Camera;
            entry.icon = "C";
            entry.iconCodepoint = "\ue10e";   // camera
        } else if (entry.hasLight) {
            entry.kind = Kind::Light;
            entry.icon = "L";
            entry.iconCodepoint = "\ue2dc";   // lightbulb
        } else if (entry.hasModel) {
            entry.kind = Kind::Model;
            entry.icon = "M";
            entry.iconCodepoint = "\ue1da";   // cube
        } else if (entry.hasMesh) {
            entry.kind = Kind::Mesh;
            entry.icon = "[]";
            entry.iconCodepoint = "\uec7c";   // cube-transparent
        } else {
            entry.kind = Kind::Unknown;
            entry.icon = "[]";
            entry.iconCodepoint = "\ue1da";   // cube
        }
        if (entry.hasGeo) {
            entry.kind = Kind::Geo;
            entry.icon = "G";
            entry.iconCodepoint = "\ue316";   // map-pin
        }

        // Entity name, when present.
        if (const ecs::NameComponent* nameComp =
                world.getComponentArchetype<ecs::NameComponent>(e)) {
            const char* n = nameComp->getName();
            if (n && n[0]) {
                std::strncpy(entry.name, n, sizeof(entry.name) - 1);
                entry.name[sizeof(entry.name) - 1] = '\0';
            }
        }

        cache.entries.push_back(entry);
    });

    cache.dirty = false;
    cache.lastRebuildTime = (float)glfwGetTime();
}

} // namespace UI
