/**
 * ECS Enhanced Features Tests
 * 
 * Tests for:
 * - Archetype-based storage
 * - Multi-threaded execution
 * - Entity relationships
 * - Event system
 * - Serialization
 * - Blueprint system
 */

#include <gtest/gtest.h>
#include "../ecs/ECS.h"
#include "../ecs/World.h"
#include "../ecs/ComponentSerializers.h"
#include "../ecs/components/Components.h"
#include <glm/glm.hpp>
#include <thread>
#include <chrono>
#include <mutex>

using namespace ecs;

// ============================================================================
// Archetype Storage Tests
// ============================================================================

class ArchetypeStorageTest : public ::testing::Test {
protected:
    World world;
    
    void SetUp() override {
        world.init();
    }
    
    void TearDown() override {
        world.shutdown();
    }
};

TEST_F(ArchetypeStorageTest, CreateEntityWithArchetype) {
    // Create entity using archetype storage
    auto entity = world.createEntityWithComponents<TransformComponent, MeshComponent>();
    
    EXPECT_TRUE(world.isAlive(entity));
    EXPECT_TRUE(world.hasComponent<TransformComponent>(entity));
    EXPECT_TRUE(world.hasComponent<MeshComponent>(entity));
}

TEST_F(ArchetypeStorageTest, ArchetypeIteration) {
    // Create multiple entities with same components
    const int entityCount = 100;
    std::vector<Entity> entities;
    
    for (int i = 0; i < entityCount; ++i) {
        auto entity = world.createEntityWithComponents<TransformComponent, MeshComponent>();
        auto* transform = world.getComponent<TransformComponent>(entity);
        transform->position = glm::vec3(i, 0, 0);
        entities.push_back(entity);
    }
    
    // Iterate using archetype-based iteration
    int count = 0;
    world.forEach<TransformComponent, MeshComponent>(
        [&count](EntityID, TransformComponent&, MeshComponent&) {
            count++;
        }
    );
    
    EXPECT_EQ(count, entityCount);
}

TEST_F(ArchetypeStorageTest, ArchetypeCacheCoherency) {
    // Create entities in batches with different component combinations
    std::vector<Entity> batch1, batch2;
    
    // Batch 1: Transform + Mesh
    for (int i = 0; i < 50; ++i) {
        batch1.push_back(world.createEntityWithComponents<TransformComponent, MeshComponent>());
    }
    
    // Batch 2: Transform + Mesh + RigidBody
    for (int i = 0; i < 50; ++i) {
        batch2.push_back(world.createEntityWithComponents<
            TransformComponent, MeshComponent, RigidBodyComponent>());
    }
    
    // Verify both archetypes exist
    int count1 = 0, count2 = 0;
    
    world.forEach<TransformComponent, MeshComponent>(
        [&count1](EntityID, TransformComponent&, MeshComponent&) {
            count1++;
        }
    );
    
    world.forEach<TransformComponent, MeshComponent, RigidBodyComponent>(
        [&count2](EntityID, TransformComponent&, MeshComponent&, RigidBodyComponent&) {
            count2++;
        }
    );
    
    EXPECT_EQ(count1, 100);  // Both batches have Transform+Mesh
    EXPECT_EQ(count2, 50);   // Only batch2 has RigidBody
}

TEST_F(ArchetypeStorageTest, LegacyAddComponentVisibleToForEach) {
    // #3 runtime sync: an entity built via the LEGACY addComponent path
    // (ComponentManager store) must be visible to World::forEach. Before the fix,
    // forEach iterated ArchetypeManager only, silently skipping legacy components
    // added through World::addComponent.
    auto entity = world.createEntity();
    world.addComponent<TransformComponent>(entity);

    int count = 0;
    world.forEach<TransformComponent>([&count](EntityID, TransformComponent&) {
        count++;
    });
    EXPECT_EQ(count, 1);

    // The legacy component is also readable via the dual-path getComponent.
    ASSERT_NE(world.getComponent<TransformComponent>(entity), nullptr);
    EXPECT_EQ(world.getComponent<TransformComponent>(entity)->position, glm::vec3(0.0f));
}

TEST_F(ArchetypeStorageTest, RemoveComponentReconcilesArchetypeStore) {
    // #3 runtime sync: removeComponent must drop the component from BOTH stores so it
    // is no longer reported by hasComponent / visited by forEach, regardless of which
    // store created it. Pre-fix, removeComponent only touched ComponentManager, leaving
    // the ArchetypeManager copy live (hasComponent stayed true and forEach still visited).
    auto entity = world.createEntityWithComponents<TransformComponent, MeshComponent>();

    EXPECT_TRUE(world.hasComponent<TransformComponent>(entity));
    EXPECT_TRUE(world.hasComponent<MeshComponent>(entity));

    world.removeComponent<TransformComponent>(entity);

    // Transform removed from ArchetypeManager -> no longer reported.
    EXPECT_FALSE(world.hasComponent<TransformComponent>(entity));
    // Mesh untouched -> still present.
    EXPECT_TRUE(world.hasComponent<MeshComponent>(entity));

    int remaining = 0;
    world.forEach<TransformComponent, MeshComponent>(
        [&remaining](EntityID, TransformComponent&, MeshComponent&) { remaining++; });
    // Transform was removed, so the (Transform, Mesh) pair no longer matches.
    EXPECT_EQ(remaining, 0);
}

// ============================================================================
// Component Cache Layout Profiling Tests (#4)
// ============================================================================
class CacheLayoutProfileTest : public ::testing::Test {};

TEST_F(CacheLayoutProfileTest, FlagsHeapHandleComponents) {
    // #4: component types whose in-struct fields are DynamicVector/DynamicString
    // heap handles must be flagged so chunk scanners can account for the
    // indirection miss on iteration. After Step B, AnimatorComponent's
    // animations/animationLayers are inlined (StaticVector) -> no heap handles.
    // SkeletonComponent still carries DynamicVector<Bone> bones -> the heap-handle component.
    // TransformComponent is fully inline.
    auto& reg = ecs::detail::ComponentSizeRegistry::instance();
    // #L1075: SkeletonComponent.bones is now BoneSoA (split arrays, self-owned,
    // NOT a DynamicPool/DynamicVector handle) -> no pool-indirection miss on scan.
    EXPECT_FALSE(reg.hasHeapHandles(getComponentTypeID<SkeletonComponent>()));
    EXPECT_FALSE(reg.hasHeapHandles(getComponentTypeID<AnimatorComponent>()));
    EXPECT_FALSE(reg.hasHeapHandles(getComponentTypeID<TransformComponent>()));
}

// ============================================================================
// Multi-threaded Execution Tests
// ============================================================================

class JobSystemTest : public ::testing::Test {
protected:
    JobSystem jobSystem;
    
    JobSystemTest() : jobSystem(4) {}  // 4 worker threads
    
    void SetUp() override {
        jobSystem.init();
    }
    
    void TearDown() override {
        jobSystem.shutdown();
    }
};

TEST_F(JobSystemTest, AddJob) {
    std::atomic<int> counter{0};
    
    auto handle = jobSystem.addJob([&counter]() {
        counter++;
    });
    
    handle.wait();
    EXPECT_EQ(counter, 1);
}

TEST_F(JobSystemTest, ParallelFor) {
    std::vector<int> data(1000, 0);
    
    jobSystem.parallelFor(0, data.size(), [&data](size_t i) {
        data[i] = i * 2;
    });
    
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(data[i], i * 2);
    }
}

TEST_F(JobSystemTest, JobWithDependencies) {
    std::vector<std::string> executionOrder;
    std::mutex mutex;
    
    auto job1 = jobSystem.addJob([&]() {
        std::lock_guard<std::mutex> lock(mutex);
        executionOrder.push_back("job1");
    });
    
    auto job2 = jobSystem.addJobWithDeps([&]() {
        std::lock_guard<std::mutex> lock(mutex);
        executionOrder.push_back("job2");
    }, {job1});
    
    job2.wait();
    
    ASSERT_EQ(executionOrder.size(), 2);
    EXPECT_EQ(executionOrder[0], "job1");
    EXPECT_EQ(executionOrder[1], "job2");
}

TEST_F(JobSystemTest, ParallelWorldUpdate) {
    World world;
    world.init();
    world.setParallelExecution(true);
    
    // Add systems with different priorities
    struct TestSystem : public System {
        std::atomic<int>* updateCount;
        std::string name;
        
        TestSystem(std::atomic<int>* count, const std::string& n) 
            : updateCount(count), name(n) {}
        
        void init() override {
            m_filter = SystemFilter();
        }
        
        void update(float dt) override {
            (*updateCount)++;
        }
        
        const char* getName() const override { return name.c_str(); }
    };
    
    std::atomic<int> updateCount{0};
    
    world.addSystem<TestSystem>(&updateCount, "System1").setPriority(0);
    world.addSystem<TestSystem>(&updateCount, "System2").setPriority(0);
    world.addSystem<TestSystem>(&updateCount, "System3").setPriority(0);
    
    world.update(0.016f);
    
    EXPECT_EQ(updateCount, 3);
    
    world.shutdown();
}

// ============================================================================
// Entity Relationship Tests
// ============================================================================

class RelationshipTest : public ::testing::Test {
protected:
    World world;
    
    void SetUp() override {
        world.init();
    }
    
    void TearDown() override {
        world.shutdown();
    }
};

TEST_F(RelationshipTest, SetParent) {
    auto parent = world.createEntity();
    auto child = world.createEntity();
    
    world.setParent(child, parent);
    
    EXPECT_TRUE(world.hasParent(child));
    EXPECT_TRUE(world.hasChildren(parent));
    EXPECT_EQ(world.getParent(child), parent);
}

TEST_F(RelationshipTest, MultipleChildren) {
    auto parent = world.createEntity();
    auto child1 = world.createEntity();
    auto child2 = world.createEntity();
    auto child3 = world.createEntity();
    
    world.setParent(child1, parent);
    world.setParent(child2, parent);
    world.setParent(child3, parent);
    
    auto children = world.getChildren(parent);
    EXPECT_EQ(children.size(), 3);
}

TEST_F(RelationshipTest, RemoveParent) {
    auto parent = world.createEntity();
    auto child = world.createEntity();
    
    world.setParent(child, parent);
    world.orphan(child);
    
    EXPECT_FALSE(world.hasParent(child));
    EXPECT_FALSE(world.hasChildren(parent));
}

TEST_F(RelationshipTest, GetDescendantsDFS) {
    // Create hierarchy: root -> child1 -> grandchild1
    //                    -> child2
    auto root = world.createEntity();
    auto child1 = world.createEntity();
    auto child2 = world.createEntity();
    auto grandchild1 = world.createEntity();
    
    world.setParent(child1, root);
    world.setParent(child2, root);
    world.setParent(grandchild1, child1);
    
    auto descendants = world.getDescendantsDFS(root);
    EXPECT_EQ(descendants.size(), 3);
}

TEST_F(RelationshipTest, GetDescendantsBFS) {
    auto root = world.createEntity();
    auto child1 = world.createEntity();
    auto child2 = world.createEntity();
    auto grandchild1 = world.createEntity();
    
    world.setParent(child1, root);
    world.setParent(child2, root);
    world.setParent(grandchild1, child1);
    
    auto descendants = world.getDescendantsBFS(root);
    EXPECT_EQ(descendants.size(), 3);
    
    // BFS should return children before grandchildren
    EXPECT_TRUE(std::find(descendants.begin(), descendants.end(), child1) != descendants.end());
    EXPECT_TRUE(std::find(descendants.begin(), descendants.end(), child2) != descendants.end());
}

TEST_F(RelationshipTest, GetRoot) {
    auto root = world.createEntity();
    auto child = world.createEntity();
    auto grandchild = world.createEntity();
    
    world.setParent(child, root);
    world.setParent(grandchild, child);
    
    EXPECT_EQ(world.getRoot(grandchild), root);
    EXPECT_EQ(world.getRoot(child), root);
    EXPECT_EQ(world.getRoot(root), root);
}

TEST_F(RelationshipTest, IsDescendantOf) {
    auto ancestor = world.createEntity();
    auto descendant = world.createEntity();
    
    world.setParent(descendant, ancestor);
    
    EXPECT_TRUE(world.isDescendantOf(descendant, ancestor));
    EXPECT_TRUE(world.isAncestorOf(ancestor, descendant));
}

// ============================================================================
// Event System Tests
// ============================================================================

class EventSystemTest : public ::testing::Test {
protected:
    World world;
    
    void SetUp() override {
        world.init();
    }
    
    void TearDown() override {
        world.shutdown();
    }
};

TEST_F(EventSystemTest, EntityCreatedEvent) {
    bool eventReceived = false;
    
    world.subscribe(EventType::EntityCreated, [&eventReceived](const Event& event) {
        eventReceived = true;
        EXPECT_EQ(event.type, EventType::EntityCreated);
    });
    
    auto entity = world.createEntity();
    world.getEventSystem().processEvents();
    
    EXPECT_TRUE(eventReceived);
}

TEST_F(EventSystemTest, EntityDestroyedEvent) {
    bool eventReceived = false;
    
    world.subscribe(EventType::EntityDestroyed, [&eventReceived](const Event& event) {
        eventReceived = true;
    });
    
    auto entity = world.createEntity();
    world.destroyEntity(entity);
    world.getEventSystem().processEvents();
    
    EXPECT_TRUE(eventReceived);
}

TEST_F(EventSystemTest, ComponentAddedEvent) {
    bool eventReceived = false;
    
    world.subscribeComponentAdded<TransformComponent>([&eventReceived](const Event& event) {
        eventReceived = true;
        EXPECT_EQ(event.type, EventType::ComponentAdded);
    });
    
    auto entity = world.createEntity();
    world.addComponent<TransformComponent>(entity, glm::vec3(1, 2, 3));
    world.getEventSystem().processEvents();
    
    EXPECT_TRUE(eventReceived);
}

TEST_F(EventSystemTest, ComponentRemovedEvent) {
    bool eventReceived = false;
    
    world.subscribeComponentRemoved<TransformComponent>([&eventReceived](const Event& event) {
        eventReceived = true;
    });
    
    auto entity = world.createEntity();
    world.addComponent<TransformComponent>(entity);
    world.removeComponent<TransformComponent>(entity);
    world.getEventSystem().processEvents();
    
    EXPECT_TRUE(eventReceived);
}

TEST_F(EventSystemTest, SubscribeOnce) {
    int callCount = 0;
    
    world.subscribeOnce(EventType::EntityCreated, [&callCount](const Event&) {
        callCount++;
    });
    
    auto entity1 = world.createEntity();
    auto entity2 = world.createEntity();
    world.getEventSystem().processEvents();
    
    EXPECT_EQ(callCount, 1);  // Should only be called once
}

TEST_F(EventSystemTest, EventPriority) {
    std::vector<int> executionOrder;
    
    world.subscribe(EventType::EntityCreated, [&executionOrder](const Event&) {
        executionOrder.push_back(1);
    }, 0);  // Low priority
    
    world.subscribe(EventType::EntityCreated, [&executionOrder](const Event&) {
        executionOrder.push_back(2);
    }, 10);  // High priority
    
    world.subscribe(EventType::EntityCreated, [&executionOrder](const Event&) {
        executionOrder.push_back(3);
    }, 5);  // Medium priority
    
    auto entity = world.createEntity();
    world.getEventSystem().processEvents();
    
    ASSERT_EQ(executionOrder.size(), 3);
    EXPECT_EQ(executionOrder[0], 2);  // High priority first
    EXPECT_EQ(executionOrder[1], 3);  // Medium priority second
    EXPECT_EQ(executionOrder[2], 1);  // Low priority last
}

// ============================================================================
// Serialization Tests
// ============================================================================

class SerializationTest : public ::testing::Test {
protected:
    World world;
    
    void SetUp() override {
        world.init();
        registerDefaultComponentSerializers();
    }
    
    void TearDown() override {
        world.shutdown();
    }
};

TEST_F(SerializationTest, SaveAndLoadWorld) {
    // Create entities with components
    auto entity1 = world.createEntity();
    world.addComponent<TransformComponent>(entity1, glm::vec3(1, 2, 3));
    
    auto entity2 = world.createEntity();
    world.addComponent<TransformComponent>(entity2, glm::vec3(4, 5, 6));
    world.addComponent<MeshComponent>(entity2);
    
    // Save to string
    std::string jsonString = world.serializeToString();
    EXPECT_FALSE(jsonString.empty());
    
    // Create new world and load
    World world2;
    world2.init();
    registerDefaultComponentSerializers();
    
    world2.deserializeFromString(jsonString);
    
    EXPECT_EQ(world2.getEntityCount(), 2);
    
    world2.shutdown();
}

TEST_F(SerializationTest, TransformSerialization) {
    auto entity = world.createEntity();
    TransformComponent original;
    original.position = glm::vec3(10, 20, 30);
    original.rotation = glm::quat(1, 0, 0, 0);
    original.scale = glm::vec3(2, 2, 2);
    
    world.addComponent<TransformComponent>(entity, original);
    
    std::string jsonString = world.serializeToString();
    
    World world2;
    world2.init();
    registerDefaultComponentSerializers();
    world2.deserializeFromString(jsonString);
    
    // Verify components were loaded
    EXPECT_EQ(world2.getEntityCount(), 1);
    
    world2.shutdown();
}

TEST_F(SerializationTest, ComponentRoundTrip) {
    // Prove the type-erased serialize/deserialize pipeline actually restores
    // component DATA (not just entity counts). Serialize an entity with a
    // real component, load into a fresh World, and read back the values.
    auto entity = world.createEntity();
    TransformComponent original;
    original.position = glm::vec3(10, 20, 30);
    original.rotation = glm::quat(1, 0, 0, 0);
    original.scale = glm::vec3(2, 2, 2);
    world.addComponent<TransformComponent>(entity, original);

    std::string jsonString = world.serializeToString();
    EXPECT_FALSE(jsonString.empty());

    World world2;
    world2.init();
    registerDefaultComponentSerializers();
    world2.deserializeFromString(jsonString);

    EXPECT_EQ(world2.getEntityCount(), 1);

    bool found = false;
    for (EntityID id : world2.getEntityManager().getLiveEntities()) {
        if (const auto* t = world2.getComponent<TransformComponent>(Entity{id}); t) {
            EXPECT_EQ(t->position, glm::vec3(10, 20, 30));
            EXPECT_EQ(t->scale, glm::vec3(2, 2, 2));
            // quat(1,0,0,0) => identity; rotation.z == 0
            EXPECT_FLOAT_EQ(t->rotation.z, 0.0f);
            found = true;
        }
    }
    EXPECT_TRUE(found) << "loaded entity missing TransformComponent";

    world2.shutdown();
}

TEST_F(SerializationTest, ArchetypeComponentRoundTrip) {
    // Bi-directional layout: an entity whose component lives ONLY in
    // ArchetypeManager (added via addComponentArchetype, the
    // createEntityWithComponents path) must survive the serialize/deserialize
    // pipeline -- not just the legacy ComponentManager path exercised by
    // ComponentRoundTrip above. Before the fix, serializeEntity iterated
    // compManager.getRegisteredTypeIDs() (CM arrays); an archetype-only entity
    // has no CM arrays, so zero components were saved and the load came back empty.
    auto entity = world.createEntity();
    TransformComponent original;
    original.position = glm::vec3(11, 22, 33);
    original.rotation = glm::quat(0.7071f, 0.0f, 0.0f, 0.7071f); // w,z => rotation.z
    original.scale = glm::vec3(3, 3, 3);
    world.addComponentArchetype<TransformComponent>(entity, original);

    std::string jsonString = world.serializeToString();
    EXPECT_FALSE(jsonString.empty());

    World world2;
    world2.init();
    registerDefaultComponentSerializers();
    world2.deserializeFromString(jsonString);

    EXPECT_EQ(world2.getEntityCount(), 1);

    bool found = false;
    for (EntityID id : world2.getEntityManager().getLiveEntities()) {
        if (const auto* t = world2.getComponent<TransformComponent>(Entity{id}); t) {
            EXPECT_EQ(t->position, glm::vec3(11, 22, 33));
            EXPECT_EQ(t->scale, glm::vec3(3, 3, 3));
            EXPECT_NEAR(t->rotation.z, 0.7071f, 1e-3f);
            found = true;
        }
    }
    EXPECT_TRUE(found) << "loaded archetype entity missing TransformComponent";

    world2.shutdown();
}

TEST_F(SerializationTest, RelationshipComponentRoundTrip) {
    // Entity-ID Translation Engine: a parent <-> child hierarchy built with
    // entity-reference components (ParentComponent::parent, ChildrenComponent::children)
    // must survive a save/load with refs remapped to the FRESH live entity ids --
    // not the stale serialized ids (which would dangle on a fresh World).
    auto parent = world.createEntity();   // serialized id 0
    auto child  = world.createEntity();   // serialized id 1
    world.addComponentArchetype<ParentComponent>(child, Entity{parent.id});
    world.addComponentArchetype<ChildrenComponent>(parent);
    world.getComponent<ChildrenComponent>(parent)->addChild(Entity{child.id});

    // Cross-link invariant in the SOURCE world.
    EXPECT_EQ(world.getComponent<ParentComponent>(child)->parent, Entity{parent.id});
    EXPECT_EQ(world.getComponent<ChildrenComponent>(parent)->children[0], Entity{child.id});

    std::string jsonString = world.serializeToString();
    EXPECT_FALSE(jsonString.empty());

    // Pre-fill world2 so loaded entities receive HIGHER live ids (5,6) than the
    // serialized ids (0,1) -> the remap pass is PROVABLY required (not a no-op),
    // making the test robust to the free-list id policy.
    World world2;
    world2.init();
    registerDefaultComponentSerializers();
    for (int i = 0; i < 5; ++i) world2.createEntity();  // consume live ids 0..4
    world2.deserializeFromString(jsonString);           // parent->5, child->6

    EXPECT_EQ(world2.getEntityCount(), 7);  // 5 dummies + parent + child

    // Locate the loaded parent (entity carrying a single-child ChildrenComponent)
    // and verify the child ref was remapped to liveParent.
    EntityID liveParent = INVALID_ENTITY_ID;
    for (EntityID id : world2.getEntityManager().getLiveEntities()) {
        if (auto* cc = world2.getComponent<ChildrenComponent>(Entity{id}); cc && cc->size() == 1) {
            liveParent = id;
            break;
        }
    }
    ASSERT_NE(liveParent, INVALID_ENTITY_ID) << "parent relationship not loaded";

    EntityID liveChild = INVALID_ENTITY_ID;
    for (EntityID id : world2.getEntityManager().getLiveEntities()) {
        if (auto* pc = world2.getComponent<ParentComponent>(Entity{id});
            pc && pc->parent.id == liveParent) {
            liveChild = id;
            break;
        }
    }
    ASSERT_NE(liveChild, INVALID_ENTITY_ID) << "child parent-ref not remapped to live parent";
    EXPECT_NE(liveChild, liveParent);

    // Cross-link after load: parent's children[0] == liveChild, child's parent == liveParent.
    auto* cc = world2.getComponent<ChildrenComponent>(Entity{liveParent});
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->children[0], Entity{liveChild});
    EXPECT_EQ(world2.getComponent<ParentComponent>(Entity{liveChild})->parent, Entity{liveParent});

    world2.shutdown();
}

// ============================================================================
// Blueprint Tests
// ============================================================================

class BlueprintTest : public ::testing::Test {
protected:
    World world;
    
    void SetUp() override {
        world.init();
    }
    
    void TearDown() override {
        world.shutdown();
    }
};

TEST_F(BlueprintTest, CreateBlueprint) {
    Blueprint blueprint("TestBlueprint");
    EXPECT_EQ(blueprint.getName(), "TestBlueprint");
}

TEST_F(BlueprintTest, BlueprintBuilder) {
    auto blueprint = BlueprintBuilder("EnemyPrefab")
        .addEntity("Root", "enemy")
        .addEntity("Collider")
        .setParent(0)
        .build();
    
    EXPECT_EQ(blueprint->getName(), "EnemyPrefab");
    EXPECT_EQ(blueprint->getEntityCount(), 2);
}

TEST_F(BlueprintTest, BlueprintSerialization) {
    auto blueprint = BlueprintBuilder("TestPrefab")
        .addEntity("Root")
        .build();
    
    // Test blueprint serialization
    Json::Value json = blueprint->serialize();
    EXPECT_EQ(json["name"].asString(), "TestPrefab");
    EXPECT_EQ(json["entities"].size(), 1);
}

// ============================================================================
// Integration Tests
// ============================================================================

class ECSIntegrationTest : public ::testing::Test {
protected:
    World world;
    
    void SetUp() override {
        world.init();
        world.setParallelExecution(true);
        registerDefaultComponentSerializers();
    }
    
    void TearDown() override {
        world.shutdown();
    }
};

TEST_F(ECSIntegrationTest, FullWorkflow) {
    // 1. Create entities with archetype storage
    std::vector<Entity> entities;
    for (int i = 0; i < 10; ++i) {
        auto entity = world.createEntityWithComponents<TransformComponent, RigidBodyComponent>();
        auto* transform = world.getComponent<TransformComponent>(entity);
        transform->position = glm::vec3(i, 0, 0);
        entities.push_back(entity);
    }
    
    // 2. Subscribe to events
    int entitiesCreated = 0;
    world.subscribe(EventType::EntityCreated, [&entitiesCreated](const Event&) {
        entitiesCreated++;
    });
    
    // 3. Set up hierarchy
    if (entities.size() > 1) {
        world.setParent(entities[1], entities[0]);
        EXPECT_TRUE(world.hasParent(entities[1]));
        EXPECT_TRUE(world.hasChildren(entities[0]));
    }
    
    // 4. Process events
    world.getEventSystem().processEvents();
    
    // 5. Update (parallel)
    world.update(0.016f);
    
    // 6. Save world
    std::string jsonString = world.serializeToString();
    EXPECT_FALSE(jsonString.empty());
    
    // 7. Verify archetype iteration works
    int count = 0;
    world.forEach<TransformComponent, RigidBodyComponent>(
        [&count](EntityID, TransformComponent&, RigidBodyComponent&) {
            count++;
        }
    );
    EXPECT_EQ(count, 10);
}

TEST_F(ECSIntegrationTest, PerformanceComparison) {
    const int entityCount = 10000;
    
    // Create entities with legacy method
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < entityCount; ++i) {
        auto entity = world.createEntity();
        world.addComponent<TransformComponent>(entity);
        world.addComponent<MeshComponent>(entity);
    }
    
    auto legacyTime = std::chrono::high_resolution_clock::now() - start;
    
    // Create entities with archetype method
    start = std::chrono::high_resolution_clock::now();
    
    World world2;
    world2.init();
    
    for (int i = 0; i < entityCount; ++i) {
        auto entity = world2.createEntityWithComponents<TransformComponent, MeshComponent>();
    }
    
    auto archetypeTime = std::chrono::high_resolution_clock::now() - start;
    
    // Archetype should be faster or comparable
    // (Note: This is a basic test, real benchmarks need more iterations)
    EXPECT_TRUE(archetypeTime <= legacyTime * 2);  // Shouldn't be much slower
    
    world2.shutdown();
}
