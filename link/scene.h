#pragma once
#include "link/Physics/Physics.h"
#include "Shader.h"
#include <memory>
#include <vector>
#include <glm/glm.hpp>

class Scene {
public:
    std::vector<std::shared_ptr<RigidBody>> objects;
    std::shared_ptr<RigidBody> floor;

    Scene() = default;

    void addObject(std::shared_ptr<RigidBody> obj) { objects.push_back(obj); }

    void setFloor(std::shared_ptr<RigidBody> f) { floor = f; }

    void update(float dt, PhysicsWorld& physics) {
        physics.step(dt);
    }

    void render(Shader& shader, unsigned int VAO) {
        glBindVertexArray(VAO);
        for (auto& obj : objects) {
            glm::mat4 model = glm::mat4(1.0f);
            model = glm::translate(model, obj->position);
            model = glm::scale(model, obj->scale);
            shader.setMat4("model", model);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }

        // Draw floor
        if (floor) {
            glm::mat4 model = glm::mat4(1.0f);
            model = glm::translate(model, floor->position);
            model = glm::scale(model, floor->scale);
            shader.setMat4("model", model);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }
        glBindVertexArray(0);
    }
    void renderDebug(Shader& shader, unsigned int cubeVAO) {
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE); // wireframe

    glBindVertexArray(cubeVAO);
    for (auto& obj : objects) {
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, obj->position);
        model = glm::scale(model, obj->scale);
        shader.setMat4("model", model);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }

    // Draw floor
    if (floor) {
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, floor->position);
        model = glm::scale(model, floor->scale);
        shader.setMat4("model", model);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }

    glBindVertexArray(0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); // reset to fill
}

};
