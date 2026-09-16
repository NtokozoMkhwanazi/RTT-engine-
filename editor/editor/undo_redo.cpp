#include "undo_redo.h"
#include "editor_state.h"
#include "entity_manager.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>

// ============================================================================
// Undo/Redo implementation
// ============================================================================
namespace UndoRedo {

namespace {

enum class CommandType { Create, Delete, Transform };

struct Command {
    CommandType type = CommandType::Create;
    std::string entityName;
    // For Create/Delete: full entity snapshot. For Transform: { "before": ..,
    // "after": .. } each holding the snapshot's transform sub-object.
    Json::Value data;
    std::chrono::steady_clock::time_point time = std::chrono::steady_clock::now();
};

std::vector<Command> g_undo;
std::vector<Command> g_redo;

// Pending transform drag state (started but not yet ended).
struct PendingDrag {
    std::string entityName;
    ecs::TransformComponent before;
    bool active = false;
};
PendingDrag g_pendingDrag;

ecs::World& World() { return g_editor.world(); }

// ---------------------------------------------------------------------------
// Entity helpers
// ---------------------------------------------------------------------------

ecs::EntityID FindEntityByName(const std::string& name) {
    if (name.empty()) return ecs::INVALID_ENTITY_ID;
    ecs::EntityID found = ecs::INVALID_ENTITY_ID;
    World().forEach<ecs::NameComponent>(
        [&](ecs::EntityID id, ecs::NameComponent& n) {
            if (found == ecs::INVALID_ENTITY_ID && std::string(n.getName()) == name) {
                found = id;
            }
        });
    return found;
}

bool EntityAlive(ecs::EntityID id) {
    return id != ecs::INVALID_ENTITY_ID && World().isAlive(ecs::Entity{id});
}

// ---------------------------------------------------------------------------
// Snapshot helpers (transform / mesh / name -> JSON)
// ---------------------------------------------------------------------------

Json::Value TransformToJson(const ecs::TransformComponent& t) {
    Json::Value j;
    j["position"][0] = t.position.x; j["position"][1] = t.position.y; j["position"][2] = t.position.z;
    j["rotation"][0] = t.rotation.x; j["rotation"][1] = t.rotation.y;
    j["rotation"][2] = t.rotation.z; j["rotation"][3] = t.rotation.w;
    j["scale"][0] = t.scale.x; j["scale"][1] = t.scale.y; j["scale"][2] = t.scale.z;
    return j;
}

void ApplyTransformFromJson(ecs::TransformComponent& t, const Json::Value& j) {
    if (j.isMember("position")) {
        t.position = glm::vec3(j["position"][0].asFloat(), j["position"][1].asFloat(), j["position"][2].asFloat());
    }
    if (j.isMember("rotation")) {
        t.rotation = glm::quat(j["rotation"][3].asFloat(), j["rotation"][0].asFloat(),
                               j["rotation"][1].asFloat(), j["rotation"][2].asFloat());
    }
    if (j.isMember("scale")) {
        t.scale = glm::vec3(j["scale"][0].asFloat(), j["scale"][1].asFloat(), j["scale"][2].asFloat());
    }
}

Json::Value SnapshotEntity(ecs::EntityID id) {
    Json::Value snap;
    ecs::Entity e{id};
    if (auto* t = World().getComponentArchetype<ecs::TransformComponent>(e)) {
        snap["transform"] = TransformToJson(*t);
    }
    if (auto* m = World().getComponentArchetype<ecs::MeshComponent>(e)) {
        snap["mesh"]["meshType"] = static_cast<int>(m->meshType);
        snap["mesh"]["meshID"] = m->meshID;
        snap["mesh"]["visible"] = m->visible;
        snap["mesh"]["color"][0] = m->color.r;
        snap["mesh"]["color"][1] = m->color.g;
        snap["mesh"]["color"][2] = m->color.b;
    }
    if (auto* n = World().getComponentArchetype<ecs::NameComponent>(e)) {
        snap["name"] = n->getName();
    }
    if (auto* l = World().getComponentArchetype<ecs::LightComponent>(e)) {
        snap["light"]["type"] = static_cast<int>(l->type);
        snap["light"]["color"][0] = l->color.r;
        snap["light"]["color"][1] = l->color.g;
        snap["light"]["color"][2] = l->color.b;
        snap["light"]["intensity"] = l->intensity;
        snap["light"]["range"] = l->range;
        snap["light"]["temperature"] = l->temperature;
        snap["light"]["enabled"] = l->enabled;
        snap["light"]["castShadows"] = l->castShadows;
    }
    if (auto* c = World().getComponentArchetype<ecs::CameraComponent>(e)) {
        snap["camera"]["fov"] = c->fov;
        snap["camera"]["nearPlane"] = c->nearPlane;
        snap["camera"]["farPlane"] = c->farPlane;
        snap["camera"]["isOrthographic"] = c->isOrthographic;
        snap["camera"]["isActive"] = c->isActive;
    }
    return snap;
}

// Apply a snapshot to an existing entity (overwrites the three supported
// component types in place).
void ApplySnapshotToEntity(ecs::EntityID id, const Json::Value& snap) {
    ecs::Entity e{id};
    if (snap.isMember("transform")) {
        if (auto* t = World().getComponentArchetype<ecs::TransformComponent>(e)) {
            ApplyTransformFromJson(*t, snap["transform"]);
        }
    }
    if (snap.isMember("mesh")) {
        World().addComponentArchetype<ecs::MeshComponent>(e);
        if (auto* m = World().getComponentArchetype<ecs::MeshComponent>(e)) {
            m->meshType = static_cast<ecs::MeshType>(snap["mesh"]["meshType"].asInt());
            m->meshID = snap["mesh"]["meshID"].asInt();
            m->visible = snap["mesh"]["visible"].asBool();
            m->color = glm::vec3(snap["mesh"]["color"][0].asFloat(),
                                 snap["mesh"]["color"][1].asFloat(),
                                 snap["mesh"]["color"][2].asFloat());
        }
    }
    if (snap.isMember("name")) {
        World().addComponentArchetype<ecs::NameComponent>(e);
        if (auto* n = World().getComponentArchetype<ecs::NameComponent>(e)) {
            n->setName(snap["name"].asString());
        }
    }
    if (snap.isMember("light")) {
        World().addComponentArchetype<ecs::LightComponent>(e);
        if (auto* l = World().getComponentArchetype<ecs::LightComponent>(e)) {
            l->type = static_cast<ecs::LightType>(snap["light"]["type"].asInt());
            l->color = glm::vec3(snap["light"]["color"][0].asFloat(),
                                 snap["light"]["color"][1].asFloat(),
                                 snap["light"]["color"][2].asFloat());
            l->intensity = snap["light"]["intensity"].asFloat();
            l->range = snap["light"]["range"].asFloat();
            l->temperature = snap["light"]["temperature"].asFloat();
            l->enabled = snap["light"]["enabled"].asBool();
            l->castShadows = snap["light"]["castShadows"].asBool();
        }
    }
    if (snap.isMember("camera")) {
        World().addComponentArchetype<ecs::CameraComponent>(e);
        if (auto* c = World().getComponentArchetype<ecs::CameraComponent>(e)) {
            c->fov = snap["camera"]["fov"].asFloat();
            c->nearPlane = snap["camera"]["nearPlane"].asFloat();
            c->farPlane = snap["camera"]["farPlane"].asFloat();
            c->isOrthographic = snap["camera"]["isOrthographic"].asBool();
            c->isActive = snap["camera"]["isActive"].asBool();
        }
    }
}

// Recreate an entity from a snapshot (used by undo-of-delete and redo-of-create).
// Returns the new entity id. If an entity with the same name already exists the
// snapshot is applied onto it instead.
ecs::EntityID RecreateFromSnapshot(const std::string& name, const Json::Value& snap) {
    ecs::EntityID existing = FindEntityByName(name);
    if (EntityAlive(existing)) {
        ApplySnapshotToEntity(existing, snap);
        return existing;
    }

    ecs::Entity e = World().createEntityWithComponents<ecs::TransformComponent,
                                                       ecs::MeshComponent,
                                                       ecs::NameComponent>();
    if (!e.isValid()) return ecs::INVALID_ENTITY_ID;
    ApplySnapshotToEntity(e.id, snap);
    return e.id;
}

// ---------------------------------------------------------------------------
// Command push helpers
// ---------------------------------------------------------------------------

void PushCommand(Command cmd) {
    g_undo.push_back(std::move(cmd));
    g_redo.clear();  // a new push invalidates the redo branch
}

std::string EntityNameOrEmpty(ecs::EntityID id) {
    if (!EntityAlive(id)) return std::string();
    if (auto* n = World().getComponentArchetype<ecs::NameComponent>(ecs::Entity{id})) {
        return std::string(n->getName());
    }
    return std::string();
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Clear() {
    g_undo.clear();
    g_redo.clear();
    g_pendingDrag = PendingDrag{};
}

bool CanUndo() { return !g_undo.empty(); }
bool CanRedo() { return !g_redo.empty(); }

void RecordCreate(ecs::EntityID id) {
    if (!EntityAlive(id)) return;
    const std::string name = EntityNameOrEmpty(id);
    Command cmd;
    cmd.type = CommandType::Create;
    cmd.entityName = name;
    cmd.data = SnapshotEntity(id);
    PushCommand(std::move(cmd));
}

void RecordCreateMany(const std::vector<ecs::EntityID>& ids) {
    // Build a combined command with one entity per node plus parent links by
    // name so the hierarchy can be restored on redo.
    Command cmd;
    cmd.type = CommandType::Create;
    cmd.entityName = "__many__";
    Json::Value entitiesJson;
    int index = 0;
    for (ecs::EntityID id : ids) {
        if (!EntityAlive(id)) continue;
        const std::string name = EntityNameOrEmpty(id);
        if (name.empty()) continue;
        Json::Value entry = SnapshotEntity(id);
        entry["_name"] = name;
        // Parent link (by name) so redo can rebuild the hierarchy.
        ecs::Entity parent = World().getParent(ecs::Entity{id});
        if (parent.isValid()) {
            if (auto* pn = World().getComponentArchetype<ecs::NameComponent>(parent)) {
                entry["_parent"] = pn->getName();
            }
        }
        entitiesJson[index++] = entry;
    }
    if (entitiesJson.empty()) return;
    cmd.data["entities"] = entitiesJson;
    PushCommand(std::move(cmd));
}

void RecordDelete(ecs::EntityID id) {
    if (!EntityAlive(id)) return;
    const std::string name = EntityNameOrEmpty(id);
    // Engine-owned entities are not recorded.
    if (name == "Bot Player") return;
    Command cmd;
    cmd.type = CommandType::Delete;
    cmd.entityName = name;
    cmd.data = SnapshotEntity(id);
    PushCommand(std::move(cmd));
}

void RecordTransformStart(ecs::EntityID id, const ecs::TransformComponent& before) {
    if (!EntityAlive(id)) return;
    g_pendingDrag.entityName = EntityNameOrEmpty(id);
    g_pendingDrag.before = before;
    g_pendingDrag.active = true;
}

void RecordTransformEnd(ecs::EntityID id, const ecs::TransformComponent& after) {
    if (!g_pendingDrag.active) return;  // end without start is a no-op
    if (g_pendingDrag.entityName != EntityNameOrEmpty(id)) {
        g_pendingDrag = PendingDrag{};
        return;
    }

    const ecs::TransformComponent before = g_pendingDrag.before;
    g_pendingDrag = PendingDrag{};

    // Nothing moved -> no command.
    if (before.position == after.position && before.rotation == after.rotation &&
        before.scale == after.scale) {
        return;
    }

    const std::string name = EntityNameOrEmpty(id);

    // Coalesce with a previous transform drag on the same entity inside the
    // coalesce window: keep the FIRST command's "before", update "after".
    if (!g_undo.empty()) {
        Command& top = g_undo.back();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - top.time);
        if (top.type == CommandType::Transform && top.entityName == name &&
            elapsed <= kCoalesceWindow) {
            top.data["after"] = TransformToJson(after);
            top.time = std::chrono::steady_clock::now();
            return;
        }
    }

    Command cmd;
    cmd.type = CommandType::Transform;
    cmd.entityName = name;
    cmd.data["before"] = TransformToJson(before);
    cmd.data["after"] = TransformToJson(after);
    PushCommand(std::move(cmd));
}

void Undo() {
    if (g_undo.empty()) return;
    Command cmd = std::move(g_undo.back());
    g_undo.pop_back();

    ecs::EntityID id = FindEntityByName(cmd.entityName);
    if (cmd.type == CommandType::Create) {
        // Create -> destroy. Handle both single and multi-entity commands.
        if (cmd.entityName == "__many__" && cmd.data.isMember("entities")) {
            for (const auto& entry : cmd.data["entities"]) {
                const ecs::EntityID eid = FindEntityByName(entry["_name"].asString());
                if (EntityAlive(eid)) {
                    World().destroyEntity(ecs::Entity{eid});
                    if (g_editor.selectedEntity() == eid) {
                        g_editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
                    }
                }
            }
        } else if (EntityAlive(id)) {
            World().destroyEntity(ecs::Entity{id});
            if (g_editor.selectedEntity() == id) {
                g_editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
            }
        }
    } else if (cmd.type == CommandType::Delete) {
        RecreateFromSnapshot(cmd.entityName, cmd.data);
    } else if (cmd.type == CommandType::Transform) {
        if (EntityAlive(id)) {
            if (auto* t = World().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id})) {
                ApplyTransformFromJson(*t, cmd.data["before"]);
            }
        }
    }

    g_redo.push_back(std::move(cmd));
}

void Redo() {
    if (g_redo.empty()) return;
    Command cmd = std::move(g_redo.back());
    g_redo.pop_back();

    if (cmd.type == CommandType::Create) {
        if (cmd.entityName == "__many__" && cmd.data.isMember("entities")) {
            // Recreate all nodes first, then restore parent links by name.
            std::vector<ecs::EntityID> created;
            for (const auto& entry : cmd.data["entities"]) {
                created.push_back(RecreateFromSnapshot(entry["_name"].asString(), entry));
            }
            int i = 0;
            for (const auto& entry : cmd.data["entities"]) {
                if (entry.isMember("_parent") && i < (int)created.size()) {
                    const ecs::EntityID parentId = FindEntityByName(entry["_parent"].asString());
                    if (EntityAlive(parentId) && EntityAlive(created[i])) {
                        World().setParent(ecs::Entity{created[i]}, ecs::Entity{parentId});
                    }
                }
                ++i;
            }
        } else {
            RecreateFromSnapshot(cmd.entityName, cmd.data);
        }
    } else if (cmd.type == CommandType::Delete) {
        const ecs::EntityID id = FindEntityByName(cmd.entityName);
        if (EntityAlive(id)) {
            World().destroyEntity(ecs::Entity{id});
            if (g_editor.selectedEntity() == id) {
                g_editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
            }
        }
    } else if (cmd.type == CommandType::Transform) {
        const ecs::EntityID id = FindEntityByName(cmd.entityName);
        if (EntityAlive(id)) {
            if (auto* t = World().getComponentArchetype<ecs::TransformComponent>(ecs::Entity{id})) {
                ApplyTransformFromJson(*t, cmd.data["after"]);
            }
        }
    }

    g_undo.push_back(std::move(cmd));
}

Json::Value SerializeHistory() {
    Json::Value root;
    Json::Value cmds;
    int i = 0;
    for (const Command& cmd : g_undo) {
        Json::Value j;
        switch (cmd.type) {
            case CommandType::Create:  j["type"] = "create";  break;
            case CommandType::Delete:  j["type"] = "delete";  break;
            case CommandType::Transform: j["type"] = "transform"; break;
        }
        j["name"] = cmd.entityName;
        j["data"] = cmd.data;
        cmds[i++] = j;
    }
    root["commands"] = cmds;
    return root;
}

void DeserializeHistory(const Json::Value& json) {
    g_undo.clear();
    g_redo.clear();
    g_pendingDrag = PendingDrag{};

    if (!json.isMember("commands") || !json["commands"].isArray()) {
        return;  // scene without history -> empty stacks
    }

    for (const auto& j : json["commands"]) {
        Command cmd;
        const std::string type = j["type"].asString();
        if (type == "create") cmd.type = CommandType::Create;
        else if (type == "delete") cmd.type = CommandType::Delete;
        else if (type == "transform") cmd.type = CommandType::Transform;
        else continue;
        cmd.entityName = j["name"].asString();
        cmd.data = j["data"];
        cmd.time = std::chrono::steady_clock::now();
        g_undo.push_back(std::move(cmd));
    }
}

} // namespace UndoRedo
