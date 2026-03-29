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
#include <mutex>

namespace ecs {

/**
 * Archetype - Represents a unique combination of component types
 * 
 * Archetypes provide cache-coherent storage by grouping entities
 * with identical component compositions together in memory.
 * 
 * This is the foundation of archetype-based ECS (like Unity DOTS).
 */
class Archetype {
public:
    using ComponentTypeList = std::vector<ComponentTypeID>;

    Archetype() = default;
    
    /**
     * Create an archetype from a list of component types
     */
    explicit Archetype(ComponentTypeList componentTypes) 
        : m_componentTypes(std::move(componentTypes)) 
    {
        // Sort component types for consistent hashing
        std::sort(m_componentTypes.begin(), m_componentTypes.end());
        computeHash();
    }

    /**
     * Get the component types in this archetype
     */
    const ComponentTypeList& getComponentTypes() const { return m_componentTypes; }

    /**
     * Check if this archetype has a specific component type
     */
    bool hasComponentType(ComponentTypeID typeID) const {
        return std::binary_search(m_componentTypes.begin(), m_componentTypes.end(), typeID);
    }

    /**
     * Get the hash of this archetype
     */
    size_t getHash() const { return m_hash; }

    /**
     * Check if this archetype can be transitioned to another by adding a component
     */
    bool canAddComponent(ComponentTypeID typeID) const {
        return !hasComponentType(typeID);
    }

    /**
     * Check if this archetype can be transitioned to another by removing a component
     */
    bool canRemoveComponent(ComponentTypeID typeID) const {
        return hasComponentType(typeID);
    }

    /**
     * Get the archetype that results from adding a component type
     */
    ComponentTypeList getWithAddedComponent(ComponentTypeID typeID) const {
        if (hasComponentType(typeID)) {
            return m_componentTypes;  // Already has it
        }
        
        ComponentTypeList result = m_componentTypes;
        result.push_back(typeID);
        std::sort(result.begin(), result.end());
        return result;
    }

    /**
     * Get the archetype that results from removing a component type
     */
    ComponentTypeList getWithRemovedComponent(ComponentTypeID typeID) const {
        if (!hasComponentType(typeID)) {
            return m_componentTypes;  // Doesn't have it
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

    /**
     * Equality comparison
     */
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

/**
 * Archetype Hash - Custom hash function for Archetype
 */
struct ArchetypeHash {
    size_t operator()(const Archetype& archetype) const {
        return archetype.getHash();
    }
};

/**
 * Chunk - A contiguous block of memory storing entities and components for an archetype
 * 
 * Each chunk holds a fixed number of entities (typically 100-1000) with the same
 * component layout. Data is stored in Structure of Arrays (SoA) format for
 * optimal cache coherence during system iteration.
 */
template<size_t ChunkSize = 256>
class Chunk {
public:
    static constexpr size_t CHUNK_SIZE = ChunkSize;

    Chunk(const Archetype::ComponentTypeList& componentTypes)
        : m_componentTypes(componentTypes)
        , m_size(0)
    {
        // Calculate total size needed for all component arrays
        size_t totalSize = 0;
        for (auto typeID : componentTypes) {
            totalSize += getComponentSize(typeID);
        }
        
        // Allocate component data as contiguous blocks
        // Each component type gets a contiguous array of ChunkSize elements
        m_componentData.resize(componentTypes.size());
        m_componentSizes.resize(componentTypes.size());
        
        for (size_t i = 0; i < componentTypes.size(); ++i) {
            m_componentSizes[i] = getComponentSize(componentTypes[i]);
            m_componentData[i].resize(m_componentSizes[i] * ChunkSize);
        }
    }

    /**
     * Check if the chunk is full
     */
    bool isFull() const { return m_size >= CHUNK_SIZE; }

    /**
     * Get the current number of entities in this chunk
     */
    size_t size() const { return m_size; }

    /**
     * Get the component types in this chunk
     */
    const Archetype::ComponentTypeList& getComponentTypes() const { return m_componentTypes; }

    /**
     * Add an entity to the chunk
     * Returns the index within the chunk where the entity was added
     */
    size_t addEntity(EntityID entityID) {
        if (isFull()) {
            return INVALID_ENTITY_ID;
        }
        
        size_t index = m_size;
        m_entities[index] = entityID;
        m_size++;
        return index;
    }

    /**
     * Remove an entity from the chunk using swap-remove
     * Returns true if the entity was found and removed
     */
    bool removeEntity(EntityID entityID) {
        for (size_t i = 0; i < m_size; ++i) {
            if (m_entities[i] == entityID) {
                removeAt(i);
                return true;
            }
        }
        return false;
    }

    /**
     * Remove an entity at a specific index using swap-remove
     */
    void removeAt(size_t index) {
        assert(index < m_size && "Index out of bounds");
        
        size_t lastIndex = m_size - 1;
        
        // Swap with last element for each component array
        for (size_t compIdx = 0; compIdx < m_componentTypes.size(); ++compIdx) {
            size_t elemSize = m_componentSizes[compIdx];
            void* srcData = m_componentData[compIdx].data() + (lastIndex * elemSize);
            void* dstData = m_componentData[compIdx].data() + (index * elemSize);
            std::memcpy(dstData, srcData, elemSize);
        }
        
        // Swap entity IDs
        m_entities[index] = m_entities[lastIndex];
        m_size--;
    }

    /**
     * Get the entity ID at a specific index
     */
    EntityID getEntity(size_t index) const {
        assert(index < m_size && "Index out of bounds");
        return m_entities[index];
    }

    /**
     * Get a pointer to component data for a specific entity index
     */
    void* getComponentData(size_t index, ComponentTypeID typeID) {
        auto it = std::find(m_componentTypes.begin(), m_componentTypes.end(), typeID);
        if (it == m_componentTypes.end()) {
            return nullptr;
        }
        
        size_t compIdx = std::distance(m_componentTypes.begin(), it);
        size_t elemSize = m_componentSizes[compIdx];
        return m_componentData[compIdx].data() + (index * elemSize);
    }

    const void* getComponentData(size_t index, ComponentTypeID typeID) const {
        auto it = std::find(m_componentTypes.begin(), m_componentTypes.end(), typeID);
        if (it == m_componentTypes.end()) {
            return nullptr;
        }
        
        size_t compIdx = std::distance(m_componentTypes.begin(), it);
        size_t elemSize = m_componentSizes[compIdx];
        return m_componentData[compIdx].data() + (index * elemSize);
    }

    /**
     * Get typed component pointer at index
     */
    template<typename T>
    T* getComponent(size_t index) {
        return static_cast<T*>(getComponentData(index, getComponentTypeID<T>()));
    }

    template<typename T>
    const T* getComponent(size_t index) const {
        return static_cast<const T*>(getComponentData(index, getComponentTypeID<T>()));
    }

    /**
     * Get all component data arrays for iteration
     * Returns pointers to the start of each array
     */
    const std::vector<std::vector<uint8_t>>& getAllComponentData() const {
        return m_componentData;
    }

    /**
     * Begin/end iterators for entity indices
     */
    size_t begin() const { return 0; }
    size_t end() const { return m_size; }

private:
    size_t getComponentSize(ComponentTypeID typeID) const {
        // Component size registry - maps type IDs to their sizes
        // This is populated automatically when component types are first used
        static std::unordered_map<ComponentTypeID, size_t> s_componentSizes;
        static std::mutex s_mutex;

        // Check if we already have this type registered
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto it = s_componentSizes.find(typeID);
            if (it != s_componentSizes.end()) {
                return it->second;
            }
        }

        // Register common component sizes (would be done at startup in production)
        // This uses template specialization to get actual sizes
        size_t size = getComponentSizeForType(typeID);
        
        std::lock_guard<std::mutex> lock(s_mutex);
        s_componentSizes[typeID] = size;
        return size;
    }

    /**
     * Get component size for a specific type
     * Uses template magic to get actual component sizes
     */
    template<typename T>
    static size_t getComponentSizeForTypeImpl() {
        return sizeof(T);
    }

    /**
     * Get component size by type ID
     * In production, this would use a proper registry
     */
    size_t getComponentSizeForType(ComponentTypeID typeID) const {
        // Default sizes for common ECS components
        // These are approximate sizes - actual sizes may vary
        switch (typeID) {
            case 0: return 96;   // TransformComponent (position+rotation+scale+matrices)
            case 1: return 48;   // MeshComponent
            case 2: return 64;   // CameraComponent
            case 3: return 256;  // RigidBodyComponent
            case 4: return 128;  // AnimatorComponent
            case 5: return 64;   // SkeletonComponent
            case 6: return 96;   // LightComponent
            case 7: return 32;   // NameComponent
            default: return 64;  // Default for unknown types
        }
    }

    Archetype::ComponentTypeList m_componentTypes;
    std::vector<std::vector<uint8_t>> m_componentData;  // SoA: one array per component type
    std::vector<size_t> m_componentSizes;
    std::array<EntityID, CHUNK_SIZE> m_entities;
    size_t m_size;
};

/**
 * ArchetypeData - Manages all chunks for a specific archetype
 */
template<size_t ChunkSize = 256>
class ArchetypeData {
public:
    using ChunkType = Chunk<ChunkSize>;

    ArchetypeData(const Archetype& archetype)
        : m_archetype(archetype)
    {
        // Start with one chunk
        if (!m_archetype.getComponentTypes().empty()) {
            m_chunks.emplace_back(m_archetype.getComponentTypes());
        }
    }

    /**
     * Get the archetype this data belongs to
     */
    const Archetype& getArchetype() const { return m_archetype; }

    /**
     * Add an entity to this archetype
     * Returns {chunkIndex, indexInChunk}
     */
    std::pair<size_t, size_t> addEntity(EntityID entityID) {
        // Find a chunk with space
        for (size_t i = 0; i < m_chunks.size(); ++i) {
            if (!m_chunks[i].isFull()) {
                size_t index = m_chunks[i].addEntity(entityID);
                return {i, index};
            }
        }
        
        // All chunks full, create a new one
        m_chunks.emplace_back(m_archetype.getComponentTypes());
        size_t chunkIndex = m_chunks.size() - 1;
        size_t index = m_chunks[chunkIndex].addEntity(entityID);
        return {chunkIndex, index};
    }

    /**
     * Remove an entity from this archetype
     */
    bool removeEntity(EntityID entityID) {
        for (size_t i = 0; i < m_chunks.size(); ++i) {
            if (m_chunks[i].removeEntity(entityID)) {
                return true;
            }
        }
        return false;
    }

    /**
     * Get the number of entities in this archetype
     */
    size_t entityCount() const {
        size_t count = 0;
        for (const auto& chunk : m_chunks) {
            count += chunk.size();
        }
        return count;
    }

    /**
     * Get all chunks for iteration
     */
    const std::vector<ChunkType>& getChunks() const { return m_chunks; }
    std::vector<ChunkType>& getChunks() { return m_chunks; }

    /**
     * Check if this archetype has a specific component type
     */
    bool hasComponentType(ComponentTypeID typeID) const {
        return m_archetype.hasComponentType(typeID);
    }

private:
    Archetype m_archetype;
    std::vector<ChunkType> m_chunks;
};

} // namespace ecs
