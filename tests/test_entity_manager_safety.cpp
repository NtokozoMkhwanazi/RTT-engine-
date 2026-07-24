#include <gtest/gtest.h>
#include "editor/entity_manager.h"

TEST(EntityManagerSafety, InvalidEntityIDIsNotValid) {
    EXPECT_FALSE(EntityManager::IsValidEntityID(ecs::INVALID_ENTITY_ID));
}

TEST(EntityManagerSafety, NonExistentEntityDoesNotExist) {
    EXPECT_FALSE(EntityManager::EntityExists(999999));
}

TEST(EntityManagerSafety, ValidateOrClearClearsInvalid) {
    EXPECT_EQ(EntityManager::ValidateOrClear(999999), ecs::INVALID_ENTITY_ID);
}

TEST(EntityManagerSafety, SafeNameFallbackForInvalid) {
    EXPECT_EQ(EntityManager::GetSafeEntityName(ecs::INVALID_ENTITY_ID, "None"), "None");
}
