#include <gtest/gtest.h>
#include "editor/ui_cache.h"

TEST(EntityCache, EmptyCacheGetReturnsNull) {
    EntityCache cache;
    EXPECT_EQ(cache.get(0), nullptr);
}

TEST(EntityCache, MarkDirtyIsDirty) {
    EntityCache cache;
    cache.dirty = false;
    cache.markDirty();
    EXPECT_TRUE(cache.dirty);
}

TEST(EntityCache, FindInvalidReturnsNull) {
    EntityCache cache;
    EXPECT_EQ(cache.find(ecs::INVALID_ENTITY_ID), nullptr);
}
