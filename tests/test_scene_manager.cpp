/**
 * Scene Manager Tests - Save/Load round trip
 *
 * Verifies that a scene saved via SceneManager::SaveScene (which serializes
 * Transform + Name entities, including the playable bot) restores the same
 * entities and transforms through SceneManager::LoadScene.
 *
 * Run with: make test
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdio>
#include <string>

#include "../editor/scene_manager.h"
#include "../ecs/ECS.h"
#include "../ecs/components/Components.h"

namespace {

constexpr const char* kTestScene = "/tmp/rtt_scene_test.json";

}  // namespace

TEST(SceneManager, SaveLoadRoundTripRestoresBotTransform) {
    ecs::World world;
    world.init();

    auto e = world.createEntityWithComponents<ecs::TransformComponent,
                                              ecs::NameComponent>();
    ASSERT_TRUE(e.isValid());
    if (auto* n = world.getComponent<ecs::NameComponent>(e)) n->setName("Bot Player");
    if (auto* t = world.getComponent<ecs::TransformComponent>(e)) {
        t->position = glm::vec3(4.0f, 12.5f, 4.0f);
        t->scale = glm::vec3(0.0099f);
        t->rotation = glm::quat(glm::vec3(0.0f, 1.0f, 0.0f));
    }

    ASSERT_TRUE(SceneManager::SaveScene(kTestScene, world));

    // Load into a fresh world (simulates restarting / reopening the scene).
    ecs::World world2;
    world2.init();
    ASSERT_TRUE(SceneManager::LoadScene(kTestScene, world2));

    bool found = false;
    glm::vec3 pos(0.0f);
    world2.forEach<ecs::TransformComponent, ecs::NameComponent>(
        [&](ecs::EntityID, ecs::TransformComponent& t, ecs::NameComponent& n) {
            if (std::string(n.name) == "Bot Player") {
                found = true;
                pos = t.position;
            }
        });

    EXPECT_TRUE(found) << "loaded scene must contain the 'Bot Player' entity";
    EXPECT_NEAR(pos.x, 4.0f, 1e-3f);
    EXPECT_NEAR(pos.y, 12.5f, 1e-3f);
    EXPECT_NEAR(pos.z, 4.0f, 1e-3f);

    std::remove(kTestScene);
}

TEST(SceneManager, SaveLoadRoundTripRestoresLightEntity) {
    ecs::World world;
    world.init();

    // Mirror EntityManager::CreateLight but without the editor singleton.
    auto e = world.createEntityWithComponents<ecs::TransformComponent,
                                              ecs::LightComponent,
                                              ecs::NameComponent>();
    ASSERT_TRUE(e.isValid());
    if (auto* n = world.getComponentArchetype<ecs::NameComponent>(e)) n->setName("Point Light 7");
    if (auto* t = world.getComponentArchetype<ecs::TransformComponent>(e))
        t->position = glm::vec3(3.0f, 6.0f, -4.0f);
    if (auto* l = world.getComponentArchetype<ecs::LightComponent>(e)) {
        l->type = ecs::LightType::POINT;
        l->color = glm::vec3(1.0f, 0.8f, 0.3f);
        l->intensity = 3.0f;
        l->range = 12.0f;
        l->enabled = true;
        l->castShadows = false;
        l->temperature = 4200.0f;
        l->useTemperature = true;
    }

    ASSERT_TRUE(SceneManager::SaveScene(kTestScene, world));

    ecs::World world2;
    world2.init();
    ASSERT_TRUE(SceneManager::LoadScene(kTestScene, world2));

    bool found = false;
    world2.forEach<ecs::TransformComponent, ecs::NameComponent>(
        [&](ecs::EntityID id, ecs::TransformComponent&, ecs::NameComponent& n) {
            if (std::string(n.name) == "Point Light 7") {
                found = true;
                auto* l = world2.getComponentArchetype<ecs::LightComponent>(ecs::Entity{id});
                ASSERT_NE(l, nullptr) << "light component must be restored";
                EXPECT_EQ(l->type, ecs::LightType::POINT);
                EXPECT_NEAR(l->color.x, 1.0f, 1e-4f);
                EXPECT_NEAR(l->color.y, 0.8f, 1e-4f);
                EXPECT_NEAR(l->color.z, 0.3f, 1e-4f);
                EXPECT_NEAR(l->intensity, 3.0f, 1e-4f);
                EXPECT_NEAR(l->range, 12.0f, 1e-4f);
                EXPECT_TRUE(l->enabled);
                EXPECT_FALSE(l->castShadows);
                EXPECT_NEAR(l->temperature, 4200.0f, 1e-3f);
                EXPECT_TRUE(l->useTemperature);
            }
        });
    EXPECT_TRUE(found) << "loaded scene must contain the light entity";

    std::remove(kTestScene);
}

TEST(SceneManager, SaveLoadRoundTripRestoresModelPath) {
    ecs::World world;
    world.init();

    auto e = world.createEntityWithComponents<ecs::TransformComponent,
                                              ecs::ModelComponent,
                                              ecs::NameComponent>();
    ASSERT_TRUE(e.isValid());
    if (auto* n = world.getComponentArchetype<ecs::NameComponent>(e)) n->setName("Imported Bot");
    if (auto* t = world.getComponentArchetype<ecs::TransformComponent>(e))
        t->position = glm::vec3(4.0f, 0.5f, 4.0f);
    if (auto* m = world.getComponentArchetype<ecs::ModelComponent>(e)) {
        m->setModelPath("assets/bot.fbx");
        m->visible = true;
        m->useMaterialOverrides = true;
        m->albedoOverride = glm::vec3(0.2f, 0.4f, 0.9f);
        m->metallicOverride = 0.3f;
        m->roughnessOverride = 0.7f;
    }

    ASSERT_TRUE(SceneManager::SaveScene(kTestScene, world));

    ecs::World world2;
    world2.init();
    ASSERT_TRUE(SceneManager::LoadScene(kTestScene, world2));

    bool found = false;
    world2.forEach<ecs::TransformComponent, ecs::NameComponent>(
        [&](ecs::EntityID id, ecs::TransformComponent&, ecs::NameComponent& n) {
            if (std::string(n.name) == "Imported Bot") {
                found = true;
                auto* m = world2.getComponentArchetype<ecs::ModelComponent>(ecs::Entity{id});
                ASSERT_NE(m, nullptr) << "model component must be restored";
                EXPECT_TRUE(m->hasModelPath());
                EXPECT_STREQ(m->getModelPath(), "assets/bot.fbx");
                EXPECT_TRUE(m->visible);
                EXPECT_TRUE(m->useMaterialOverrides);
                EXPECT_NEAR(m->albedoOverride.x, 0.2f, 1e-4f);
                EXPECT_NEAR(m->metallicOverride, 0.3f, 1e-4f);
                EXPECT_NEAR(m->roughnessOverride, 0.7f, 1e-4f);
            }
        });
    EXPECT_TRUE(found) << "loaded scene must contain the model entity";

    std::remove(kTestScene);
}

TEST(SceneManager, LoadSceneDedupesByName) {
    ecs::World world;
    world.init();

    auto e1 = world.createEntityWithComponents<ecs::TransformComponent,
                                               ecs::NameComponent>();
    if (auto* n = world.getComponent<ecs::NameComponent>(e1)) n->setName("Bot Player");
    if (auto* t = world.getComponent<ecs::TransformComponent>(e1))
        t->position = glm::vec3(1.0f, 2.0f, 3.0f);

    ASSERT_TRUE(SceneManager::SaveScene(kTestScene, world));

    // Load into the SAME world - the existing "Bot Player" must be replaced,
    // not duplicated.
    ASSERT_TRUE(SceneManager::LoadScene(kTestScene, world));

    size_t count = 0;
    world.forEach<ecs::TransformComponent, ecs::NameComponent>(
        [&](ecs::EntityID, ecs::TransformComponent&, ecs::NameComponent& n) {
            if (std::string(n.name) == "Bot Player") ++count;
        });
    EXPECT_EQ(count, 1u) << "reloading must not stack duplicate entities";

    std::remove(kTestScene);
}

// Note: main() is in test_main.cpp - don't duplicate
