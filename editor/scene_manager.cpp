#include "scene_manager.h"
#include "undo_redo.h"
#include "entity_manager.h"
#include "ecs/components/Components.h"
#include "ecs/components/LightComponent.h"
#include "ecs/components/ModelComponent.h"
#include "modelSystem/ModelManager.h"
#include <jsoncpp/json/json.h>
#include <fstream>
#include <sstream>
#include <iostream>

namespace SceneManager {

static std::string g_currentSceneFile;

namespace {

void WriteVec3(Json::Value& j, const glm::vec3& v) {
    j[0] = v.x; j[1] = v.y; j[2] = v.z;
}

glm::vec3 ReadVec3(const Json::Value& j) {
    return glm::vec3(j[0].asFloat(), j[1].asFloat(), j[2].asFloat());
}

// Re-attach a saved LightComponent to an entity (adds the component if the
// entity didn't have one).
void RestoreLight(ecs::World& world, ecs::Entity ent, const Json::Value& e) {
    if (!e.isMember("light")) return;
    world.addComponentArchetype<ecs::LightComponent>(ent);
    if (auto* l = world.getComponentArchetype<ecs::LightComponent>(ent)) {
        const Json::Value& j = e["light"];
        l->type = static_cast<ecs::LightType>(j["type"].asInt());
        l->color = ReadVec3(j["color"]);
        l->intensity = j["intensity"].asFloat();
        l->range = j["range"].asFloat();
        l->enabled = j["enabled"].asBool();
        l->castShadows = j["castShadows"].asBool();
        l->temperature = j["temperature"].asFloat();
        l->useTemperature = j["useTemperature"].asBool();
    }
}

// Re-attach a saved ModelComponent: restores the path + material overrides and
// asks the model registry to (re)load the mesh so the entity renders again.
void RestoreModel(ecs::World& world, ecs::Entity ent, const Json::Value& e) {
    if (!e.isMember("model")) return;
    world.addComponentArchetype<ecs::ModelComponent>(ent);
    if (auto* m = world.getComponentArchetype<ecs::ModelComponent>(ent)) {
        const Json::Value& j = e["model"];
        const std::string path = j["path"].asString();
        m->setModelPath(path);
        m->visible = j["visible"].asBool();
        m->useMaterialOverrides = j["useOverrides"].asBool();
        m->albedoOverride = ReadVec3(j["albedo"]);
        m->metallicOverride = j["metallic"].asFloat();
        m->roughnessOverride = j["roughness"].asFloat();
        if (!path.empty()) {
            m->modelHandle = ModelSystem::ModelRegistry::getInstance().load(path);
        }
    }
}

} // namespace

bool SaveScene(const std::string& filename, ecs::World& world) {
    Json::Value root;
    root["version"] = "1.0";

    Json::Value entitiesJson;
    int index = 0;
    world.forEach<ecs::TransformComponent, ecs::NameComponent>(
        [&](ecs::EntityID id, ecs::TransformComponent& t, ecs::NameComponent& n) {
            Json::Value e;
            e["name"] = n.getName();
            WriteVec3(e["position"], t.position);
            e["rotation"][0] = t.rotation.x; e["rotation"][1] = t.rotation.y;
            e["rotation"][2] = t.rotation.z; e["rotation"][3] = t.rotation.w;
            WriteVec3(e["scale"], t.scale);

            if (auto* m = world.getComponentArchetype<ecs::MeshComponent>(ecs::Entity{id})) {
                e["mesh"]["meshType"] = static_cast<int>(m->meshType);
                e["mesh"]["meshID"] = m->meshID;
                e["mesh"]["visible"] = m->visible;
                WriteVec3(e["mesh"]["color"], m->color);
            }

            // Lights (Add menu -> Point Light) survive save/load so a scene
            // with editor-placed lighting reopens lit.
            if (auto* l = world.getComponentArchetype<ecs::LightComponent>(ecs::Entity{id})) {
                e["light"]["type"] = static_cast<int>(l->type);
                WriteVec3(e["light"]["color"], l->color);
                e["light"]["intensity"] = l->intensity;
                e["light"]["range"] = l->range;
                e["light"]["enabled"] = l->enabled;
                e["light"]["castShadows"] = l->castShadows;
                e["light"]["temperature"] = l->temperature;
                e["light"]["useTemperature"] = l->useTemperature;
            }

            // Models (Import Model...) keep their path + material overrides so
            // the mesh reappears on load.
            if (auto* mod = world.getComponentArchetype<ecs::ModelComponent>(ecs::Entity{id})) {
                e["model"]["path"] = mod->hasModelPath() ? mod->getModelPath() : "";
                e["model"]["visible"] = mod->visible;
                e["model"]["useOverrides"] = mod->useMaterialOverrides;
                WriteVec3(e["model"]["albedo"], mod->albedoOverride);
                e["model"]["metallic"] = mod->metallicOverride;
                e["model"]["roughness"] = mod->roughnessOverride;
            }
            entitiesJson[index++] = e;
        });
    root["entities"] = entitiesJson;

    // Persist the undo history (remapped by name on load).
    root["history"] = UndoRedo::SerializeHistory();

    Json::StreamWriterBuilder builder;
    builder["commentStyle"] = "None";
    builder["indentation"] = "  ";

    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "ERROR: Could not open file for writing: " << filename << "\n";
        return false;
    }
    file << Json::writeString(builder, root);
    file.close();

    g_currentSceneFile = filename;
    std::cout << "Scene saved to: " << filename << "\n";
    return true;
}

bool LoadScene(const std::string& filename, ecs::World& world) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "ERROR: Could not open scene file: " << filename << "\n";
        return false;
    }

    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string errors;
    if (!Json::parseFromStream(reader, file, &root, &errors)) {
        std::cerr << "ERROR: Failed to parse scene JSON: " << errors << "\n";
        return false;
    }
    file.close();

    const Json::Value& entitiesJson = root["entities"];
    for (const auto& e : entitiesJson) {
        const std::string name = e["name"].asString();

        // Find an existing entity with the same name; if present, restore its
        // saved state in place (entities accumulate across loads, so never
        // clear the world first).
        ecs::EntityID existing = ecs::INVALID_ENTITY_ID;
        world.forEach<ecs::NameComponent>(
            [&](ecs::EntityID id, ecs::NameComponent& n) {
                if (existing == ecs::INVALID_ENTITY_ID && std::string(n.getName()) == name) {
                    existing = id;
                }
            });

        if (existing != ecs::INVALID_ENTITY_ID) {
            ecs::Entity ent{existing};
            if (auto* t = world.getComponentArchetype<ecs::TransformComponent>(ent)) {
                t->position = ReadVec3(e["position"]);
                if (e.isMember("rotation")) {
                    t->rotation = glm::quat(e["rotation"][3].asFloat(), e["rotation"][0].asFloat(),
                                            e["rotation"][1].asFloat(), e["rotation"][2].asFloat());
                }
                if (e.isMember("scale")) t->scale = ReadVec3(e["scale"]);
            }
            if (e.isMember("mesh")) {
                world.addComponentArchetype<ecs::MeshComponent>(ent);
                if (auto* m = world.getComponentArchetype<ecs::MeshComponent>(ent)) {
                    m->meshType = static_cast<ecs::MeshType>(e["mesh"]["meshType"].asInt());
                    m->meshID = e["mesh"]["meshID"].asInt();
                    m->visible = e["mesh"]["visible"].asBool();
                    if (e["mesh"].isMember("color")) m->color = ReadVec3(e["mesh"]["color"]);
                }
            }
            RestoreLight(world, ent, e);
            RestoreModel(world, ent, e);
        } else {
            ecs::Entity created = world.createEntityWithComponents<ecs::TransformComponent,
                                                                   ecs::NameComponent>();
            if (!created.isValid()) continue;
            if (auto* t = world.getComponentArchetype<ecs::TransformComponent>(created)) {
                t->position = ReadVec3(e["position"]);
                if (e.isMember("rotation")) {
                    t->rotation = glm::quat(e["rotation"][3].asFloat(), e["rotation"][0].asFloat(),
                                            e["rotation"][1].asFloat(), e["rotation"][2].asFloat());
                }
                if (e.isMember("scale")) t->scale = ReadVec3(e["scale"]);
            }
            if (auto* n = world.getComponentArchetype<ecs::NameComponent>(created)) {
                n->setName(name);
            }
            if (e.isMember("mesh")) {
                world.addComponentArchetype<ecs::MeshComponent>(created);
                if (auto* m = world.getComponentArchetype<ecs::MeshComponent>(created)) {
                    m->meshType = static_cast<ecs::MeshType>(e["mesh"]["meshType"].asInt());
                    m->meshID = e["mesh"]["meshID"].asInt();
                    m->visible = e["mesh"]["visible"].asBool();
                    if (e["mesh"].isMember("color")) m->color = ReadVec3(e["mesh"]["color"]);
                }
            }
            RestoreLight(world, created, e);
            RestoreModel(world, created, e);
        }
    }

    // Restore undo history (remapped by name automatically).
    if (root.isMember("history")) {
        UndoRedo::DeserializeHistory(root["history"]);
    } else {
        UndoRedo::DeserializeHistory(Json::Value());
    }

    g_currentSceneFile = filename;
    std::cout << "Scene loaded from: " << filename << "\n";
    return true;
}

const std::string& GetCurrentSceneFile() {
    return g_currentSceneFile;
}

void SetCurrentSceneFile(const std::string& filename) {
    g_currentSceneFile = filename;
}

} // namespace SceneManager
