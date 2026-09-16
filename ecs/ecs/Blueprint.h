#pragma once

#include "Entity.h"
#include "Component.h"
#include "Serialization.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <jsoncpp/json/json.h>
#include <fstream>
#include <sstream>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ecs {

/**
 * Blueprint Component Override - Allows overriding specific component values
 */
struct ComponentOverride {
    ComponentTypeID typeID;
    std::string componentName;
    Json::Value data;
    
    /**
     * Check if this override applies to a specific component type
     */
    bool isForType(ComponentTypeID id) const { return typeID == id; }
};

/**
 * Blueprint Entity - Defines an entity template within a blueprint
 */
struct BlueprintEntity {
    std::string name;
    std::string tag = "";
    EntityID parentIndex = INVALID_ENTITY_ID;  // Index in blueprint's entity list
    std::vector<ComponentOverride> componentOverrides;
    
    /**
     * Add a component override
     */
    template<typename T>
    void addOverride(const std::string& name, const Json::Value& value) {
        ComponentOverride override;
        override.typeID = getComponentTypeID<T>();
        override.componentName = name;
        override.data = value;
        componentOverrides.push_back(std::move(override));
    }
    
    /**
     * Check if this entity has an override for a component type
     */
    bool hasOverride(ComponentTypeID typeID) const {
        for (const auto& override : componentOverrides) {
            if (override.typeID == typeID) return true;
        }
        return false;
    }
    
    /**
     * Get an override for a component type
     */
    const ComponentOverride* getOverride(ComponentTypeID typeID) const {
        for (const auto& override : componentOverrides) {
            if (override.typeID == typeID) return &override;
        }
        return nullptr;
    }
};

/**
 * Blueprint - Reusable entity/prefab template
 * 
 * Blueprints define entity templates that can be instantiated multiple times
 * with optional overrides. Similar to Unity Prefabs or Unreal Blueprints.
 */
class Blueprint {
public:
    Blueprint() = default;
    explicit Blueprint(const std::string& name) : m_name(name) {}
    ~Blueprint() = default;

    // Prevent copying
    Blueprint(const Blueprint&) = delete;
    Blueprint& operator=(const Blueprint&) = delete;
    
    // Allow moving
    Blueprint(Blueprint&&) = default;
    Blueprint& operator=(Blueprint&&) = default;

    /**
     * Get the blueprint name
     */
    const std::string& getName() const { return m_name; }

    /**
     * Set the blueprint name
     */
    void setName(const std::string& name) { m_name = name; }

    /**
     * Add an entity definition to the blueprint
     */
    void addEntity(const BlueprintEntity& entity) {
        m_entities.push_back(entity);
    }

    /**
     * Get all entity definitions
     */
    const std::vector<BlueprintEntity>& getEntities() const { return m_entities; }

    /**
     * Get the number of entities in this blueprint
     */
    size_t getEntityCount() const { return m_entities.size(); }

    /**
     * Clear all entity definitions
     */
    void clear() { m_entities.clear(); }

    /**
     * Serialize the blueprint to JSON
     */
    Json::Value serialize() const {
        Json::Value root;
        root["name"] = m_name;
        root["version"] = m_version;
        
        Json::Value entitiesJson;
        for (const auto& entity : m_entities) {
            Json::Value entityJson;
            entityJson["name"] = entity.name;
            entityJson["tag"] = entity.tag;
            entityJson["parentIndex"] = entity.parentIndex;
            
            Json::Value overridesJson;
            for (const auto& override : entity.componentOverrides) {
                Json::Value overrideJson;
                overrideJson["typeID"] = static_cast<Json::UInt64>(override.typeID);
                overrideJson["componentName"] = override.componentName;
                overrideJson["data"] = override.data;
                overridesJson.append(overrideJson);
            }
            entityJson["overrides"] = overridesJson;
            
            entitiesJson.append(entityJson);
        }
        
        root["entities"] = entitiesJson;
        return root;
    }

    /**
     * Deserialize the blueprint from JSON
     */
    void deserialize(const Json::Value& json) {
        m_name = json["name"].asString();
        m_version = json["version"].asUInt();
        
        m_entities.clear();
        const Json::Value& entitiesJson = json["entities"];
        
        for (const auto& entityJson : entitiesJson) {
            BlueprintEntity entity;
            entity.name = entityJson["name"].asString();
            entity.tag = entityJson["tag"].asString();
            entity.parentIndex = entityJson["parentIndex"].asUInt();
            
            const Json::Value& overridesJson = entityJson["overrides"];
            for (const auto& overrideJson : overridesJson) {
                ComponentOverride override;
                override.typeID = static_cast<ComponentTypeID>(overrideJson["typeID"].asUInt64());
                override.componentName = overrideJson["componentName"].asString();
                override.data = overrideJson["data"];
                entity.componentOverrides.push_back(std::move(override));
            }
            
            m_entities.push_back(std::move(entity));
        }
    }

    /**
     * Save the blueprint to a file
     */
    bool saveToFile(const std::string& filename) const {
        Json::Value root = serialize();
        Json::StreamWriterBuilder builder;
        builder["commentStyle"] = "None";
        builder["indentation"] = "  ";
        
        std::ofstream file(filename);
        if (!file.is_open()) return false;
        
        file << Json::writeString(builder, root);
        file.close();
        return true;
    }

    /**
     * Load the blueprint from a file
     */
    static std::unique_ptr<Blueprint> loadFromFile(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) return nullptr;
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        file.close();
        
        Json::CharReaderBuilder reader;
        Json::Value root;
        std::string errors;
        
        if (!Json::parseFromStream(reader, buffer, &root, &errors)) {
            return nullptr;
        }
        
        auto blueprint = std::make_unique<Blueprint>();
        blueprint->deserialize(root);
        return blueprint;
    }

private:
    std::string m_name;
    uint32_t m_version = 1;
    std::vector<BlueprintEntity> m_entities;
};

/**
 * Blueprint Instance - An instantiated blueprint with tracking
 */
struct BlueprintInstance {
    Blueprint* blueprint;
    std::vector<EntityID> entities;  // Created entity IDs
    std::string instanceName;
    glm::vec3 position = glm::vec3(0);
    glm::quat rotation = glm::quat(1, 0, 0, 0);
    glm::vec3 scale = glm::vec3(1);
    
    /**
     * Check if this instance is valid
     */
    bool isValid() const { return blueprint != nullptr && !entities.empty(); }
    
    /**
     * Get the root entity
     */
    EntityID getRootEntity() const {
        return entities.empty() ? INVALID_ENTITY_ID : entities[0];
    }
};

/**
 * Blueprint Manager - Manages blueprint registration and instantiation
 */
template<typename World, typename ComponentManager>
class BlueprintManager {
public:
    BlueprintManager(World& world, ComponentManager& compManager)
        : m_world(world)
        , m_compManager(compManager)
    {
    }

    /**
     * Register a blueprint
     */
    void registerBlueprint(std::unique_ptr<Blueprint> blueprint) {
        if (!blueprint) return;
        m_blueprints[blueprint->getName()] = std::move(blueprint);
    }

    /**
     * Unregister a blueprint
     */
    void unregisterBlueprint(const std::string& name) {
        m_blueprints.erase(name);
    }

    /**
     * Get a blueprint by name
     */
    Blueprint* getBlueprint(const std::string& name) {
        auto it = m_blueprints.find(name);
        return it != m_blueprints.end() ? it->second.get() : nullptr;
    }

    const Blueprint* getBlueprint(const std::string& name) const {
        auto it = m_blueprints.find(name);
        return it != m_blueprints.end() ? it->second.get() : nullptr;
    }

    /**
     * Check if a blueprint exists
     */
    bool hasBlueprint(const std::string& name) const {
        return m_blueprints.find(name) != m_blueprints.end();
    }

    /**
     * Instantiate a blueprint at a position
     * @param name Blueprint name
     * @param position World position
     * @param rotation World rotation
     * @param scale Scale
     * @return BlueprintInstance with created entities
     */
    BlueprintInstance instantiate(const std::string& name,
                                   const glm::vec3& position = glm::vec3(0),
                                   const glm::quat& rotation = glm::quat(1, 0, 0, 0),
                                   const glm::vec3& scale = glm::vec3(1)) {
        BlueprintInstance instance;
        instance.blueprint = getBlueprint(name);
        instance.instanceName = name;
        instance.position = position;
        instance.rotation = rotation;
        instance.scale = scale;
        
        if (!instance.blueprint) {
            return instance;
        }
        
        // Create entities from blueprint
        const auto& entities = instance.blueprint->getEntities();
        instance.entities.reserve(entities.size());
        
        for (const auto& bpEntity : entities) {
            EntityID entityID = createEntityFromBlueprint(bpEntity, instance);
            instance.entities.push_back(entityID);
        }
        
        m_instances.push_back(instance);
        return instance;
    }

    /**
     * Destroy a blueprint instance
     */
    void destroyInstance(BlueprintInstance& instance) {
        for (EntityID entityID : instance.entities) {
            m_world.destroyEntity(Entity{entityID});
        }
        instance.entities.clear();
        instance.blueprint = nullptr;
        
        // Remove from instances list
        m_instances.erase(
            std::remove_if(m_instances.begin(), m_instances.end(),
                [&instance](const BlueprintInstance& inst) {
                    return &inst == &instance;
                }),
            m_instances.end()
        );
    }

    /**
     * Destroy all instances of a blueprint
     */
    void destroyAllInstances(const std::string& blueprintName) {
        for (auto& instance : m_instances) {
            if (instance.blueprint && instance.blueprint->getName() == blueprintName) {
                destroyInstance(instance);
            }
        }
    }

    /**
     * Get all instances
     */
    const std::vector<BlueprintInstance>& getInstances() const {
        return m_instances;
    }

    /**
     * Get the number of registered blueprints
     */
    size_t getBlueprintCount() const {
        return m_blueprints.size();
    }

    /**
     * Get the number of active instances
     */
    size_t getInstanceCount() const {
        return m_instances.size();
    }

    /**
     * Create a blueprint from an existing entity hierarchy
     */
    std::unique_ptr<Blueprint> createBlueprintFromEntity(
        EntityID rootEntity,
        const std::string& name) {
        
        auto blueprint = std::make_unique<Blueprint>(name);
        // Implementation would traverse entity hierarchy and capture components
        return blueprint;
    }

private:
    EntityID createEntityFromBlueprint(const BlueprintEntity& bpEntity,
                                        const BlueprintInstance& instance) {
        Entity entity = m_world.createEntity();
        
        // Apply component overrides
        for (const auto& override : bpEntity.componentOverrides) {
            // Apply override data to entity's component
            // This would use the serializer registry to deserialize component data
        }
        
        // Apply transform
        // Would need TransformComponent to set position/rotation/scale
        
        return entity.id;
    }

    World& m_world;
    ComponentManager& m_compManager;
    std::unordered_map<std::string, std::unique_ptr<Blueprint>> m_blueprints;
    std::vector<BlueprintInstance> m_instances;
};

/**
 * Blueprint Builder - Fluent interface for creating blueprints
 */
class BlueprintBuilder {
public:
    explicit BlueprintBuilder(const std::string& name)
        : m_blueprint(std::make_unique<Blueprint>(name))
    {
    }

    /**
     * Add an entity to the blueprint
     */
    BlueprintBuilder& addEntity(const std::string& name, const std::string& tag = "") {
        BlueprintEntity entity;
        entity.name = name;
        entity.tag = tag;
        m_currentEntityIndex = m_blueprint->getEntities().size();
        m_blueprint->addEntity(entity);
        return *this;
    }

    /**
     * Set the parent of the current entity
     */
    BlueprintBuilder& setParent(size_t parentIndex) {
        if (m_currentEntityIndex < m_blueprint->getEntities().size()) {
            auto& entities = const_cast<std::vector<BlueprintEntity>&>(
                m_blueprint->getEntities());
            entities[m_currentEntityIndex].parentIndex = parentIndex;
        }
        return *this;
    }

    /**
     * Add a component override to the current entity
     */
    template<typename T>
    BlueprintBuilder& addComponentOverride(const std::string& name, const Json::Value& value) {
        if (m_currentEntityIndex < m_blueprint->getEntities().size()) {
            auto& entities = const_cast<std::vector<BlueprintEntity>&>(
                m_blueprint->getEntities());
            entities[m_currentEntityIndex].addOverride<T>(name, value);
        }
        return *this;
    }

    /**
     * Build the blueprint
     */
    std::unique_ptr<Blueprint> build() {
        return std::move(m_blueprint);
    }

private:
    std::unique_ptr<Blueprint> m_blueprint;
    size_t m_currentEntityIndex = 0;
};

} // namespace ecs
