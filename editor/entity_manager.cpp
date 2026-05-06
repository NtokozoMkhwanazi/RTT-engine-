#include "entity_manager.h"
#include "editor_state.h"
#include "console.h"
#include "ecs/components/ModelComponent.h"
#include "modelSystem/ModelManager.h"

namespace EntityManager {

// Remove primitives - use model loading instead

ecs::Entity CreateLight(const glm::vec3& pos, const glm::vec3& color, float intensity) {
    auto e = g_editor.world.createEntityWithComponents<ecs::TransformComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world.getComponentArchetype<ecs::TransformComponent>(e);
    if (t) { t->position = pos; t->scale = glm::vec3(0.2f); t->rotation = glm::quat(1, 0, 0, 0); }

    EditorConsole::Log("Created Light");
    (void)color; (void)intensity;
    return e;
}

ecs::Entity CreateCamera(const glm::vec3& pos, const glm::vec3& target) {
    auto e = g_editor.world.createEntityWithComponents<ecs::TransformComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world.getComponentArchetype<ecs::TransformComponent>(e);
    if (t) { t->position = pos; t->scale = glm::vec3(1); t->rotation = glm::quat(1, 0, 0, 0); }

    EditorConsole::Log("Created Camera");
    (void)target;
    return e;
}

void DeleteEntity(ecs::EntityID id) {
    if (id == ecs::INVALID_ENTITY_ID) return;
    
    EditorConsole::Log("Deleted entity " + std::to_string(id));
    
    // TODO: Properly destroy entity through ECS
    if (g_editor.selectedEntity == id) {
        g_editor.selectedEntity = ecs::INVALID_ENTITY_ID;
    }
}

void DuplicateEntity(ecs::EntityID id) {
    if (id == ecs::INVALID_ENTITY_ID) return;

    ecs::Entity entity{id};
    auto* t = g_editor.world.getComponentArchetype<ecs::TransformComponent>(entity);
    auto* m = g_editor.world.getComponentArchetype<ecs::MeshComponent>(entity);

    if (t && m) {
        auto newEntity = g_editor.world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
        auto* newT = g_editor.world.getComponentArchetype<ecs::TransformComponent>(newEntity);
        auto* newM = g_editor.world.getComponentArchetype<ecs::MeshComponent>(newEntity);

        if (newT && newM) {
            newT->position = t->position + glm::vec3(1, 0, 0);
            newT->rotation = t->rotation;
            newT->scale = t->scale;
            newM->color = m->color;
            newM->visible = m->visible;
            newM->meshID = m->meshID;
            newM->meshType = m->meshType;

            EditorConsole::Log("Duplicated entity " + std::to_string(id) + " -> " + std::to_string(newEntity.id));
        }
    }
}

std::string GetEntityName(ecs::EntityID id) {
    ecs::Entity entity{id};
    auto* n = g_editor.world.getComponentArchetype<ecs::NameComponent>(entity);
    if (n && n->hasName()) return n->getName();
    return "Entity " + std::to_string(id);
}

void SetEntityName(ecs::EntityID id, const std::string& name) {
    ecs::Entity entity{id};
    auto* n = g_editor.world.getComponentArchetype<ecs::NameComponent>(entity);
    if (n) {
        n->setName(name);
    }
}

ecs::Entity CreateModel(const std::string& modelPath, const glm::vec3& pos, const glm::vec3& scale, const glm::vec3& rotation) {
    auto handle = ModelSystem::ModelRegistry::getInstance().load(modelPath);
    Model* model = ModelSystem::ModelRegistry::getInstance().get(handle);
    if (!model) {
        EditorConsole::Log("Failed to load model: " + modelPath, 2);
        return ecs::Entity{ecs::INVALID_ENTITY_ID};
    }

    auto e = g_editor.world.createEntityWithComponents<ecs::TransformComponent, ecs::ModelComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world.getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_editor.world.getComponentArchetype<ecs::ModelComponent>(e);

    if (t) {
        t->position = pos;
        t->scale = scale;
        t->setEulerAngles(glm::radians(rotation));
    }

    if (m) {
        m->modelHandle = handle;
        m->visible = true;
        m->setModelPath(modelPath);
    }

    EditorConsole::Log("Loaded model: " + modelPath + 
                       " | Meshes: " + std::to_string(model->GetMeshCount()) +
                       " | Triangles: " + std::to_string(model->GetTotalTriangleCount()));
    
    return e;
}

ecs::Entity CreateAnimatedModel(const std::string& modelPath, const glm::vec3& pos, const glm::vec3& scale, const glm::vec3& rotation, int animationIndex) {
    auto handle = ModelSystem::ModelRegistry::getInstance().load(modelPath);
    Model* model = ModelSystem::ModelRegistry::getInstance().get(handle);
    if (!model) {
        EditorConsole::Log("Failed to load animated model: " + modelPath, 2);
        return ecs::Entity{ecs::INVALID_ENTITY_ID};
    }

    if (model->GetAnimationCount() == 0) {
        EditorConsole::Log("Model has no animations, creating as static model", 1);
        return CreateModel(modelPath, pos, scale, rotation);
    }

    auto e = g_editor.world.createEntityWithComponents<ecs::TransformComponent, ecs::ModelComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world.getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_editor.world.getComponentArchetype<ecs::ModelComponent>(e);

    if (t) {
        t->position = pos;
        t->scale = scale;
        t->setEulerAngles(glm::radians(rotation));
    }

    if (m) {
        m->modelHandle = handle;
        m->visible = true;
        m->setModelPath(modelPath);
    }

    EditorConsole::Log("Loaded model: " + modelPath +
                       " | Animations: " + std::to_string(model->GetAnimationCount()));

    return e;
}

} // namespace EntityManager
