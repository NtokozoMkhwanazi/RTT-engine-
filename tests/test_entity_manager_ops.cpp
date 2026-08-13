#include <gtest/gtest.h>
#include "editor/editor_state.h"
#include "editor/entity_manager.h"
#include "ecs/components/Components.h"
#include <memory>
#include <algorithm>

// ============================================================================
// EntityManager operation tests (delete / duplicate / primitives / blueprints).
//
// These exercise the ECS-backed editor operations on the global editor's
// world. Entity creation + archetype storage work without a GL context, so
// full Editor::initialize() is intentionally NOT called here (the EntityManager
// constructor pre-fills the entity-id pool, so createEntity() needs no init).
//
// NOTE: all suites share the global g_editor.world(), so entities and
// blueprints ACCUMULATE across tests for the rest of the process. Every test
// asserts only on entities it created itself - do not add tests that depend on
// total entity/blueprint counts here.
//
// NOTE: the entity operations below also record undo history (undo_redo.h),
// which therefore accumulates across these tests too. UndoRedoTest clears the
// history at the start of each of its cases - if you add a new suite that
// asserts on UndoRedo::CanUndo(), call UndoRedo::Clear() first.
// ============================================================================

namespace {

ecs::World& W() { return g_editor.world(); }

ecs::EntityID MakeTransformEntity(const glm::vec3& pos = glm::vec3(0.0f)) {
    ecs::Entity e = W().createEntityWithComponents<ecs::TransformComponent>();
    if (auto* t = W().getComponentArchetype<ecs::TransformComponent>(e)) t->position = pos;
    return e.id;
}

} // namespace

// ----------------------------------------------------------------------------
// DeleteEntity
// ----------------------------------------------------------------------------

TEST(EntityManagerOps, DeleteEntityRemovesFromWorldAndArchetype) {
    const ecs::EntityID id = MakeTransformEntity(glm::vec3(1, 2, 3));
    ASSERT_TRUE(EntityManager::EntityExists(id));
    ASSERT_NE(W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id}), nullptr);

    EntityManager::DeleteEntity(id);

    EXPECT_FALSE(EntityManager::EntityExists(id));
    // Archetype storage must be gone too, or render systems would keep drawing it.
    EXPECT_EQ(W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id}), nullptr);
}

TEST(EntityManagerOps, DeleteEntityClearsSelection) {
    const ecs::EntityID id = MakeTransformEntity();
    g_editor.setSelectedEntity(id);
    EntityManager::DeleteEntity(id);
    EXPECT_EQ(g_editor.selectedEntity(), ecs::INVALID_ENTITY_ID);
}

TEST(EntityManagerOps, DeleteEntityRefusesPlayableCharacter) {
    ecs::Entity e = W().createEntityWithComponents<ecs::TransformComponent, ecs::NameComponent>();
    ASSERT_TRUE(e.isValid());
    if (auto* n = W().getComponentArchetype<ecs::NameComponent>(e)) n->setName("Bot Player");

    EntityManager::DeleteEntity(e.id);

    // The playable character is engine-owned; deleting it is refused.
    EXPECT_TRUE(EntityManager::EntityExists(e.id));
}

TEST(EntityManagerOps, DeleteInvalidIsSafe) {
    EntityManager::DeleteEntity(ecs::INVALID_ENTITY_ID);  // must not crash/assert
    EntityManager::DeleteEntity(999999);
    SUCCEED();
}

// ----------------------------------------------------------------------------
// DuplicateEntity
// ----------------------------------------------------------------------------

TEST(EntityManagerOps, DuplicateMeshEntityCopiesTransformAndMesh) {
    const ecs::EntityID src = [&]() {
        ecs::Entity e = W().createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
        if (auto* t = W().getComponentArchetype<ecs::TransformComponent>(e)) {
            t->position = glm::vec3(5, 6, 7);
            t->scale = glm::vec3(2.0f);
        }
        if (auto* m = W().getComponentArchetype<ecs::MeshComponent>(e)) {
            m->meshType = ecs::MeshType::Sphere;
            m->meshID = 1;
            m->color = glm::vec3(1.0f, 0.0f, 0.0f);
        }
        return e.id;
    }();

    const ecs::EntityID dup = EntityManager::DuplicateEntity(src);

    ASSERT_NE(dup, ecs::INVALID_ENTITY_ID);
    ASSERT_NE(dup, src);
    EXPECT_TRUE(EntityManager::EntityExists(dup));

    auto* dt = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{dup});
    ASSERT_NE(dt, nullptr);
    EXPECT_EQ(dt->position, glm::vec3(6, 6, 7));  // offset by +1 X
    EXPECT_EQ(dt->scale, glm::vec3(2.0f));

    auto* dm = W().getComponentArchetype<ecs::MeshComponent>(ecs::Entity{dup});
    ASSERT_NE(dm, nullptr);
    EXPECT_EQ(dm->meshType, ecs::MeshType::Sphere);
    EXPECT_EQ(dm->color, glm::vec3(1.0f, 0.0f, 0.0f));
}

TEST(EntityManagerOps, DuplicateCopiesNameWithSuffix) {
    ecs::Entity src = W().createEntityWithComponents<ecs::TransformComponent, ecs::NameComponent>();
    ASSERT_TRUE(src.isValid());
    if (auto* n = W().getComponentArchetype<ecs::NameComponent>(src)) n->setName("Original");

    const ecs::EntityID dup = EntityManager::DuplicateEntity(src.id);

    ASSERT_NE(dup, ecs::INVALID_ENTITY_ID);
    auto* dn = W().getComponentArchetype<ecs::NameComponent>(ecs::Entity{dup});
    ASSERT_NE(dn, nullptr);
    EXPECT_STREQ(dn->getName(), "Original_copy");
}

TEST(EntityManagerOps, DuplicateInvalidReturnsInvalid) {
    EXPECT_EQ(EntityManager::DuplicateEntity(ecs::INVALID_ENTITY_ID), ecs::INVALID_ENTITY_ID);
    EXPECT_EQ(EntityManager::DuplicateEntity(999999), ecs::INVALID_ENTITY_ID);
}

// ----------------------------------------------------------------------------
// CreatePrimitive
// ----------------------------------------------------------------------------

TEST(EntityManagerOps, CreatePrimitiveAddsMeshComponent) {
    ecs::Entity e = EntityManager::CreatePrimitive(ecs::MeshType::Cube, glm::vec3(0, 1, 0));
    ASSERT_TRUE(e.isValid());

    auto* t = W().getComponentArchetype<ecs::TransformComponent>(e);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->position, glm::vec3(0, 1, 0));

    auto* m = W().getComponentArchetype<ecs::MeshComponent>(e);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->meshType, ecs::MeshType::Cube);
    EXPECT_TRUE(m->visible);
}

// ----------------------------------------------------------------------------
// Blueprint create / spawn / registry
// ----------------------------------------------------------------------------

TEST(EntityManagerOps, BlueprintCreateAndSpawnRoundTrip) {
    ecs::Entity src = W().createEntityWithComponents<ecs::TransformComponent, ecs::NameComponent>();
    ASSERT_TRUE(src.isValid());
    if (auto* t = W().getComponentArchetype<ecs::TransformComponent>(src)) {
        t->position = glm::vec3(10, 0, 0);
        t->scale = glm::vec3(3.0f);
    }
    if (auto* n = W().getComponentArchetype<ecs::NameComponent>(src)) n->setName("TestMarker");

    ASSERT_TRUE(EntityManager::CreateBlueprintFromSelected(src.id, "TestBP"));
    ASSERT_NE(W().getBlueprint("TestBP"), nullptr);
    const auto names = W().getBlueprintNames();
    EXPECT_NE(std::find(names.begin(), names.end(), "TestBP"), names.end());

    const ecs::EntityID spawned = EntityManager::SpawnBlueprint("TestBP", glm::vec3(1, 0, 0));
    ASSERT_NE(spawned, ecs::INVALID_ENTITY_ID);
    ASSERT_TRUE(EntityManager::EntityExists(spawned));

    auto* st = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{spawned});
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(st->position, glm::vec3(11, 0, 0));  // stored (10,0,0) + spawn offset (1,0,0)
    EXPECT_EQ(st->scale, glm::vec3(3.0f));

    auto* sn = W().getComponentArchetype<ecs::NameComponent>(ecs::Entity{spawned});
    ASSERT_NE(sn, nullptr);
    EXPECT_STREQ(sn->getName(), "TestMarker");
}

TEST(EntityManagerOps, CreateBlueprintFromInvalidFails) {
    EXPECT_FALSE(EntityManager::CreateBlueprintFromSelected(ecs::INVALID_ENTITY_ID, "Nope"));
    EXPECT_FALSE(EntityManager::CreateBlueprintFromSelected(999999, "Nope"));
}

TEST(EntityManagerOps, SpawnMissingBlueprintReturnsInvalid) {
    EXPECT_EQ(EntityManager::SpawnBlueprint("DoesNotExist", glm::vec3(0)), ecs::INVALID_ENTITY_ID);
}

TEST(EntityManagerOps, BlueprintRegistryRegisterUnregister) {
    W().registerBlueprint(std::make_unique<ecs::Blueprint>("TempBP"));
    ASSERT_NE(W().getBlueprint("TempBP"), nullptr);
    W().unregisterBlueprint("TempBP");
    EXPECT_EQ(W().getBlueprint("TempBP"), nullptr);
}

// ----------------------------------------------------------------------------
// CreateLight / CreateCamera (wired into the Add menu)
// ----------------------------------------------------------------------------

TEST(EntityManagerOps, CreateLightCreatesEntityWithLightComponent) {
    const glm::vec3 pos(3.0f, 4.0f, 5.0f);
    const glm::vec3 color(1.0f, 0.3f, 0.2f);
    ecs::Entity e = EntityManager::CreateLight(pos, color, 2.5f);
    ASSERT_TRUE(e.isValid());

    auto* t = W().getComponentArchetype<ecs::TransformComponent>(e);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->position, pos);
    EXPECT_EQ(t->scale, glm::vec3(0.2f));  // small gizmo-scale marker

    auto* l = W().getComponentArchetype<ecs::LightComponent>(e);
    ASSERT_NE(l, nullptr);
    EXPECT_EQ(l->type, ecs::LightType::POINT);
    EXPECT_EQ(l->color, color);
    EXPECT_FLOAT_EQ(l->intensity, 2.5f);
}

TEST(EntityManagerOps, CreateCameraCreatesEntityWithActiveCameraComponent) {
    ecs::Entity e = EntityManager::CreateCamera(glm::vec3(0.0f, 5.0f, 10.0f), glm::vec3(0.0f));
    ASSERT_TRUE(e.isValid());

    auto* t = W().getComponentArchetype<ecs::TransformComponent>(e);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->position, glm::vec3(0.0f, 5.0f, 10.0f));

    auto* c = W().getComponentArchetype<ecs::CameraComponent>(e);
    ASSERT_NE(c, nullptr);
    EXPECT_TRUE(c->isActive);
}

TEST(EntityManagerOps, CreateModelInvalidPathReturnsInvalidEntity) {
    // Missing file must fail cleanly without crashing or creating an entity.
    ecs::Entity e = EntityManager::CreateModel("assets/does_not_exist.fbx", glm::vec3(0), glm::vec3(1), glm::vec3(0));
    EXPECT_FALSE(e.isValid());
}

TEST(EntityManagerOps, CreateAnimatedModelInvalidPathReturnsInvalidEntity) {
    ecs::Entity e = EntityManager::CreateAnimatedModel("assets/does_not_exist.fbx", glm::vec3(0), glm::vec3(1), glm::vec3(0), 0);
    EXPECT_FALSE(e.isValid());
}

// ----------------------------------------------------------------------------
// Name helpers (GetEntityName / SetEntityName / GetSafeEntityName / ValidateOrClear)
// ----------------------------------------------------------------------------

TEST(EntityManagerOps, SetAndGetEntityNameRoundTrip) {
    ecs::Entity e = W().createEntityWithComponents<ecs::TransformComponent, ecs::NameComponent>();
    ASSERT_TRUE(e.isValid());

    EXPECT_EQ(EntityManager::GetEntityName(e.id), "Entity " + std::to_string(e.id));  // no name yet
    EntityManager::SetEntityName(e.id, "HeroMesh");
    EXPECT_EQ(EntityManager::GetEntityName(e.id), "HeroMesh");
}

TEST(EntityManagerOps, GetSafeEntityNameFallsBackForInvalid) {
    EXPECT_EQ(EntityManager::GetSafeEntityName(ecs::INVALID_ENTITY_ID, "Fallback"), "Fallback");
    EXPECT_EQ(EntityManager::GetSafeEntityName(999999, "Fallback"), "Fallback");
}

TEST(EntityManagerOps, ValidateOrClearClearsInvalidSelection) {
    EXPECT_EQ(EntityManager::ValidateOrClear(ecs::INVALID_ENTITY_ID), ecs::INVALID_ENTITY_ID);
    EXPECT_EQ(EntityManager::ValidateOrClear(999999), ecs::INVALID_ENTITY_ID);

    ecs::Entity e = W().createEntityWithComponents<ecs::TransformComponent>();
    ASSERT_TRUE(e.isValid());
    EXPECT_EQ(EntityManager::ValidateOrClear(e.id), e.id);
}
