#pragma once

#include "Entity.h"
#include "Component.h"
#include "ComponentManager.h"
#include "ArchetypeManager.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <memory>
#include <functional>
#include <typeindex>
#include <jsoncpp/json/json.h>

namespace ecs {

/**
 * Serialization Context - Holds state during serialization
 */
struct SerializeContext {
    std::unordered_map<EntityID, std::string> entityNames;
    std::unordered_map<std::string, EntityID> entityLookup;
    uint32_t version = 1;
    std::string engineVersion;
    
    SerializeContext() : engineVersion("1.0.0") {}
};

/**
 * Deserialization Context - Holds state during deserialization
 */
struct DeserializeContext : public SerializeContext {
    // Entity-ID translation table: maps STALE serialized EntityIDs to the FRESH
    // live EntityIDs handed out by EntityManager::createEntity during load.
    std::unordered_map<EntityID, EntityID> idMap;
    std::vector<std::function<void()>> postLoadCallbacks;
    
    void addPostLoadCallback(std::function<void()> callback) {
        postLoadCallbacks.push_back(std::move(callback));
    }
    
    void executePostLoadCallbacks() {
        for (auto& callback : postLoadCallbacks) {
            callback();
        }
        postLoadCallbacks.clear();
    }
};

/**
 * Component Serializer - Interface for serializing individual component types
 */
class IComponentSerializer {
public:
    virtual ~IComponentSerializer() = default;
    
    /**
     * Serialize a component to JSON
     */
    virtual Json::Value serialize(const void* component) const = 0;
    
    /**
     * Deserialize a component from JSON
     */
    virtual void* deserialize(const Json::Value& json) const = 0;
    
    /**
     * Get the component type ID this serializer handles
     */
    virtual ComponentTypeID getComponentTypeID() const = 0;
    
    /**
     * Get the component type name
     */
    virtual const char* getComponentTypeName() const = 0;
    
    /**
     * Destroy a deserialized component (cleanup)
     */
    virtual void destroy(void* component) const = 0;
    /**
     * Type-erased component access (bridge for serialize/deserialize loops).
     * `compManager` is the concrete ecs::ComponentManager* and `archManager`
     * is ecs::ArchetypeManager<>* (may be nullptr). hasComponentIn /
     * getComponentPtr check BOTH stores (CM-first, AM-fallback) so entities
     * created via addComponent (ComponentManager) OR createEntityWithComponents
     * (ArchetypeManager) round-trip. insertComponent writes to the
     * ComponentManager sink; read-back stays unified via World::getComponent.
     */
    virtual bool hasComponentIn(void* compManager, void* archManager, EntityID entityID) const = 0;
    virtual void* getComponentPtr(void* compManager, void* archManager, EntityID entityID) const = 0;
    virtual void insertComponent(void* compManager, EntityID entityID, const Json::Value& json) const = 0;

    /**
     * Entity-ID translation engine. After deserialization into fresh live ids,
     * components that reference OTHER entities by id (ParentComponent::parent,
     * ChildrenComponent::children, ...) hold STALE serialized ids. The concrete
     * serializer walks its own entity-reference fields and rewrites them through
     * the (serializedID -> liveID) map. Default no-op so ref-less types are
     * unaffected; types without a registered remapFunc are a no-op too.
     */
    virtual void remapReferences(void* /*component*/,
                                 const std::unordered_map<EntityID, EntityID>& /*idMap*/) const {}
};

/**
 * Typed Component Serializer - Implementation for specific component types
 */
template<typename ComponentType>
class ComponentSerializer : public IComponentSerializer {
public:
    using SerializeFunc = Json::Value (*)(const ComponentType&);
    using DeserializeFunc = ComponentType (*)(const Json::Value&);
    using RemapFunc = std::function<void(void*, const std::unordered_map<EntityID, EntityID>&)>;

    ComponentSerializer(SerializeFunc serializeFunc, DeserializeFunc deserializeFunc,
                        RemapFunc remapFunc = nullptr)
        : m_serializeFunc(serializeFunc)
        , m_deserializeFunc(deserializeFunc)
        , m_remapFunc(remapFunc)
    {
    }

    ComponentTypeID getComponentTypeID() const override {
        return ecs::getComponentTypeID<ComponentType>();
    }

    const char* getComponentTypeName() const override {
        return ecs::getComponentTypeName<ComponentType>();
    }
    
    Json::Value serialize(const void* component) const override {
        return m_serializeFunc(*static_cast<const ComponentType*>(component));
    }
    
    void* deserialize(const Json::Value& json) const override {
        ComponentType* comp = new ComponentType();
        *comp = m_deserializeFunc(json);
        return comp;
    }
    
    void destroy(void* component) const override {
        delete static_cast<ComponentType*>(component);
    }
    bool hasComponentIn(void* compManager, void* archManager, EntityID entityID) const override {
        if (auto* cm = static_cast<ComponentManager*>(compManager)) {
            if (cm->hasComponent<ComponentType>(entityID)) return true;
        }
        if (auto* am = static_cast<ArchetypeManager<>*>(archManager)) {
            if (am->hasComponent<ComponentType>(entityID)) return true;
        }
        return false;
    }
    void* getComponentPtr(void* compManager, void* archManager, EntityID entityID) const override {
        if (auto* cm = static_cast<ComponentManager*>(compManager)) {
            if (auto* p = cm->getComponent<ComponentType>(entityID))
                return static_cast<void*>(p);
        }
        if (auto* am = static_cast<ArchetypeManager<>*>(archManager)) {
            if (auto* p = am->getComponent<ComponentType>(entityID))
                return static_cast<void*>(p);
        }
        return nullptr;
    }
    void insertComponent(void* compManager, EntityID entityID, const Json::Value& json) const override {
        auto* cm = static_cast<ComponentManager*>(compManager);
        cm->addComponent<ComponentType>(entityID, m_deserializeFunc(json));
    }
    void remapReferences(void* component,
                         const std::unordered_map<EntityID, EntityID>& idMap) const override {
        if (m_remapFunc) m_remapFunc(component, idMap);
    }

private:
    SerializeFunc m_serializeFunc;
    DeserializeFunc m_deserializeFunc;
    RemapFunc m_remapFunc;
};

/**
 * Serializer Registry - Manages all component serializers
 */
class SerializerRegistry {
public:
    static SerializerRegistry& getInstance() {
        static SerializerRegistry instance;
        return instance;
    }
    
    /**
     * Register a component serializer
     */
    template<typename ComponentType>
    void registerSerializer(
        typename ComponentSerializer<ComponentType>::SerializeFunc serializeFunc,
        typename ComponentSerializer<ComponentType>::DeserializeFunc deserializeFunc,
        typename ComponentSerializer<ComponentType>::RemapFunc remapFunc = nullptr) 
    {
        ComponentTypeID typeID = getComponentTypeID<ComponentType>();
        
        auto serializer = std::make_unique<ComponentSerializer<ComponentType>>(
            serializeFunc, deserializeFunc, remapFunc);
        
        m_serializers[typeID] = std::move(serializer);
    }
    
    /**
     * Get a serializer by component type ID
     */
    const IComponentSerializer* getSerializer(ComponentTypeID typeID) const {
        auto it = m_serializers.find(typeID);
        return it != m_serializers.end() ? it->second.get() : nullptr;
    }
    
    /**
     * Check if a serializer is registered
     */
    bool hasSerializer(ComponentTypeID typeID) const {
        return m_serializers.find(typeID) != m_serializers.end();
    }
    
    /**
     * Get all registered serializer type IDs
     */
    std::vector<ComponentTypeID> getRegisteredTypeIDs() const {
        std::vector<ComponentTypeID> ids;
        ids.reserve(m_serializers.size());
        for (const auto& pair : m_serializers) {
            ids.push_back(pair.first);
        }
        return ids;
    }
    
private:
    SerializerRegistry() = default;
    std::unordered_map<ComponentTypeID, std::unique_ptr<IComponentSerializer>> m_serializers;
};

/**
 * Entity Serializer - Serializes individual entities and their components
 */
class EntitySerializer {
public:
    /**
     * Serialize an entity to JSON
     */
    template<typename ComponentManager>
    static Json::Value serializeEntity(
        EntityID entityID,
        ComponentManager& compManager,
        ArchetypeManager<>* archManager,
        const SerializeContext& context) 
    {
        Json::Value entityJson;
        
        entityJson["id"] = static_cast<Json::UInt64>(entityID);
        
        // Add entity name if available
        auto nameIt = context.entityNames.find(entityID);
        if (nameIt != context.entityNames.end()) {
            entityJson["name"] = nameIt->second;
        }
        
        // Serialize all components of this entity. Iterate the serializer
        // REGISTRY (all registered component types) and dual-check BOTH stores
        // (ComponentManager-first, ArchetypeManager-fallback) via the type-erased
        // bridge, so entities created via addComponent (ComponentManager) OR
        // createEntityWithComponents (ArchetypeManager) round-trip.
        Json::Value componentsJson;
        for (ComponentTypeID typeID : SerializerRegistry::getInstance().getRegisteredTypeIDs()) {
            const IComponentSerializer* serializer =
                SerializerRegistry::getInstance().getSerializer(typeID);
            if (!serializer) { continue; }
            if (serializer->hasComponentIn(&compManager, archManager, entityID)) {
                void* compPtr = serializer->getComponentPtr(&compManager, archManager, entityID);
                componentsJson[serializer->getComponentTypeName()] =
                    serializer->serialize(compPtr);
            }
        }
        entityJson["components"] = componentsJson;
        return entityJson;
    }
    
    /**
     * Deserialize an entity from JSON
     */
    template<typename ComponentManager>
    static void deserializeEntity(
        const Json::Value& entityJson,
        EntityID liveEntityID,
        ComponentManager& compManager,
        DeserializeContext& context)
    {
        // Components attach to the LIVE entity id (created by WorldSerializer::
        // deserialize), not the stale serialized id. Name mapping uses the
        // live id too.
        if (entityJson.isMember("name")) {
            context.entityNames[liveEntityID] = entityJson["name"].asString();
            context.entityLookup[entityJson["name"].asString()] = liveEntityID;
        }

        // Deserialize components. JSON is name-keyed; the registry is typeID-keyed,
        // so match by component type name.
        if (entityJson.isMember("components")) {
            const Json::Value& componentsJson = entityJson["components"];
            for (const auto& compName : componentsJson.getMemberNames()) {
                const Json::Value& compJson = componentsJson[compName];
                for (ComponentTypeID typeID : SerializerRegistry::getInstance().getRegisteredTypeIDs()) {
                    const IComponentSerializer* serializer =
                        SerializerRegistry::getInstance().getSerializer(typeID);
                    if (!serializer) { continue; }
                    if (compName == serializer->getComponentTypeName()) {
                        serializer->insertComponent(&compManager, liveEntityID, compJson);
                        break;
                    }
                }
            }
        }
    }
};

/**
 * World Serializer - Serializes entire ECS worlds
 */
template<typename ComponentManager, typename EntityManager>
class WorldSerializer {
public:
    WorldSerializer(ComponentManager& compManager, EntityManager& entManager,
                    ArchetypeManager<>* archManager = nullptr)
        : m_compManager(compManager)
        , m_entManager(entManager)
        , m_archManager(archManager)
    {
    }
    
    /**
     * Serialize the entire world to a JSON string
     */
    std::string serializeToString(const SerializeContext& context = SerializeContext()) {
        Json::Value root = serialize(context);
        Json::StreamWriterBuilder builder;
        builder["commentStyle"] = "None";
        builder["indentation"] = "  ";
        return Json::writeString(builder, root);
    }
    
    /**
     * Serialize the entire world to a JSON value
     */
    Json::Value serialize(const SerializeContext& context = SerializeContext()) {
        Json::Value root;
        
        // Header
        root["version"] = context.version;
        root["engineVersion"] = context.engineVersion;
        root["entityCount"] = static_cast<Json::UInt64>(m_entManager.getLiveEntityCount());
        
        // Serialize all entities
        Json::Value entitiesJson;
        for (EntityID entityID : m_entManager.getLiveEntities()) {
            Json::Value entityJson = EntitySerializer::serializeEntity(
                entityID, m_compManager, m_archManager, context);
            entitiesJson.append(entityJson);
        }
        
        root["entities"] = entitiesJson;
        return root;
    }
    
    /**
     * Deserialize the world from a JSON string
     */
    void deserializeFromString(const std::string& jsonString, 
                                DeserializeContext& context = DeserializeContext()) {
        Json::CharReaderBuilder reader;
        std::istringstream iss(jsonString);
        Json::Value root;
        std::string errors;
        
        if (!Json::parseFromStream(reader, iss, &root, &errors)) {
            throw std::runtime_error("Failed to parse JSON: " + errors);
        }
        
        deserialize(root, context);
    }
    
    /**
     * Deserialize the world from a JSON value
     */
    void deserialize(const Json::Value& root, DeserializeContext& context = DeserializeContext()) {
        // Verify version
        uint32_t version = root["version"].asUInt();
        if (version != context.version) {
            // Handle version migration if needed
        }
        
        // Deserialize all entities. Create a fresh LIVE entity per entry and
        // hand its id to deserializeEntity so components attach to the live
        // entity (not the stale serialized id). Serialized ids are NOT re-mapped
        // onto existing entities: createEntity() hands out fresh ids from the
        // available pool, so isAlive(serializedID) could collide with a just-
        // created entity and skip round-trip entries.
        const Json::Value& entitiesJson = root["entities"];
        for (const auto& entityJson : entitiesJson) {
            Entity liveEntity = m_entManager.createEntity();
            EntityID serializedID = static_cast<EntityID>(entityJson["id"].asUInt64());
            // Translation table: stale serialized id -> fresh live id.
            context.idMap[serializedID] = liveEntity.id;
            EntitySerializer::deserializeEntity(
                entityJson, liveEntity.id, m_compManager, context);
        }

        // Entity-ID translation pass: rewrite entity-reference fields (Parent/Children,
        // ...) that point at STALE serialized ids to the fresh live ids above. Generic:
        // each registered serializer's RemapFunc walks its own ref fields (no-op for
        // types without entity refs, and no-op if no remapFunc was registered).
        const auto& typeIDs = SerializerRegistry::getInstance().getRegisteredTypeIDs();
        for (EntityID liveID : m_entManager.getLiveEntities()) {
            for (ComponentTypeID typeID : typeIDs) {
                const IComponentSerializer* ser =
                    SerializerRegistry::getInstance().getSerializer(typeID);
                if (!ser) continue;
                if (void* comp = ser->getComponentPtr(&m_compManager, m_archManager, liveID)) {
                    ser->remapReferences(comp, context.idMap);
                }
            }
        }

        // Execute post-load callbacks
        context.executePostLoadCallbacks();
    }
    
    /**
     * Save the world to a file
     */
    bool saveToFile(const std::string& filename, const SerializeContext& context = SerializeContext()) {
        std::ofstream file(filename);
        if (!file.is_open()) {
            return false;
        }
        
        file << serializeToString(context);
        file.close();
        return true;
    }
    
    /**
     * Load the world from a file
     */
    bool loadFromFile(const std::string& filename, DeserializeContext& context = DeserializeContext()) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            return false;
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        file.close();
        
        deserializeFromString(buffer.str(), context);
        return true;
    }
    
private:
    ComponentManager& m_compManager;
    EntityManager& m_entManager;
    ArchetypeManager<>* m_archManager;
};

/**
 * Binary Serializer - For compact binary serialization
 */
class BinarySerializer {
public:
    /**
     * Write data to the buffer
     */
    template<typename T>
    void write(const T& value) {
        size_t offset = m_buffer.size();
        m_buffer.resize(offset + sizeof(T));
        *reinterpret_cast<T*>(m_buffer.data() + offset) = value;
    }
    
    /**
     * Write a string to the buffer
     */
    void writeString(const std::string& str) {
        write(static_cast<uint32_t>(str.size()));
        m_buffer.insert(m_buffer.end(), str.begin(), str.end());
    }
    
    /**
     * Read data from the buffer
     */
    template<typename T>
    T read() {
        if (m_offset + sizeof(T) > m_buffer.size()) {
            throw std::runtime_error("Buffer underflow");
        }
        
        T value = *reinterpret_cast<const T*>(m_buffer.data() + m_offset);
        m_offset += sizeof(T);
        return value;
    }
    
    /**
     * Read a string from the buffer
     */
    std::string readString() {
        uint32_t size = read<uint32_t>();
        std::string str(m_buffer.begin() + m_offset, m_buffer.begin() + m_offset + size);
        m_offset += size;
        return str;
    }
    
    /**
     * Get the buffer data
     */
    const std::vector<uint8_t>& getBuffer() const { return m_buffer; }
    
    /**
     * Set the buffer data for reading
     */
    void setBuffer(const std::vector<uint8_t>& buffer) {
        m_buffer = buffer;
        m_offset = 0;
    }
    
    /**
     * Reset the serializer
     */
    void reset() {
        m_buffer.clear();
        m_offset = 0;
    }
    
    /**
     * Get the current read offset
     */
    size_t getOffset() const { return m_offset; }
    
    /**
     * Get the buffer size
     */
    size_t size() const { return m_buffer.size(); }
    
private:
    std::vector<uint8_t> m_buffer;
    size_t m_offset = 0;
};

/**
 * Scene Format - Standard scene file format
 * 
 * File structure:
 * - Header: Magic number, version, flags
 * - Entity count
 * - Entity data (repeated)
 * - Component data (repeated)
 */
#pragma pack(push, 1)
struct SceneHeader {
    uint32_t magic = 0x53434E45;  // "SCNE"
    uint32_t version = 1;
    uint32_t flags = 0;
    uint64_t entityCount = 0;
    uint64_t componentCount = 0;
};
#pragma pack(pop)

} // namespace ecs
