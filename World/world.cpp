#include "World.h"
#include <iostream>

// ------------------- CONSTRUCTOR -------------------
World::World()
{
    std::cout << "[World] Initializing world..." << std::endl;

    // Initialize the physics world (gravity etc.)
    physics = PhysicsWorld();

    // Optionally create a simple test entity
    auto ground = createEntity();
    ground->rigidBody = std::make_shared<RigidBody>(
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(20.0f, 1.0f, 20.0f),
        0.0f,
        true,
        ColliderType::BOX
    );
    physics.addBody(ground->rigidBody);

    // Example player
    playerEntity = createEntity();
    playerEntity->rigidBody = std::make_shared<RigidBody>(
        glm::vec3(0.0f, 2.0f, 0.0f),
        glm::vec3(0.5f, 1.0f, 0.5f),
        1.0f,
        false,
        ColliderType::BOX
    );
    physics.addBody(playerEntity->rigidBody);
}

// ------------------- ENTITY CREATION -------------------
std::shared_ptr<Entity> World::createEntity()
{
    auto e = std::make_shared<Entity>();
    entities.push_back(e);
    physics.addEntity(e.get());
    return e;
}

// ------------------- UPDATE STEP -------------------
void World::step(float dt)
{
    // Update physics simulation
    physics.step(dt);

    // Update entity transforms from their rigid bodies
    for (auto& e : entities)
    {
        if (e && e->rigidBody)
            e->transform.position = e->rigidBody->position;
    }

    // Optional: update player movement / camera input
    // (handled externally via updatePlayerMovement)
}

// ------------------- RENDER -------------------
void World::render(Shader& shader, flyCamera& cam)
{
    renderSystem.beginScene(shader, cam, lightPos);

    for (auto& e : entities)
    {
        if (!e) continue;
        renderSystem.renderEntity(*e);
    }

    renderSystem.endScene();
}

