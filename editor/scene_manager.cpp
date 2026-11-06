#include "scene_manager.h"
#include "undo_redo.h"
#include "entity_manager.h"
#include "ecs/components/Components.h"
#include "ecs/components/LightComponent.h"
#include "ecs/components/ModelComponent.h"
#include "modelSystem/ModelManager.h"
#include "modelSystem/Model.h"          // Model::LoadModelData / CreateFromAsync / AsyncModelData (async scene load)
#include "modelSystem/ModelHandle.h"     // ModelRegistry::registerModel / ModelHandle
#include "editor/editor_state.h"         // Editor::Editor, g_editor, SceneLoadAsyncState
#include "editor/console.h"             // EditorConsole::Log (resolve diagnostics)
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
    if (!world.hasComponent<ecs::LightComponent>(ent)) {
        world.addComponentArchetype<ecs::LightComponent>(ent);
    }
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
    if (!world.hasComponent<ecs::ModelComponent>(ent)) {
        world.addComponentArchetype<ecs::ModelComponent>(ent);
    }
    if (auto* m = world.getComponentArchetype<ecs::ModelComponent>(ent)) {
        const Json::Value& j = e["model"];
        const std::string path = j["path"].asString();
        m->setModelPath(path);
        m->visible = j["visible"].asBool();
        m->useMaterialOverrides = j["useOverrides"].asBool();
        m->albedoOverride = ReadVec3(j["albedo"]);
        m->metallicOverride = j["metallic"].asFloat();
        m->roughnessOverride = j["roughness"].asFloat();
        m->windSway = j.get("windSway", 0.0f).asFloat();
        if (!path.empty()) {
            if (g_editor.uiState.sceneLoadAsync.active) {
                // Engine-layer async scene load (mirrors the NPC / content-browser
                // resolve): dispatch the heavy Assimp parse + texture decode (2048
                // streaming cap) on the bounded JobSystem pool. The GL upload
                // (CreateFromAsync + registerModel) and the ModelHandle attach to
                // this entity happen later, on the main thread, time-sliced by
                // CommitSceneModelLoads() — so the render thread never blocks.
                // modelHandle is intentionally left invalid here (render skips
                // invalid handles); it is filled in once the worker + GL commit finish.
                using SlaState = decltype(g_editor.uiState.sceneLoadAsync);
                SlaState::Request req;
                req.assetPath  = path;
                req.entityID   = ent.id;
                req.progress   = std::make_shared<std::atomic<float>>(0.0f);
                req.promise    = std::make_shared<std::promise<std::unique_ptr<AsyncModelData>>>();
                req.dataFuture = req.promise->get_future();
                const std::string assetPath = path;
                auto progressAtom = req.progress;
                auto promise      = req.promise;
                constexpr int kWorldTextureCap = 2048;  // 4K terrain/prop maps -> 2K decode cap
                g_editor.jobSystem().addJob([assetPath, progressAtom, promise]() {
                    try {
                        promise->set_value(Model::LoadModelData(assetPath, progressAtom.get(),
                                                                kWorldTextureCap));
                    } catch (const std::exception& e) {
                        auto r = std::make_unique<AsyncModelData>();
                        r->success = false;
                        r->error = std::string("Exception: ") + e.what();
                        promise->set_value(std::move(r));
                    } catch (...) {
                        auto r = std::make_unique<AsyncModelData>();
                        r->success = false;
                        r->error = "Unknown exception during scene async load";
                        promise->set_value(std::move(r));
                    }
                });
                g_editor.uiState.sceneLoadAsync.totalIssued++;
                g_editor.uiState.sceneLoadAsync.requests.emplace_back(std::move(req));
            } else {
                // Sync boot path (and tests / headless): full parse + GL inline.
                m->modelHandle = ModelSystem::ModelRegistry::getInstance().load(path);
            }
        }
    }
}

} // namespace

bool SaveScene(const std::string& filename, ecs::World& world) {
    Json::Value root;
    root["version"] = "1.0";

    Json::Value entitiesJson;
    world.forEach<ecs::TransformComponent, ecs::NameComponent>(
        [&](ecs::EntityID id, ecs::TransformComponent& t, ecs::NameComponent& n) {
            Json::Value e;
            e["name"] = n.getName();
            WriteVec3(e["position"], t.position);
            e["rotation"][0] = t.rotation.x; e["rotation"][1] = t.rotation.y;
            e["rotation"][2] = t.rotation.z; e["rotation"][3] = t.rotation.w;
            WriteVec3(e["scale"], t.scale);

            if (auto* m = world.getComponentArchetype<ecs::MeshComponent>(id)) {
                e["mesh"]["meshType"] = static_cast<int>(m->meshType);
                e["mesh"]["meshID"] = m->meshID;
                e["mesh"]["visible"] = m->visible;
                WriteVec3(e["mesh"]["color"], m->color);
            }

            // Lights (Add menu -> Point Light) survive save/load so a scene
            // with editor-placed lighting reopens lit.
            if (auto* l = world.getComponentArchetype<ecs::LightComponent>(id)) {
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
            if (auto* mod = world.getComponentArchetype<ecs::ModelComponent>(id)) {
                e["model"]["path"] = mod->hasModelPath() ? mod->getModelPath() : "";
                e["model"]["visible"] = mod->visible;
                e["model"]["useOverrides"] = mod->useMaterialOverrides;
                WriteVec3(e["model"]["albedo"], mod->albedoOverride);
                e["model"]["metallic"] = mod->metallicOverride;
                e["model"]["roughness"] = mod->roughnessOverride;
            }
            entitiesJson.append(e);
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
                if (!world.hasComponent<ecs::MeshComponent>(ent)) {
                    world.addComponentArchetype<ecs::MeshComponent>(ent);
                }
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
                if (!world.hasComponent<ecs::MeshComponent>(created)) {
                    world.addComponentArchetype<ecs::MeshComponent>(created);
                }
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

// Pump deferred scene model loads (CPU parse already running on the worker
// pool) — GL upload + ModelHandle attach, time-sliced on the MAIN (GL) thread
// so the editor stays >=30 FPS while a heavy world prop / terrain-texture
// model commits. Mirrors CommitContentBrowserSpawns().
void CommitSceneModelLoads(Editor::Editor& editor) {
    auto& state = editor.uiState.sceneLoadAsync;
    if (!state.isLoading) return;
    if (state.requests.empty()) {
        // Nothing was dispatched (e.g. a scene with no models). Close the bar.
        state.isLoading = false;
        state.active    = false;
        return;
    }

    using SlaState = decltype(editor.uiState.sceneLoadAsync);

    // 1) Promote Parsing -> ReadyToCommit for workers that finished parsing.
    for (auto& req : state.requests) {
        if (req.phase == SlaState::Phase::Parsing &&
            req.dataFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            req.phase = SlaState::Phase::ReadyToCommit;
        }
    }

    // 2) Time-sliced GL commit: bounded CPU budget per frame, >=1 model/frame.
    static constexpr double kBudgetMs = 4.0;
    const auto t0 = std::chrono::steady_clock::now();
    bool committedOne = false;
    auto& world = editor.world();
    for (auto it = state.requests.begin(); it != state.requests.end(); ) {
        if (it->phase != SlaState::Phase::ReadyToCommit) { ++it; continue; }
        if (committedOne) {
            using namespace std::chrono;
            if (duration<double, std::milli>(steady_clock::now() - t0).count() > kBudgetMs) break;
        }

        auto parsed = it->dataFuture.get();  // worker is done (promise fulfilled)
        if (!parsed || !parsed->success) {
            it->phase = SlaState::Phase::Failed;
            it->error = (!parsed || parsed->error.empty()) ? "parse failed" : parsed->error;
            EditorConsole::Log("[SceneLoad] Resolve failed: " + it->assetPath +
                                 " - " + it->error, 2);
            it = state.requests.erase(it);
            committedOne = true;
            continue;
        }

        std::string err;
        Model* glModel = Model::CreateFromAsync(std::move(parsed), &err);
        if (!glModel) {
            it->phase = SlaState::Phase::Failed;
            it->error = err.empty() ? "GL upload failed" : err;
            EditorConsole::Log("[SceneLoad] GL commit failed: " + it->assetPath +
                                 " - " + it->error, 2);
            it = state.requests.erase(it);
            committedOne = true;
            continue;
        }

        // Cache the GL-uploaded model by path so any later sync load(path) is a
        // no-parse cache hit (mirrors the NPC / content-browser registerModel step).
        auto handle = ModelSystem::ModelRegistry::getInstance().registerModel(glModel, it->assetPath);
        if (!handle.isValid()) {
            delete glModel;
            EditorConsole::Log("[SceneLoad] Registry rejected: " + it->assetPath, 2);
            it = state.requests.erase(it);
            committedOne = true;
            continue;
        }

        // Attach the handle to the restored entity's ModelComponent (invalid until
        // now, so the renderer was already safely skipping it).
        if (auto* mc = world.getComponentArchetype<ecs::ModelComponent>(ecs::Entity{ it->entityID })) {
            mc->modelHandle = handle;
        } else {
            EditorConsole::Log("[SceneLoad] Host entity gone: " + it->assetPath, 2);
            delete glModel;
        }
        it = state.requests.erase(it);
        committedOne = true;
    }

    // All models resolved (or failed): drop the bar.
    if (state.requests.empty()) {
        state.isLoading = false;
        state.active    = false;
        EditorConsole::Log("[SceneLoad] All scene models resolved.");
    }
}

} // namespace SceneManager
