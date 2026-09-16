#include "entity_manager.h"
#include "editor_state.h"
#include "console.h"
#include "undo_redo.h"
#include "ecs/components/Components.h"
#include "ecs/components/ModelComponent.h"
#include "modelSystem/ModelManager.h"

namespace EntityManager {

// Remove primitives - use model loading instead

ecs::Entity CreateLight(const glm::vec3& pos, const glm::vec3& color, float intensity) {
    auto e = g_editor.world().createEntityWithComponents<ecs::TransformComponent,
                                                         ecs::LightComponent,
                                                         ecs::NameComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e);
    if (t) { t->position = pos; t->scale = glm::vec3(0.2f); t->rotation = glm::quat(1, 0, 0, 0); }

    auto* l = g_editor.world().getComponentArchetype<ecs::LightComponent>(e);
    if (l) { l->color = color; l->intensity = intensity; l->type = ecs::LightType::POINT; }

    // Unique name (id-suffixed) so the undo stack can locate this entity by
    // name and the outliner shows something meaningful.
    if (auto* n = g_editor.world().getComponentArchetype<ecs::NameComponent>(e)) {
        n->setName("Point Light " + std::to_string(e.id));
    }

    UndoRedo::RecordCreate(e.id);
    EditorConsole::Log("Created Light");
    return e;
}

ecs::Entity CreateCamera(const glm::vec3& pos, const glm::vec3& target) {
    auto e = g_editor.world().createEntityWithComponents<ecs::TransformComponent,
                                                         ecs::CameraComponent,
                                                         ecs::NameComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e);
    if (t) { t->position = pos; t->scale = glm::vec3(1); t->rotation = glm::quat(1, 0, 0, 0); }

    auto* c = g_editor.world().getComponentArchetype<ecs::CameraComponent>(e);
    if (c) { c->isActive = true; }

    if (auto* n = g_editor.world().getComponentArchetype<ecs::NameComponent>(e)) {
        n->setName("Camera " + std::to_string(e.id));
    }

    // Orient the camera toward the target so it spawns facing somewhere useful.
    glm::vec3 dir = target - pos;
    const float len = glm::length(dir);
    if (len > 0.0001f) {
        dir /= len;
        if (std::abs(dir.y) > 0.999f) dir.x += 0.001f;  // avoid degenerate up-alignment
        if (auto* t = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e)) {
            t->rotation = glm::quatLookAt(glm::normalize(dir), glm::vec3(0.0f, 1.0f, 0.0f));
        }
    }

    UndoRedo::RecordCreate(e.id);
    EditorConsole::Log("Created Camera");
    return e;
}

void DeleteEntity(ecs::EntityID id) {
    if (!EntityExists(id)) return;

    // The playable character is engine-owned; refuse to delete it.
    ecs::Entity ent{id};
    if (auto* n = g_editor.world().getComponentArchetype<ecs::NameComponent>(ent)) {
        if (std::string(n->getName()) == "Bot Player") return;
    }

    EditorConsole::Log("Deleted entity " + std::to_string(id));

    // Record BEFORE destroying so the snapshot can capture the live entity.
    UndoRedo::RecordDelete(id);
    g_editor.world().destroyEntity(ent);

    if (g_editor.selectedEntity() == id) {
        g_editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
    }
}

ecs::EntityID DuplicateEntity(ecs::EntityID id) {
    if (!EntityExists(id)) return ecs::INVALID_ENTITY_ID;

    ecs::World& world = g_editor.world();
    ecs::Entity entity{id};
    auto* t = world.getComponentArchetype<ecs::TransformComponent>(entity);
    auto* m = world.getComponentArchetype<ecs::MeshComponent>(entity);
    auto* n = world.getComponentArchetype<ecs::NameComponent>(entity);

    auto newEntity = world.createEntityWithComponents<ecs::TransformComponent>();
    if (!newEntity.isValid()) return ecs::INVALID_ENTITY_ID;

    if (auto* newT = world.getComponentArchetype<ecs::TransformComponent>(newEntity)) {
        if (t) {
            newT->position = t->position + glm::vec3(1, 0, 0);
            newT->rotation = t->rotation;
            newT->scale = t->scale;
        }
    }

    if (m) {
        world.addComponentArchetype<ecs::MeshComponent>(newEntity);
        if (auto* newM = world.getComponentArchetype<ecs::MeshComponent>(newEntity)) {
            newM->color = m->color;
            newM->visible = m->visible;
            newM->meshID = m->meshID;
            newM->meshType = m->meshType;
        }
    }

    if (n) {
        world.addComponentArchetype<ecs::NameComponent>(newEntity);
        if (auto* newN = world.getComponentArchetype<ecs::NameComponent>(newEntity)) {
            newN->setName(std::string(n->getName()) + "_copy");
        }
    }

    UndoRedo::RecordCreate(newEntity.id);
    EditorConsole::Log("Duplicated entity " + std::to_string(id) + " -> " + std::to_string(newEntity.id));
    return newEntity.id;
}

std::string GetEntityName(ecs::EntityID id) {
    ecs::Entity entity{id};
    auto* n = g_editor.world().getComponentArchetype<ecs::NameComponent>(entity);
    if (n && n->hasName()) return n->getName();
    return "Entity " + std::to_string(id);
}

void SetEntityName(ecs::EntityID id, const std::string& name) {
    if (!EntityExists(id)) return;

    ecs::Entity entity{id};
    auto* n = g_editor.world().getComponentArchetype<ecs::NameComponent>(entity);
    if (n) {
        n->setName(name);
    }
}

// ============================================================================
// Entity Validation Helpers
// ============================================================================

bool EntityExists(ecs::EntityID id) {
    if (!IsValidEntityID(id)) return false;
    return g_editor.world().isAlive(ecs::Entity{id});
}

ecs::EntityID ValidateOrClear(ecs::EntityID id) {
    return EntityExists(id) ? id : ecs::INVALID_ENTITY_ID;
}

std::string GetSafeEntityName(ecs::EntityID id, const char* fallback) {
    if (!EntityExists(id)) return std::string(fallback ? fallback : "Invalid Entity");
    return GetEntityName(id);
}

ecs::Entity CreateModel(const std::string& modelPath, const glm::vec3& pos, const glm::vec3& scale, const glm::vec3& rotation) {
    auto handle = ModelSystem::ModelRegistry::getInstance().load(modelPath);
    Model* model = ModelSystem::ModelRegistry::getInstance().get(handle);
    if (!model || model->GetMeshCount() == 0) {
        // Missing file OR a file that failed to parse (0 meshes) - never
        // create a broken empty entity.
        EditorConsole::Log("Failed to load model: " + modelPath, 2);
        return ecs::Entity{ecs::INVALID_ENTITY_ID};
    }

    auto e = g_editor.world().createEntityWithComponents<ecs::TransformComponent, ecs::ModelComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_editor.world().getComponentArchetype<ecs::ModelComponent>(e);

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

    UndoRedo::RecordCreate(e.id);
    EditorConsole::Log("Loaded model: " + modelPath + 
                       " | Meshes: " + std::to_string(model->GetMeshCount()) +
                       " | Triangles: " + std::to_string(model->GetTotalTriangleCount()));
    
    return e;
}

ecs::Entity CreateAnimatedModel(const std::string& modelPath, const glm::vec3& pos, const glm::vec3& scale, const glm::vec3& rotation, int animationIndex) {
    auto handle = ModelSystem::ModelRegistry::getInstance().load(modelPath);
    Model* model = ModelSystem::ModelRegistry::getInstance().get(handle);
    if (!model || model->GetMeshCount() == 0) {
        EditorConsole::Log("Failed to load animated model: " + modelPath, 2);
        return ecs::Entity{ecs::INVALID_ENTITY_ID};
    }

    if (model->GetAnimationCount() == 0) {
        EditorConsole::Log("Model has no animations, creating as static model", 1);
        return CreateModel(modelPath, pos, scale, rotation);
    }

    auto e = g_editor.world().createEntityWithComponents<ecs::TransformComponent, ecs::ModelComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_editor.world().getComponentArchetype<ecs::ModelComponent>(e);

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

    UndoRedo::RecordCreate(e.id);
    EditorConsole::Log("Loaded model: " + modelPath +
                       " | Animations: " + std::to_string(model->GetAnimationCount()));

    return e;
}

ecs::Entity CreatePrimitive(ecs::MeshType type, const glm::vec3& pos) {
    auto e = g_editor.world().createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    if (!e.isValid()) return e;

    if (auto* t = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e)) {
        t->position = pos;
    }
    if (auto* m = g_editor.world().getComponentArchetype<ecs::MeshComponent>(e)) {
        m->meshType = type;
        m->visible = true;
    }
    UndoRedo::RecordCreate(e.id);
    EditorConsole::Log("Created primitive entity " + std::to_string(e.id));
    return e;
}

bool CreateBlueprintFromSelected(ecs::EntityID id, const std::string& name) {
    if (!EntityExists(id)) return false;

    ecs::World& world = g_editor.world();
    ecs::Entity ent{id};
    auto blueprint = std::make_unique<ecs::Blueprint>(name);
    ecs::BlueprintEntity be;
    be.name = GetEntityName(id);

    if (auto* t = world.getComponentArchetype<ecs::TransformComponent>(ent)) {
        ecs::ComponentOverride co;
        co.typeID = ecs::getComponentTypeID<ecs::TransformComponent>();
        co.componentName = "TransformComponent";
        co.data["position"][0] = t->position.x;
        co.data["position"][1] = t->position.y;
        co.data["position"][2] = t->position.z;
        co.data["scale"][0] = t->scale.x;
        co.data["scale"][1] = t->scale.y;
        co.data["scale"][2] = t->scale.z;
        co.data["rotation"][0] = t->rotation.x;
        co.data["rotation"][1] = t->rotation.y;
        co.data["rotation"][2] = t->rotation.z;
        co.data["rotation"][3] = t->rotation.w;
        be.componentOverrides.push_back(std::move(co));
    }

    if (auto* n = world.getComponentArchetype<ecs::NameComponent>(ent)) {
        ecs::ComponentOverride co;
        co.typeID = ecs::getComponentTypeID<ecs::NameComponent>();
        co.componentName = "NameComponent";
        co.data["name"] = n->getName();
        be.componentOverrides.push_back(std::move(co));
    }

    blueprint->addEntity(be);
    world.registerBlueprint(std::move(blueprint));
    EditorConsole::Log("Created blueprint '" + name + "' from entity " + std::to_string(id));
    return true;
}

ecs::EntityID SpawnBlueprint(const std::string& name, const glm::vec3& pos) {
    ecs::World& world = g_editor.world();
    ecs::Blueprint* bp = world.getBlueprint(name);
    if (!bp) return ecs::INVALID_ENTITY_ID;

    const auto& entities = bp->getEntities();
    if (entities.empty()) return ecs::INVALID_ENTITY_ID;
    const ecs::BlueprintEntity& be = entities[0];

    ecs::Entity e = world.createEntityWithComponents<ecs::TransformComponent, ecs::NameComponent>();
    if (!e.isValid()) return ecs::INVALID_ENTITY_ID;

    if (auto* t = world.getComponentArchetype<ecs::TransformComponent>(e)) {
        for (const auto& co : be.componentOverrides) {
            if (co.typeID != ecs::getComponentTypeID<ecs::TransformComponent>()) continue;
            if (co.data.isMember("position")) {
                t->position = glm::vec3(co.data["position"][0].asFloat(),
                                        co.data["position"][1].asFloat(),
                                        co.data["position"][2].asFloat());
            }
            if (co.data.isMember("scale")) {
                t->scale = glm::vec3(co.data["scale"][0].asFloat(),
                                     co.data["scale"][1].asFloat(),
                                     co.data["scale"][2].asFloat());
            }
            if (co.data.isMember("rotation")) {
                t->rotation = glm::quat(co.data["rotation"][3].asFloat(),
                                        co.data["rotation"][0].asFloat(),
                                        co.data["rotation"][1].asFloat(),
                                        co.data["rotation"][2].asFloat());
            }
        }
        t->position += pos;  // spawn offset
    }

    if (auto* n = world.getComponentArchetype<ecs::NameComponent>(e)) {
        for (const auto& co : be.componentOverrides) {
            if (co.typeID == ecs::getComponentTypeID<ecs::NameComponent>() && co.data.isMember("name")) {
                n->setName(co.data["name"].asString());
                break;
            }
        }
    }

    EditorConsole::Log("Spawned blueprint '" + name + "' -> entity " + std::to_string(e.id));
    return e.id;
}

} // namespace EntityManager
