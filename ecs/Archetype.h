#pragma once

#include "Entity.h"
#include "Component.h"
#include <vector>
#include <array>
#include <bitset>
#include <unordered_map>
#include <type_traits>
#include <cstring>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <typeinfo>

namespace ecs {

class Archetype {
public:
    using ComponentTypeList = std::vector<ComponentTypeID>;

    Archetype() = default;
    
    explicit Archetype(ComponentTypeList componentTypes) 
        : m_componentTypes(std::move(componentTypes)) 
    {
        std::sort(m_componentTypes.begin(), m_componentTypes.end());
        computeHash();
    }

    const ComponentTypeList& getComponentTypes() const { return m_componentTypes; }

    bool hasComponentType(ComponentTypeID typeID) const {
        return std::binary_search(m_componentTypes.begin(), m_componentTypes.end(), typeID);
    }

    size_t getHash() const { return m_hash; }

    bool canAddComponent(ComponentTypeID typeID) const {
        return !hasComponentType(typeID);
    }

    bool canRemoveComponent(ComponentTypeID typeID) const {
        return hasComponentType(typeID);
    }

    ComponentTypeList getWithAddedComponent(ComponentTypeID typeID) const {
        if (hasComponentType(typeID)) {
            return m_componentTypes;
        }
        ComponentTypeList result = m_componentTypes;
        result.push_back(typeID);
        std::sort(result.begin(), result.end());
        return result;
    }

    ComponentTypeList getWithRemovedComponent(ComponentTypeID typeID) const {
        if (!hasComponentType(typeID)) {
            return m_componentTypes;
        }
        ComponentTypeList result;
        result.reserve(m_componentTypes.size() - 1);
        for (auto type : m_componentTypes) {
            if (type != typeID) {
                result.push_back(type);
            }
        }
        return result;
    }

    bool operator==(const Archetype& other) const {
        return m_hash == other.m_hash && m_componentTypes == other.m_componentTypes;
    }

private:
    void computeHash() {
        m_hash = 0;
        for (auto type : m_componentTypes) {
            m_hash ^= std::hash<ComponentTypeID>{}(type) + 0x9e3779b9 + (m_hash << 6) + (m_hash >> 2);
        }
    }

    ComponentTypeList m_componentTypes;
    size_t m_hash = 0;
};

struct ArchetypeHash {
    size_t operator()(const Archetype& archetype) const {
        return archetype.getHash();
    }
};

namespace detail {

class ComponentSizeRegistry {
public:
    static constexpr size_t MAX_COMPONENT_TYPES = 256;

    static ComponentSizeRegistry& instance() {
        static ComponentSizeRegistry inst;
        return inst;
    }
    
    template<typename T>
    void registerType(ComponentTypeID typeID) {
        if (typeID >= MAX_COMPONENT_TYPES) {
            std::cerr << "[Archetype] ComponentTypeID " << typeID << " exceeds MAX_COMPONENT_TYPES (" << MAX_COMPONENT_TYPES << ")\n";
            return;
        }
        
        if (m_registered[typeID]) {
            return;
        }
        
        m_sizes[typeID] = sizeof(T);
        m_registered[typeID] = true;
    }
    
    size_t getSize(ComponentTypeID typeID) const {
        if (typeID >= MAX_COMPONENT_TYPES || !m_registered[typeID]) {
            std::cerr << "[Archetype] WARNING: Unknown component type ID " << typeID 
                      << " - not registered! Using safe default of 128 bytes.\n"
                      << "[Archetype] Add ECS_REGISTER_COMPONENT(T) to register component sizes.\n";
            return 128;
        }
        return m_sizes[typeID];
    }
    
    void setSize(ComponentTypeID typeID, size_t size) {
        if (typeID >= MAX_COMPONENT_TYPES) return;
        m_sizes[typeID] = size;
        m_registered[typeID] = true;
    }
    
    bool isRegistered(ComponentTypeID typeID) const {
        if (typeID >= MAX_COMPONENT_TYPES) return false;
        return m_registered[typeID];
    }

private:
    ComponentSizeRegistry() {
        m_registered.fill(false);
    }
    
    std::array<size_t, MAX_COMPONENT_TYPES> m_sizes;
    std::array<bool, MAX_COMPONENT_TYPES> m_registered;
};

template<typename T>
struct ComponentSizeRegistrar {
    ComponentSizeRegistrar(ComponentTypeID typeID) {
        ComponentSizeRegistry::instance().registerType<T>(typeID);
    }
};

} // namespace detail

#define ECS_REGISTER_COMPONENT(T) \
    static ::ecs::detail::ComponentSizeRegistrar<T> ECS_CONCAT(_registrar_, __LINE__)(::ecs::getComponentTypeID<T>())

#define ECS_REGISTER_COMPONENT_WITH_ID(T, id) \
    static ::ecs::detail::ComponentSizeRegistrar<T> ECS_CONCAT(_registrar_, __LINE__)(id)

#define ECS_CONCAT(a, b) ECS_CONCAT_IMPL(a, b)
#define ECS_CONCAT_IMPL(a, b) a##b

template<size_t ChunkSize = 256>
class Chunk {
public:
    static constexpr size_t CHUNK_SIZE = ChunkSize;

    Chunk(const Archetype::ComponentTypeList& componentTypes)
        : m_componentTypes(componentTypes)
        , m_size(0)
    {
        computeLayout();
    }

    bool isFull() const { return m_size >= CHUNK_SIZE; }
    size_t size() const { return m_size; }
    const Archetype::ComponentTypeList& getComponentTypes() const { return m_componentTypes; }

    size_t addEntity(EntityID entityID) {
        if (isFull()) {
            return INVALID_ENTITY_ID;
        }
        size_t index = m_size;
        m_entities[index] = entityID;
        m_size++;
        return index;
    }

    bool removeEntity(EntityID entityID) {
        for (size_t i = 0; i < m_size; ++i) {
            if (m_entities[i] == entityID) {
                removeAt(i);
                return true;
            }
        }
        return false;
    }

    void removeAt(size_t index) {
        assert(index < m_size && "Index out of bounds");
        size_t lastIndex = m_size - 1;
        
        for (size_t compIdx = 0; compIdx < m_componentTypes.size(); ++compIdx) {
            size_t elemSize = m_componentSizes[compIdx];
            void* srcData = getComponentPtrRaw(compIdx, lastIndex);
            void* dstData = getComponentPtrRaw(compIdx, index);
            std::memcpy(dstData, srcData, elemSize);
        }
        m_entities[index] = m_entities[lastIndex];
        m_size--;
    }

    EntityID getEntity(size_t index) const {
        assert(index < m_size && "Index out of bounds");
        return m_entities[index];
    }

    void* getComponentData(size_t index, ComponentTypeID typeID) {
        auto it = std::find(m_componentTypes.begin(), m_componentTypes.end(), typeID);
        if (it == m_componentTypes.end()) {
            return nullptr;
        }
        size_t compIdx = std::distance(m_componentTypes.begin(), it);
        return getComponentPtrRaw(compIdx, index);
    }

    const void* getComponentData(size_t index, ComponentTypeID typeID) const {
        auto it = std::find(m_componentTypes.begin(), m_componentTypes.end(), typeID);
        if (it == m_componentTypes.end()) {
            return nullptr;
        }
        size_t compIdx = std::distance(m_componentTypes.begin(), it);
        return getComponentPtrRaw(compIdx, index);
    }

    template<typename T>
    T* getComponent(size_t index) {
        return static_cast<T*>(getComponentData(index, getComponentTypeID<T>()));
    }

    template<typename T>
    const T* getComponent(size_t index) const {
        return static_cast<const T*>(getComponentData(index, getComponentTypeID<T>()));
    }

    // Direct access to contiguous buffer for systems that want raw iteration
    const uint8_t* getRawData() const { return m_chunkData.data(); }
    size_t getEntityStride() const { return m_entityStride; }
    size_t getComponentOffset(size_t compIdx) const { return m_componentOffsets[compIdx]; }
    size_t getComponentCount() const { return m_componentTypes.size(); }

    size_t begin() const { return 0; }
    size_t end() const { return m_size; }

private:
    void computeLayout() {
        m_componentSizes.resize(m_componentTypes.size());
        m_componentOffsets.resize(m_componentTypes.size());
        
        size_t stride = 0;
        for (size_t i = 0; i < m_componentTypes.size(); ++i) {
            m_componentSizes[i] = detail::ComponentSizeRegistry::instance().getSize(m_componentTypes[i]);
            m_componentOffsets[i] = stride;
            stride += m_componentSizes[i] * CHUNK_SIZE;
        }
        m_entityStride = stride / CHUNK_SIZE;
        
        m_chunkData.resize(stride);
    }

    void* getComponentPtrRaw(size_t compIdx, size_t entityIndex) {
        return m_chunkData.data() + m_componentOffsets[compIdx] + (entityIndex * m_componentSizes[compIdx]);
    }

    const void* getComponentPtrRaw(size_t compIdx, size_t entityIndex) const {
        return m_chunkData.data() + m_componentOffsets[compIdx] + (entityIndex * m_componentSizes[compIdx]);
    }

    Archetype::ComponentTypeList m_componentTypes;
    std::vector<uint8_t> m_chunkData;
    std::vector<size_t> m_componentSizes;
    std::vector<size_t> m_componentOffsets;
    std::array<EntityID, CHUNK_SIZE> m_entities;
    size_t m_entityStride = 0;
    size_t m_size;
};

template<size_t ChunkSize>
class ArchetypeData {
public:
    using ChunkType = Chunk<ChunkSize>;

    ArchetypeData(const Archetype& archetype)
        : m_archetype(archetype)
    {
        if (!m_archetype.getComponentTypes().empty()) {
            m_chunks.emplace_back(m_archetype.getComponentTypes());
        }
    }

    const Archetype& getArchetype() const { return m_archetype; }

    std::pair<size_t, size_t> addEntity(EntityID entityID) {
        for (size_t i = 0; i < m_chunks.size(); ++i) {
            if (!m_chunks[i].isFull()) {
                size_t index = m_chunks[i].addEntity(entityID);
                return {i, index};
            }
        }
        m_chunks.emplace_back(m_archetype.getComponentTypes());
        size_t chunkIndex = m_chunks.size() - 1;
        size_t index = m_chunks[chunkIndex].addEntity(entityID);
        return {chunkIndex, index};
    }

    bool removeEntity(EntityID entityID) {
        for (size_t i = 0; i < m_chunks.size(); ++i) {
            if (m_chunks[i].removeEntity(entityID)) {
                return true;
            }
        }
        return false;
    }

    size_t entityCount() const {
        size_t count = 0;
        for (const auto& chunk : m_chunks) {
            count += chunk.size();
        }
        return count;
    }

    const std::vector<ChunkType>& getChunks() const { return m_chunks; }
    std::vector<ChunkType>& getChunks() { return m_chunks; }

    bool hasComponentType(ComponentTypeID typeID) const {
        return m_archetype.hasComponentType(typeID);
    }

private:
    Archetype m_archetype;
    std::vector<ChunkType> m_chunks;
};

} // namespace ecs
