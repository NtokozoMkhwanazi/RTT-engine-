#ifndef UI_CACHE_H
#define UI_CACHE_H

#include "ecs/ECS.h"
#include <vector>
#include <cstddef>

struct EntityCache {
    struct Entry {
        ecs::EntityID id = ecs::INVALID_ENTITY_ID;
        const char* icon = "";
        bool hasGeo = false;
        uint32_t _padding = 0;

        bool isValid() const { return id != ecs::INVALID_ENTITY_ID; }
    };

    std::vector<Entry> entries;
    bool dirty = true;
    float lastRebuildTime = 0.0f;

    void clear();
    void markDirty();
    bool empty() const { return entries.empty(); }
    std::size_t size() const { return entries.size(); }

    const Entry* get(std::size_t index) const;
    const Entry* find(ecs::EntityID id) const;
    bool contains(ecs::EntityID id) const;
    void reserve(std::size_t capacity);
};

namespace UI {
    void RebuildEntityCache(EntityCache& cache, const ecs::World& world);
}

#endif
