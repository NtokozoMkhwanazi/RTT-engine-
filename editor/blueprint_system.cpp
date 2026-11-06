/**
 * blueprint_system.cpp
 *
 * Implementation of Editor::BlueprintSystem — blueprint instantiation and
 * entity-to-blueprint capture.
 */

#include "blueprint_system.h"
#include "ecs/components/Components.h"   // TransformComponent, ModelComponent, etc.
#include <functional>
#include <filesystem>

namespace Editor {

// ── Constructor ─────────────────────────────────────────────────────────────

BlueprintSystem::BlueprintSystem(ecs::World& world)
    : m_world(world) {}

// ── Blueprint registry (delegation) ──────────────────────────────────────────

bool BlueprintSystem::hasBlueprint(const std::string& name) const {
    return m_world.getBlueprint(name) != nullptr;
}

void BlueprintSystem::registerBlueprint(std::unique_ptr<ecs::Blueprint> blueprint) {
    m_world.registerBlueprint(std::move(blueprint));
}

ecs::Blueprint* BlueprintSystem::getBlueprint(const std::string& name) {
    return m_world.getBlueprint(name);
}

const ecs::Blueprint* BlueprintSystem::getBlueprint(const std::string& name) const {
    return m_world.getBlueprint(name);
}

std::vector<std::string> BlueprintSystem::getBlueprintNames() const {
    return m_world.getBlueprintNames();
}

// ── Component override application (instantiate) ──────────────────────────────
//
// Dispatch on componentName to add the right archetype component and
// deserialize its fields from the ComponentOverride's JSON data. Only the
// component types that have serialisable fields are handled; unknown types
// are silently skipped (the entity still gets created, just without that
// component).

namespace {

/// Add a component to `entity` and apply `override.data` to it.
void applyOverride(ecs::World& world, ecs::Entity entity,
                   const ecs::ComponentOverride& override) {
    const std::string& name = override.componentName;
    const Json::Value& data = override.data;

    if (name == "TransformComponent") {
        auto& t = world.addComponentArchetype<ecs::TransformComponent>(entity);
        if (data.isMember("position") && data["position"].isArray() && data["position"].size() == 3) {
            t.position = glm::vec3(
                data["position"][0].asFloat(),
                data["position"][1].asFloat(),
                data["position"][2].asFloat());
        }
        if (data.isMember("rotation") && data["rotation"].isArray() && data["rotation"].size() == 4) {
            // Stored as [x, y, z, w] — glm::quat(w, x, y, z)
            t.rotation = glm::quat(
                data["rotation"][3].asFloat(),
                data["rotation"][0].asFloat(),
                data["rotation"][1].asFloat(),
                data["rotation"][2].asFloat());
        }
        if (data.isMember("scale") && data["scale"].isArray() && data["scale"].size() == 3) {
            t.scale = glm::vec3(
                data["scale"][0].asFloat(),
                data["scale"][1].asFloat(),
                data["scale"][2].asFloat());
        }
        t.markDirty();
    }
    else if (name == "NameComponent") {
        auto& n = world.addComponentArchetype<ecs::NameComponent>(entity);
        if (data.isMember("name")) {
            n.setName(data["name"].asString());
        }
    }
    else if (name == "ModelComponent") {
        auto& mc = world.addComponentArchetype<ecs::ModelComponent>(entity);
        if (data.isMember("modelPath"))   mc.setModelPath(data["modelPath"].asString());
        if (data.isMember("visible"))     mc.visible = data["visible"].asBool();
        if (data.isMember("castShadow"))  mc.castShadow = data["castShadow"].asBool();
        if (data.isMember("receiveShadow")) mc.receiveShadow = data["receiveShadow"].asBool();
        if (data.isMember("albedoOverride") && data["albedoOverride"].isArray() && data["albedoOverride"].size() == 3) {
            mc.albedoOverride = glm::vec3(
                data["albedoOverride"][0].asFloat(),
                data["albedoOverride"][1].asFloat(),
                data["albedoOverride"][2].asFloat());
        }
        if (data.isMember("metallicOverride"))  mc.metallicOverride = data["metallicOverride"].asFloat();
        if (data.isMember("roughnessOverride")) mc.roughnessOverride = data["roughnessOverride"].asFloat();
        if (data.isMember("useLOD"))            mc.useLOD = data["useLOD"].asBool();
        if (data.isMember("lodBias"))           mc.lodBias = data["lodBias"].asFloat();
        if (data.isMember("windSway"))          mc.windSway = data["windSway"].asFloat();
    }
    else if (name == "LightComponent") {
        auto& lc = world.addComponentArchetype<ecs::LightComponent>(entity);
        if (data.isMember("type"))         lc.type = static_cast<ecs::LightType>(data["type"].asInt());
        if (data.isMember("color") && data["color"].isArray() && data["color"].size() == 3)
            lc.color = glm::vec3(data["color"][0].asFloat(), data["color"][1].asFloat(), data["color"][2].asFloat());
        if (data.isMember("intensity"))    lc.intensity = data["intensity"].asFloat();
        if (data.isMember("temperature"))  lc.temperature = data["temperature"].asFloat();
        if (data.isMember("range"))        lc.range = data["range"].asFloat();
        if (data.isMember("constant"))     lc.constant = data["constant"].asFloat();
        if (data.isMember("linear"))       lc.linear = data["linear"].asFloat();
        if (data.isMember("quadratic"))    lc.quadratic = data["quadratic"].asFloat();
        if (data.isMember("spotInnerAngle")) lc.spotInnerAngle = data["spotInnerAngle"].asFloat();
        if (data.isMember("spotOuterAngle")) lc.spotOuterAngle = data["spotOuterAngle"].asFloat();
        if (data.isMember("castShadows"))  lc.castShadows = data["castShadows"].asBool();
        if (data.isMember("shadowMapSize")) lc.shadowMapSize = data["shadowMapSize"].asInt();
        if (data.isMember("shadowBias"))   lc.shadowBias = data["shadowBias"].asFloat();
        if (data.isMember("shadowNear"))   lc.shadowNear = data["shadowNear"].asFloat();
        if (data.isMember("shadowFar"))    lc.shadowFar = data["shadowFar"].asFloat();
        if (data.isMember("enabled"))      lc.enabled = data["enabled"].asBool();
        if (data.isMember("useTemperature")) lc.useTemperature = data["useTemperature"].asBool();
    }
    else if (name == "MeshComponent") {
        auto& m = world.addComponentArchetype<ecs::MeshComponent>(entity);
        if (data.isMember("meshID"))     m.meshID = data["meshID"].asInt();
        if (data.isMember("meshType"))   m.meshType = static_cast<ecs::MeshType>(data["meshType"].asInt());
        if (data.isMember("visible"))    m.visible = data["visible"].asBool();
        if (data.isMember("castShadow")) m.castShadow = data["castShadow"].asBool();
        if (data.isMember("receiveShadow")) m.receiveShadow = data["receiveShadow"].asBool();
        if (data.isMember("color") && data["color"].isArray() && data["color"].size() == 3)
            m.color = glm::vec3(data["color"][0].asFloat(), data["color"][1].asFloat(), data["color"][2].asFloat());
        if (data.isMember("metallic"))   m.metallic = data["metallic"].asFloat();
        if (data.isMember("roughness"))  m.roughness = data["roughness"].asFloat();
        if (data.isMember("alpha"))      m.alpha = data["alpha"].asFloat();
        if (data.isMember("useInstancing")) m.useInstancing = data["useInstancing"].asBool();
        if (data.isMember("instanceCount")) m.instanceCount = data["instanceCount"].asInt();
    }
    else if (name == "TagComponent") {
        auto& tag = world.addComponentArchetype<ecs::TagComponent>(entity);
        if (data.isMember("tag")) {
            tag.setTag(data["tag"].asString());
        }
    }
}

} // anonymous namespace

// ── instantiate ───────────────────────────────────────────────────────────────

ecs::BlueprintInstance BlueprintSystem::instantiate(
    const std::string& name,
    const glm::vec3&   position,
    const glm::quat&   rotation,
    const glm::vec3&   scale) {

    ecs::BlueprintInstance instance;
    instance.blueprint = m_world.getBlueprint(name);
    instance.instanceName = name;
    instance.position = position;
    instance.rotation = rotation;
    instance.scale = scale;

    if (!instance.blueprint) {
        return instance;
    }

    const auto& entities = instance.blueprint->getEntities();
    instance.entities.reserve(entities.size());

    // Track created entities by blueprint index for parent-child wiring.
    std::vector<ecs::Entity> created;
    created.reserve(entities.size());

    for (size_t i = 0; i < entities.size(); ++i) {
        const ecs::BlueprintEntity& bpEntity = entities[i];
        ecs::Entity entity = m_world.createEntity();

        // Apply each component override in order.
        for (const ecs::ComponentOverride& co : bpEntity.componentOverrides) {
            applyOverride(m_world, entity, co);
        }

        // Apply the spawn transform offset on top of the blueprint's baked
        // transform (position += offset, rotation = rotation * baked,
        // scale *= scale).
        if (auto* t = m_world.getComponentArchetype<ecs::TransformComponent>(entity)) {
            t->position += position;
            t->rotation = rotation * t->rotation;
            t->scale    *= scale;
            t->markDirty();
        }

        created.push_back(entity);
        instance.entities.push_back(entity.id);
    }

    // Re-establish parent-child links.
    for (size_t i = 0; i < entities.size(); ++i) {
        if (entities[i].parentIndex != ecs::INVALID_ENTITY_ID &&
            entities[i].parentIndex < created.size()) {
            m_world.setParent(created[i], created[entities[i].parentIndex]);
        }
    }

    return instance;
}

// ── createBlueprintFromEntity ─────────────────────────────────────────────────

namespace {

/// Append a ComponentOverride for TransformComponent to bpEntity.
void captureTransform(const ecs::TransformComponent& t, ecs::BlueprintEntity& bpEntity) {
    ecs::ComponentOverride co;
    co.typeID = ecs::getComponentTypeID<ecs::TransformComponent>();
    co.componentName = "TransformComponent";
    co.data["position"].append(t.position.x);
    co.data["position"].append(t.position.y);
    co.data["position"].append(t.position.z);
    co.data["rotation"].append(t.rotation.x);
    co.data["rotation"].append(t.rotation.y);
    co.data["rotation"].append(t.rotation.z);
    co.data["rotation"].append(t.rotation.w);
    co.data["scale"].append(t.scale.x);
    co.data["scale"].append(t.scale.y);
    co.data["scale"].append(t.scale.z);
    bpEntity.componentOverrides.push_back(std::move(co));
}

/// Append a ComponentOverride for NameComponent to bpEntity.
void captureName(const ecs::NameComponent& n, ecs::BlueprintEntity& bpEntity) {
    ecs::ComponentOverride co;
    co.typeID = ecs::getComponentTypeID<ecs::NameComponent>();
    co.componentName = "NameComponent";
    co.data["name"] = n.getName();
    bpEntity.componentOverrides.push_back(std::move(co));
}

/// Append a ComponentOverride for ModelComponent to bpEntity.
void captureModel(const ecs::ModelComponent& mc, ecs::BlueprintEntity& bpEntity) {
    ecs::ComponentOverride co;
    co.typeID = ecs::getComponentTypeID<ecs::ModelComponent>();
    co.componentName = "ModelComponent";
    co.data["modelPath"] = mc.getModelPath();
    co.data["visible"] = mc.visible;
    co.data["castShadow"] = mc.castShadow;
    co.data["receiveShadow"] = mc.receiveShadow;
    co.data["albedoOverride"].append(mc.albedoOverride.x);
    co.data["albedoOverride"].append(mc.albedoOverride.y);
    co.data["albedoOverride"].append(mc.albedoOverride.z);
    co.data["metallicOverride"] = mc.metallicOverride;
    co.data["roughnessOverride"] = mc.roughnessOverride;
    co.data["useLOD"] = mc.useLOD;
    co.data["lodBias"] = mc.lodBias;
    co.data["windSway"] = mc.windSway;
    bpEntity.componentOverrides.push_back(std::move(co));
}

/// Append a ComponentOverride for LightComponent to bpEntity.
void captureLight(const ecs::LightComponent& lc, ecs::BlueprintEntity& bpEntity) {
    ecs::ComponentOverride co;
    co.typeID = ecs::getComponentTypeID<ecs::LightComponent>();
    co.componentName = "LightComponent";
    co.data["type"] = static_cast<int>(lc.type);
    co.data["color"].append(lc.color.r);
    co.data["color"].append(lc.color.g);
    co.data["color"].append(lc.color.b);
    co.data["intensity"] = lc.intensity;
    co.data["temperature"] = lc.temperature;
    co.data["range"] = lc.range;
    co.data["constant"] = lc.constant;
    co.data["linear"] = lc.linear;
    co.data["quadratic"] = lc.quadratic;
    co.data["spotInnerAngle"] = lc.spotInnerAngle;
    co.data["spotOuterAngle"] = lc.spotOuterAngle;
    co.data["castShadows"] = lc.castShadows;
    co.data["shadowMapSize"] = lc.shadowMapSize;
    co.data["shadowBias"] = lc.shadowBias;
    co.data["shadowNear"] = lc.shadowNear;
    co.data["shadowFar"] = lc.shadowFar;
    co.data["enabled"] = lc.enabled;
    co.data["useTemperature"] = lc.useTemperature;
    bpEntity.componentOverrides.push_back(std::move(co));
}

/// Append a ComponentOverride for MeshComponent to bpEntity.
void captureMesh(const ecs::MeshComponent& m, ecs::BlueprintEntity& bpEntity) {
    ecs::ComponentOverride co;
    co.typeID = ecs::getComponentTypeID<ecs::MeshComponent>();
    co.componentName = "MeshComponent";
    co.data["meshID"] = m.meshID;
    co.data["meshType"] = static_cast<int>(m.meshType);
    co.data["visible"] = m.visible;
    co.data["castShadow"] = m.castShadow;
    co.data["receiveShadow"] = m.receiveShadow;
    co.data["color"].append(m.color.r);
    co.data["color"].append(m.color.g);
    co.data["color"].append(m.color.b);
    co.data["metallic"] = m.metallic;
    co.data["roughness"] = m.roughness;
    co.data["alpha"] = m.alpha;
    co.data["useInstancing"] = m.useInstancing;
    co.data["instanceCount"] = m.instanceCount;
    bpEntity.componentOverrides.push_back(std::move(co));
}

/// Append a ComponentOverride for TagComponent to bpEntity.
void captureTag(const ecs::TagComponent& tag, ecs::BlueprintEntity& bpEntity) {
    ecs::ComponentOverride co;
    co.typeID = ecs::getComponentTypeID<ecs::TagComponent>();
    co.componentName = "TagComponent";
    co.data["tag"] = tag.tag;
    bpEntity.componentOverrides.push_back(std::move(co));
}

} // anonymous namespace

std::unique_ptr<ecs::Blueprint> BlueprintSystem::createBlueprintFromEntity(
    ecs::EntityID rootEntity,
    const std::string& name) {

    if (!m_world.isAlive(rootEntity)) {
        return nullptr;
    }

    auto blueprint = std::make_unique<ecs::Blueprint>(name);

    // Recursively capture the entity hierarchy. Each child records its
    // parent's BlueprintEntity index in `parentIndex`.
    std::function<void(const ecs::Entity&, ecs::EntityID)> captureRec;
    captureRec = [&](const ecs::Entity& ent, ecs::EntityID parentIdx) {
        ecs::BlueprintEntity bpEntity;
        bpEntity.parentIndex = parentIdx;

        // Name — captured as both bpEntity.name (display) and a ComponentOverride
        // (so instantiate re-creates the NameComponent).
        if (auto* n = m_world.getComponentArchetype<ecs::NameComponent>(ent)) {
            bpEntity.name = std::string(n->getName());
            captureName(*n, bpEntity);
        }

        // Transform
        if (auto* t = m_world.getComponentArchetype<ecs::TransformComponent>(ent)) {
            captureTransform(*t, bpEntity);
        }

        // Model
        if (auto* mc = m_world.getComponentArchetype<ecs::ModelComponent>(ent)) {
            captureModel(*mc, bpEntity);
        }

        // Light
        if (auto* lc = m_world.getComponentArchetype<ecs::LightComponent>(ent)) {
            captureLight(*lc, bpEntity);
        }

        // Mesh
        if (auto* m = m_world.getComponentArchetype<ecs::MeshComponent>(ent)) {
            captureMesh(*m, bpEntity);
        }

        // Tag
        if (auto* tag = m_world.getComponentArchetype<ecs::TagComponent>(ent)) {
            captureTag(*tag, bpEntity);
        }

        // Fallback name if Entity had no NameComponent.
        if (bpEntity.name.empty()) {
            bpEntity.name = "Entity_" + std::to_string(ent.id);
        }

        ecs::EntityID myIndex = static_cast<ecs::EntityID>(blueprint->getEntities().size());
        blueprint->addEntity(bpEntity);

        // Recurse into children.
        auto children = m_world.getChildren(ent);
        for (const auto& child : children) {
            captureRec(child, myIndex);
        }
    };

    captureRec(ecs::Entity{rootEntity}, ecs::INVALID_ENTITY_ID);
    return blueprint;
}

} // namespace Editor
