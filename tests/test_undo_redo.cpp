#include <gtest/gtest.h>
#include "editor/editor_state.h"
#include "editor/entity_manager.h"
#include "editor/undo_redo.h"
#include "editor/scene_manager.h"
#include "ecs/components/Components.h"
#include <string>
#include <chrono>
#include <thread>
#include <cstdio>

namespace {

// Temp scene file for the persistence tests (removed after each use).
constexpr const char* kHistoryScene = "test_history_save.json";

} // namespace

// ============================================================================
// Undo/Redo tests (snapshot-based editor history).
//
// Exercise the command recording helpers (RecordCreate/RecordCreateMany/
// RecordDelete/RecordTransformStart/RecordTransformEnd) and the history API
// (Undo/Redo/CanUndo/CanRedo/Clear) on the global editor's world. Like
// test_entity_manager_ops.cpp, these run WITHOUT a GL context and share the
// global world, so each test asserts only on entities it created itself and
// starts from a cleared history.
// ============================================================================

namespace {

ecs::World& W() { return g_editor.world(); }

ecs::EntityID MakeNamed(const char* name, const glm::vec3& pos = glm::vec3(0.0f)) {
    ecs::Entity e = W().createEntityWithComponents<ecs::TransformComponent, ecs::NameComponent>();
    if (!e.isValid()) return ecs::INVALID_ENTITY_ID;
    if (auto* t = W().getComponentArchetype<ecs::TransformComponent>(e)) t->position = pos;
    if (auto* n = W().getComponentArchetype<ecs::NameComponent>(e)) n->setName(name);
    return e.id;
}

// Find the (current) id of an entity by unique name; INVALID if not found.
ecs::EntityID FindByName(const char* name) {
    ecs::EntityID found = ecs::INVALID_ENTITY_ID;
    W().forEach<ecs::NameComponent>([&](ecs::EntityID id, ecs::NameComponent& n) {
        if (found == ecs::INVALID_ENTITY_ID && std::string(n.getName()) == name) found = id;
    });
    return found;
}

} // namespace

// ----------------------------------------------------------------------------
// Transform (gizmo drag) recording
// ----------------------------------------------------------------------------

TEST(UndoRedoTest, TransformDragUndoRestoresRedoReapplies) {
    UndoRedo::Clear();
    const ecs::EntityID id = MakeNamed("DragTarget", glm::vec3(0, 0, 0));
    ASSERT_NE(id, ecs::INVALID_ENTITY_ID);

    auto* t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id});
    ASSERT_NE(t, nullptr);
    const ecs::TransformComponent before = *t;

    // End without a matching start is a no-op.
    t->position = glm::vec3(5, 5, 5);
    UndoRedo::RecordTransformEnd(id, *t);
    EXPECT_FALSE(UndoRedo::CanUndo());

    // Start drag at (0,0,0), move to (5,5,5), end drag -> one command.
    UndoRedo::RecordTransformStart(id, before);
    t->position = glm::vec3(5, 5, 5);
    UndoRedo::RecordTransformEnd(id, *t);
    EXPECT_TRUE(UndoRedo::CanUndo());

    UndoRedo::Undo();
    t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id});
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->position, glm::vec3(0, 0, 0));

    UndoRedo::Redo();
    t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id});
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->position, glm::vec3(5, 5, 5));
}

TEST(UndoRedoTest, TransformDragUnchangedPushesNothing) {
    UndoRedo::Clear();
    const ecs::EntityID id = MakeNamed("NoChange", glm::vec3(1, 1, 1));
    ASSERT_NE(id, ecs::INVALID_ENTITY_ID);

    auto* t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id});
    ASSERT_NE(t, nullptr);
    UndoRedo::RecordTransformStart(id, *t);
    UndoRedo::RecordTransformEnd(id, *t);  // nothing moved
    EXPECT_FALSE(UndoRedo::CanUndo());
}

// ----------------------------------------------------------------------------
// Create recording
// ----------------------------------------------------------------------------

TEST(UndoRedoTest, CreateUndoDeletesRedoRecreatesWithState) {
    UndoRedo::Clear();
    ecs::Entity e = W().createEntityWithComponents<ecs::TransformComponent,
                                                   ecs::MeshComponent,
                                                   ecs::NameComponent>();
    ASSERT_TRUE(e.isValid());
    if (auto* t = W().getComponentArchetype<ecs::TransformComponent>(e))
        t->position = glm::vec3(7, 8, 9);
    if (auto* m = W().getComponentArchetype<ecs::MeshComponent>(e)) {
        m->meshType = ecs::MeshType::Sphere;
        m->meshID = 1;
        m->color = glm::vec3(0.2f, 0.4f, 0.6f);
    }
    if (auto* n = W().getComponentArchetype<ecs::NameComponent>(e)) n->setName("UndoSphere");

    UndoRedo::RecordCreate(e.id);
    EXPECT_TRUE(UndoRedo::CanUndo());

    UndoRedo::Undo();
    EXPECT_FALSE(EntityManager::EntityExists(e.id));
    EXPECT_EQ(W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{e.id}), nullptr);

    UndoRedo::Redo();
    const ecs::EntityID recreated = FindByName("UndoSphere");
    ASSERT_NE(recreated, ecs::INVALID_ENTITY_ID);

    auto* t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{recreated});
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->position, glm::vec3(7, 8, 9));
    auto* m = W().getComponentArchetype<ecs::MeshComponent>(ecs::Entity{recreated});
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->meshType, ecs::MeshType::Sphere);
    EXPECT_EQ(m->color, glm::vec3(0.2f, 0.4f, 0.6f));
}

TEST(UndoRedoTest, CreateManyUndoRedoRestoresHierarchy) {
    UndoRedo::Clear();
    const ecs::EntityID parent = MakeNamed("BP_Parent", glm::vec3(1, 0, 0));
    const ecs::EntityID child = MakeNamed("BP_Child", glm::vec3(2, 0, 0));
    ASSERT_NE(parent, ecs::INVALID_ENTITY_ID);
    ASSERT_NE(child, ecs::INVALID_ENTITY_ID);
    W().setParent(ecs::Entity{child}, ecs::Entity{parent});
    SUCCEED();

    UndoRedo::RecordCreateMany({parent, child});
    UndoRedo::Undo();
    EXPECT_FALSE(EntityManager::EntityExists(parent));
    EXPECT_FALSE(EntityManager::EntityExists(child));

    UndoRedo::Redo();
    const ecs::EntityID p2 = FindByName("BP_Parent");
    const ecs::EntityID c2 = FindByName("BP_Child");
    ASSERT_NE(p2, ecs::INVALID_ENTITY_ID);
    ASSERT_NE(c2, ecs::INVALID_ENTITY_ID);
    EXPECT_NE(p2, parent);  // ids change across destroy/recreate

    // Parent link must be restored (so the hierarchy survives undo/redo).
    ecs::Entity par = W().getParent(ecs::Entity{c2});
    EXPECT_TRUE(par.isValid());
    EXPECT_EQ(par.id, p2);

    auto* pt = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{p2});
    ASSERT_NE(pt, nullptr);
    EXPECT_EQ(pt->position, glm::vec3(1, 0, 0));
}

// ----------------------------------------------------------------------------
// Delete recording
// ----------------------------------------------------------------------------

TEST(UndoRedoTest, DeleteUndoRestoresRedoDeletes) {
    UndoRedo::Clear();
    const ecs::EntityID id = MakeNamed("UndoDel", glm::vec3(3, 4, 5));
    ASSERT_NE(id, ecs::INVALID_ENTITY_ID);

    UndoRedo::RecordDelete(id);
    W().destroyEntity(ecs::Entity{id});
    EXPECT_FALSE(EntityManager::EntityExists(id));

    UndoRedo::Undo();
    const ecs::EntityID restored = FindByName("UndoDel");
    ASSERT_NE(restored, ecs::INVALID_ENTITY_ID);
    auto* t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{restored});
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->position, glm::vec3(3, 4, 5));

    UndoRedo::Redo();
    EXPECT_FALSE(EntityManager::EntityExists(restored));
}

TEST(UndoRedoTest, BotPlayerDeleteIsNotRecorded) {
    UndoRedo::Clear();
    const ecs::EntityID id = MakeNamed("Bot Player");
    ASSERT_NE(id, ecs::INVALID_ENTITY_ID);
    UndoRedo::RecordDelete(id);
    EXPECT_FALSE(UndoRedo::CanUndo());  // engine-owned entity: no history entry
}

TEST(UndoRedoTest, RecordInvalidIsSafe) {
    UndoRedo::Clear();
    UndoRedo::RecordCreate(ecs::INVALID_ENTITY_ID);
    UndoRedo::RecordDelete(ecs::INVALID_ENTITY_ID);
    UndoRedo::RecordCreateMany({ecs::INVALID_ENTITY_ID, 999999});
    EXPECT_FALSE(UndoRedo::CanUndo());
}

// ----------------------------------------------------------------------------
// History stack behavior
// ----------------------------------------------------------------------------

TEST(UndoRedoTest, NewPushClearsRedoStack) {
    UndoRedo::Clear();
    const ecs::EntityID a = MakeNamed("A_Stack", glm::vec3(0));
    const ecs::EntityID b = MakeNamed("B_Stack", glm::vec3(0));
    ASSERT_NE(a, ecs::INVALID_ENTITY_ID);
    ASSERT_NE(b, ecs::INVALID_ENTITY_ID);

    UndoRedo::RecordCreate(a);
    UndoRedo::Undo();
    EXPECT_TRUE(UndoRedo::CanRedo());

    UndoRedo::RecordCreate(b);  // new push invalidates the redo branch
    EXPECT_FALSE(UndoRedo::CanRedo());

    UndoRedo::Undo();
    EXPECT_FALSE(EntityManager::EntityExists(b));
    // a's create was undone, and the new push invalidated the redo branch, so
    // a stays deleted - the redo that would bring it back is unreachable.
    EXPECT_FALSE(EntityManager::EntityExists(a));
}

TEST(UndoRedoTest, UndoRedoSelectionRevalidation) {
    UndoRedo::Clear();
    const ecs::EntityID id = MakeNamed("SelReval", glm::vec3(0));
    ASSERT_NE(id, ecs::INVALID_ENTITY_ID);
    g_editor.setSelectedEntity(id);

    UndoRedo::RecordCreate(id);
    UndoRedo::Undo();  // deletes the selected entity -> selection must clear
    EXPECT_EQ(g_editor.selectedEntity(), ecs::INVALID_ENTITY_ID);
}

// ----------------------------------------------------------------------------
// Drag coalescing
// ----------------------------------------------------------------------------

TEST(UndoRedoTest, CoalescesRapidDragsOnSameEntity) {
    UndoRedo::Clear();
    const ecs::EntityID id = MakeNamed("Coalesce", glm::vec3(0, 0, 0));
    ASSERT_NE(id, ecs::INVALID_ENTITY_ID);
    auto* t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id});
    ASSERT_NE(t, nullptr);

    // Drag 1: 0 -> 2.
    const ecs::TransformComponent before = *t;
    UndoRedo::RecordTransformStart(id, before);
    t->position.x = 2.0f;
    UndoRedo::RecordTransformEnd(id, *t);

    // Drag 2 (immediately, same entity): 2 -> 4. Must merge into drag 1.
    UndoRedo::RecordTransformStart(id, *t);
    t->position.x = 4.0f;
    UndoRedo::RecordTransformEnd(id, *t);
    EXPECT_TRUE(UndoRedo::CanUndo());

    // A single undo goes all the way back to the first drag's start.
    UndoRedo::Undo();
    t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id});
    ASSERT_NE(t, nullptr);
    EXPECT_FLOAT_EQ(t->position.x, 0.0f);

    // One redo applies the final state of the merged drags.
    UndoRedo::Redo();
    t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id});
    ASSERT_NE(t, nullptr);
    EXPECT_FLOAT_EQ(t->position.x, 4.0f);
}

TEST(UndoRedoTest, DoesNotCoalesceDragsOnDifferentEntities) {
    UndoRedo::Clear();
    const ecs::EntityID a = MakeNamed("CoalA", glm::vec3(0));
    const ecs::EntityID b = MakeNamed("CoalB", glm::vec3(0));
    ASSERT_NE(a, ecs::INVALID_ENTITY_ID);
    ASSERT_NE(b, ecs::INVALID_ENTITY_ID);

    auto* ta = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{a});
    auto* tb = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{b});
    ASSERT_NE(ta, nullptr);
    ASSERT_NE(tb, nullptr);

    UndoRedo::RecordTransformStart(a, *ta);
    ta->position.x = 3.0f;
    UndoRedo::RecordTransformEnd(a, *ta);

    UndoRedo::RecordTransformStart(b, *tb);
    tb->position.x = 9.0f;
    UndoRedo::RecordTransformEnd(b, *tb);

    UndoRedo::Undo();  // undoes b only
    tb = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{b});
    ASSERT_NE(tb, nullptr);
    EXPECT_FLOAT_EQ(tb->position.x, 0.0f);
    ta = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{a});
    ASSERT_NE(ta, nullptr);
    EXPECT_FLOAT_EQ(ta->position.x, 3.0f);  // a untouched
}

TEST(UndoRedoTest, DoesNotCoalesceAfterWindow) {
    UndoRedo::Clear();
    const ecs::EntityID id = MakeNamed("CoalWindow", glm::vec3(0));
    ASSERT_NE(id, ecs::INVALID_ENTITY_ID);
    auto* t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id});
    ASSERT_NE(t, nullptr);

    UndoRedo::RecordTransformStart(id, *t);
    t->position.x = 1.0f;
    UndoRedo::RecordTransformEnd(id, *t);

    // Wait out the coalesce window, then drag again.
    std::this_thread::sleep_for(UndoRedo::kCoalesceWindow + std::chrono::milliseconds(200));
    UndoRedo::RecordTransformStart(id, *t);
    t->position.x = 2.0f;
    UndoRedo::RecordTransformEnd(id, *t);

    UndoRedo::Undo();  // undo the second drag only
    t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id});
    ASSERT_NE(t, nullptr);
    EXPECT_FLOAT_EQ(t->position.x, 1.0f);
    EXPECT_TRUE(UndoRedo::CanUndo());  // first drag is still undoable
}

// ----------------------------------------------------------------------------
// Scene persistence with id remapping
// ----------------------------------------------------------------------------

TEST(UndoRedoTest, CreateHistoryPersistsAcrossSaveLoad) {
    UndoRedo::Clear();
    const ecs::EntityID id = MakeNamed("PersistEnt", glm::vec3(1, 2, 3));
    ASSERT_NE(id, ecs::INVALID_ENTITY_ID);
    UndoRedo::RecordCreate(id);
    ASSERT_TRUE(UndoRedo::CanUndo());

    ASSERT_TRUE(SceneManager::SaveScene(kHistoryScene, W()));
    UndoRedo::Undo();  // entity deleted
    EXPECT_FALSE(EntityManager::EntityExists(id));

    // Reload: entity recreated from the file, history restored and remapped
    // by name onto the new entity.
    ASSERT_TRUE(SceneManager::LoadScene(kHistoryScene, W()));
    EXPECT_TRUE(UndoRedo::CanUndo());
    const ecs::EntityID reloaded = FindByName("PersistEnt");
    ASSERT_NE(reloaded, ecs::INVALID_ENTITY_ID);
    EXPECT_TRUE(EntityManager::EntityExists(reloaded));

    // Undo the restored create -> the RELOADED entity is destroyed.
    UndoRedo::Undo();
    EXPECT_FALSE(EntityManager::EntityExists(reloaded));
    std::remove(kHistoryScene);
}

TEST(UndoRedoTest, TransformHistoryRemapsOnLoad) {
    UndoRedo::Clear();
    const ecs::EntityID id = MakeNamed("PersistTf", glm::vec3(0, 0, 0));
    ASSERT_NE(id, ecs::INVALID_ENTITY_ID);
    auto* t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id});
    ASSERT_NE(t, nullptr);

    UndoRedo::RecordTransformStart(id, *t);
    t->position = glm::vec3(5, 5, 5);
    UndoRedo::RecordTransformEnd(id, *t);
    ASSERT_TRUE(UndoRedo::CanUndo());

    ASSERT_TRUE(SceneManager::SaveScene(kHistoryScene, W()));
    UndoRedo::Undo();
    t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id});
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->position, glm::vec3(0, 0, 0));

    // Reload restores the saved transform (5,5,5) and the transform command
    // (remapped onto this entity by name). Undoing applies the before state.
    ASSERT_TRUE(SceneManager::LoadScene(kHistoryScene, W()));
    t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id});
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->position, glm::vec3(5, 5, 5));

    EXPECT_TRUE(UndoRedo::CanUndo());
    UndoRedo::Undo();
    t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id});
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->position, glm::vec3(0, 0, 0));

    UndoRedo::Redo();
    t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id});
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->position, glm::vec3(5, 5, 5));
    std::remove(kHistoryScene);
}

TEST(UndoRedoTest, MissingHistoryNodeClearsStacks) {
    UndoRedo::Clear();
    const ecs::EntityID id = MakeNamed("NoHist", glm::vec3(0));
    ASSERT_NE(id, ecs::INVALID_ENTITY_ID);
    UndoRedo::RecordCreate(id);
    EXPECT_TRUE(UndoRedo::CanUndo());

    UndoRedo::DeserializeHistory(Json::Value());  // scene without history
    EXPECT_FALSE(UndoRedo::CanUndo());
    EXPECT_FALSE(UndoRedo::CanRedo());
}

// ----------------------------------------------------------------------------
// Editor integration: EntityManager ops must feed the undo history
// ----------------------------------------------------------------------------

TEST(UndoRedoTest, EntityManagerCreateLightIsUndoable) {
    UndoRedo::Clear();
    ecs::Entity e = EntityManager::CreateLight(glm::vec3(1, 2, 3), glm::vec3(0.9f, 0.2f, 0.1f), 4.0f);
    ASSERT_TRUE(e.isValid());
    EXPECT_TRUE(UndoRedo::CanUndo());

    UndoRedo::Undo();
    EXPECT_FALSE(EntityManager::EntityExists(e.id));

    UndoRedo::Redo();
    // Recreated with the full light state captured at record time. Lights
    // have no name, so scan the world for the POINT light we created.
    bool found = false;
    W().forEach<ecs::LightComponent>([&](ecs::EntityID id, ecs::LightComponent& l) {
        if (!found && l.type == ecs::LightType::POINT && l.intensity == 4.0f) {
            found = true;
            EXPECT_EQ(l.color, glm::vec3(0.9f, 0.2f, 0.1f));
        }
    });
    EXPECT_TRUE(found);
}

TEST(UndoRedoTest, EntityManagerCreateCameraIsUndoable) {
    UndoRedo::Clear();
    ecs::Entity e = EntityManager::CreateCamera(glm::vec3(0, 5, 10), glm::vec3(0));
    ASSERT_TRUE(e.isValid());
    EXPECT_TRUE(UndoRedo::CanUndo());

    UndoRedo::Undo();
    EXPECT_FALSE(EntityManager::EntityExists(e.id));

    UndoRedo::Redo();
    // The recreated camera must carry its CameraComponent (isActive).
    bool found = false;
    W().forEach<ecs::CameraComponent>([&](ecs::EntityID, ecs::CameraComponent& c) {
        if (!found && c.isActive) found = true;
    });
    EXPECT_TRUE(found);
}

TEST(UndoRedoTest, EntityManagerDeleteIsUndoable) {
    UndoRedo::Clear();
    const ecs::EntityID id = MakeNamed("DelUndoable", glm::vec3(2, 0, 0));
    ASSERT_NE(id, ecs::INVALID_ENTITY_ID);

    EntityManager::DeleteEntity(id);
    EXPECT_FALSE(EntityManager::EntityExists(id));
    EXPECT_TRUE(UndoRedo::CanUndo());

    UndoRedo::Undo();
    const ecs::EntityID restored = FindByName("DelUndoable");
    ASSERT_NE(restored, ecs::INVALID_ENTITY_ID);
    auto* t = W().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{restored});
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->position, glm::vec3(2, 0, 0));

    UndoRedo::Redo();
    EXPECT_FALSE(EntityManager::EntityExists(restored));
}

TEST(UndoRedoTest, EntityManagerDuplicateIsUndoable) {
    UndoRedo::Clear();
    const ecs::EntityID src = MakeNamed("DupSrc", glm::vec3(0));
    ASSERT_NE(src, ecs::INVALID_ENTITY_ID);

    const ecs::EntityID dup = EntityManager::DuplicateEntity(src);
    ASSERT_NE(dup, ecs::INVALID_ENTITY_ID);
    EXPECT_TRUE(EntityManager::EntityExists(dup));
    EXPECT_TRUE(UndoRedo::CanUndo());

    // Undo removes only the duplicate; the source survives.
    UndoRedo::Undo();
    EXPECT_FALSE(EntityManager::EntityExists(dup));
    EXPECT_TRUE(EntityManager::EntityExists(src));
    EXPECT_NE(FindByName("DupSrc"), ecs::INVALID_ENTITY_ID);
}
