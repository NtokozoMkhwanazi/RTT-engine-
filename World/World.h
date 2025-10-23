#pragma once
#include <vector>
#include <memory>
#include <string>
#include "link/Physics/RigidBody.h"
#include "link/Physics/Physics.h"
#include "link/World/Skybox.h"
#include "link/World/Shader.h"
#include "link/World/flyCamera.h"
#include "link/ECS/Entity.h"
#include "link/ECS/PhysicsSystem.h"
#include "link/ECS/RenderSystem.h"
#include "link/ECS/MovementSystem.h"

struct Entity; // forward if you have one

class World {
    public:
        World();
    
        bool loadFromScene(const std::string& scenePath);
        void step(float dt);
        void render(Shader& shader, flyCamera& cam);
        
        std::shared_ptr<Entity> createEntity();
    
        PhysicsWorld physics;
        std::vector<std::shared_ptr<Entity>> entities;
        Skybox sky;
        glm::vec3 lightPos{2.0f, 10.0f, 2.0f};
    
        std::shared_ptr<Entity> playerEntity;
        void updatePlayerMovement(GLFWwindow* window, float dt);
    
    private:
            PhysicsSystem physSystem;
            RenderSystem renderSystem;
            MovementSystem movementSystem;
};

