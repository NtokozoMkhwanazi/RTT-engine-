#pragma once
#ifndef UI_CACHE_H
#define UI_CACHE_H

#include "ecs/ECS.h"
#include <vector>

struct EntityCache {
    struct Entry {
        ecs::EntityID id;
        const char* icon;     // "C", "L", "M", "[]"
        bool hasGeo;
        uint32_t _padding;
    };
    std::vector<Entry> entries;
    bool dirty = true;
    float lastRebuildTime = 0.0f;
};

#endif
