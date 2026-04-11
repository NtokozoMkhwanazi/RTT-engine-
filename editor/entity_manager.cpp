#include "entity_manager.h"
#include "editor_state.h"
#include "console.h"

namespace EntityManager {

ecs::Entity CreateCube(const glm::vec3& pos, const glm::vec3& scale, const glm::vec3& color) {
    auto e = g_editor.world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world.getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_editor.world.getComponentArchetype<ecs::MeshComponent>(e);

    if (t) { t->position = pos; t->scale = scale; t->rotation = glm::quat(1, 0, 0, 0); }
    if (m) { m->visible = true; m->meshID = 0; m->color = color; m->meshType = ecs::MeshType::Cube; }

    EditorConsole::Log("Created Cube at (" + std::to_string(pos.x) + ", " + 
                       std::to_string(pos.y) + ", " + std::to_string(pos.z) + ")");
    return e;
}

ecs::Entity CreateSphere(const glm::vec3& pos, float radius, const glm::vec3& color) {
    auto e = g_editor.world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world.getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_editor.world.getComponentArchetype<ecs::MeshComponent>(e);

    if (t) { t->position = pos; t->scale = glm::vec3(radius); t->rotation = glm::quat(1, 0, 0, 0); }
    if (m) { m->visible = true; m->color = color; m->meshID = 0; m->meshType = ecs::MeshType::Sphere; }

    EditorConsole::Log("Created Sphere");
    return e;
}

ecs::Entity CreatePlane(const glm::vec3& pos, const glm::vec2& size, const glm::vec3& color) {
    auto e = g_editor.world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world.getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_editor.world.getComponentArchetype<ecs::MeshComponent>(e);

    if (t) { t->position = pos; t->scale = glm::vec3(size.x, 0.01f, size.y); t->rotation = glm::quat(1, 0, 0, 0); }
    if (m) { m->visible = true; m->color = color; m->meshID = 0; m->meshType = ecs::MeshType::Plane; }

    EditorConsole::Log("Created Plane");
    return e;
}

ecs::Entity CreateCylinder(const glm::vec3& pos, float radius, float height, const glm::vec3& color) {
    auto e = g_editor.world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world.getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_editor.world.getComponentArchetype<ecs::MeshComponent>(e);

    if (t) { t->position = pos; t->scale = glm::vec3(radius, height, radius); t->rotation = glm::quat(1, 0, 0, 0); }
    if (m) { m->visible = true; m->color = color; m->meshID = 0; m->meshType = ecs::MeshType::Cylinder; }

    EditorConsole::Log("Created Cylinder");
    return e;
}

ecs::Entity CreateCone(const glm::vec3& pos, float radius, float height, const glm::vec3& color) {
    auto e = g_editor.world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world.getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_editor.world.getComponentArchetype<ecs::MeshComponent>(e);

    if (t) { t->position = pos; t->scale = glm::vec3(radius, height, radius); t->rotation = glm::quat(1, 0, 0, 0); }
    if (m) { m->visible = true; m->color = color; m->meshID = 0; m->meshType = ecs::MeshType::Cone; }

    EditorConsole::Log("Created Cone");
    return e;
}

ecs::Entity CreateTorus(const glm::vec3& pos, float majorRadius, float minorRadius, const glm::vec3& color) {
    auto e = g_editor.world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world.getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_editor.world.getComponentArchetype<ecs::MeshComponent>(e);

    if (t) { t->position = pos; t->scale = glm::vec3(majorRadius); t->rotation = glm::quat(1, 0, 0, 0); }
    if (m) { m->visible = true; m->color = color; m->meshID = 0; m->meshType = ecs::MeshType::Torus; }

    EditorConsole::Log("Created Torus");
    return e;
}

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
    if (n) return n->name;
    return "Entity " + std::to_string(id);
}

void SetEntityName(ecs::EntityID id, const std::string& name) {
    ecs::Entity entity{id};
    auto* n = g_editor.world.getComponentArchetype<ecs::NameComponent>(entity);
    if (n) {
        n->name = name;
    }
}

} // namespace EntityManager
