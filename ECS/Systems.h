#pragma once
#include "Entity.h"
#include "TransformComponent.h"
#include "ModelComponent.h"
#include "PhysicsComponent.h"
#include "link/Rendering/Shader.h"
#include "link/Rendering/flyCamera.h"
#include "link/Physics/Physics.h"
#include <vector>
#include <memory>
#pragma once
#include "PhysicsSystem.h"
#include "RenderSystem.h"
#include "MovementSystem.h"
#include "PlayerControllerSystem.h"


struct RenderSystem {
    void render(Shader& shader, flyCamera& camera, const std::vector<std::shared_ptr<Entity>>& entities) {
        shader.use();
        shader.setVec3("viewPos", camera.getPosition());
        shader.setMat4("view", camera.GetViewMatrix());
        shader.setMat4("projection", glm::perspective(glm::radians(45.0f), 800.0f/600.0f, 0.1f, 100.0f));

        for (auto& e : entities) {
            auto T = e->getComponent<TransformComponent>();
            auto M = e->getComponent<ModelComponent>();
            if (!T || !M || !M->model) continue;
            shader.setMat4("model", T->modelMatrix());
            M->model->Draw(shader);
        }
    }
};

// Simple physics system that uses PhysicsWorld but drives per-frame sync
struct PhysicsSystem {
    PhysicsWorld* world = nullptr; // pointer to your existing PhysicsWorld

    PhysicsSystem() = default;
    PhysicsSystem(PhysicsWorld* w) : world(w) {}

    // Systems.cpp (or PhysicsSystem.cpp)
    void step(float dt, std::vector<std::shared_ptr<Entity>>& entities) {
        // Apply simple gravity + integrate
        for (auto& ent : entities) {
            auto phys = ent->getComponent<PhysicsComponent>();
            if (!phys || !phys->body) continue;
               auto& body = *phys->body;

            if (!body.isStatic) {
                // Gravity
                body.applyForce(glm::vec3(0, -9.8f * body.mass, 0));

                // Integrate velocity + position
                body.velocity += (body.forceAccumulator / body.mass) * dt;
                body.position += body.velocity * dt;

                // Clear forces
                body.forceAccumulator = glm::vec3(0.0f);
            }
        }
    }

};
