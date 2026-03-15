#pragma once

#include "Entity.h"
#include <memory>
#include <vector>
#include <array>
#include <type_traits>
#include <cassert>
#include <unordered_map>

namespace ecs {

/**
 * Base component class (marker interface)
 */
struct Component {
    virtual ~Component() = default;
    Entity entity{INVALID_ENTITY_ID};
    bool isActive = true;
};

/**
 * Component Array - Stores components of a single type in a dense array
 * Provides O(1) access by entity index
 */
template<typename T, size_t Capacity = MAX_ENTITIES>
class ComponentArray {
public:
    using ValueType = T;
    static constexpr size_t CAPACITY = Capacity;

    ComponentArray() = default;

    /**
     * Insert a component for an entity
     */
    template<typename... Args>
    T& insert(EntityID entityID, EntityID index, Args&&... args) {
        assert(index < Capacity && "Entity index out of bounds");
        assert(!hasComponent(entityID) && "Component already exists");
        
        m_components[index] = T(std::forward<Args>(args)...);
        m_components[index].entity = Entity{entityID};
        m_entityToIndex[entityID] = index;
        m_indexToEntity[index] = entityID;
        m_size++;
        
        return m_components[index];
    }

    /**
     * Remove a component by entity ID
     */
    void remove(EntityID entityID) {
        assert(hasComponent(entityID) && "Component does not exist");
        
        EntityID index = m_entityToIndex[entityID];
        EntityID lastIndex = m_size - 1;
        
        // If not the last element, swap with last
        if (index != lastIndex) {
            m_components[index] = std::move(m_components[lastIndex]);
            m_components[index].entity = Entity{m_indexToEntity[lastIndex]};
            
            // Update mappings for the swapped entity
            m_entityToIndex[m_indexToEntity[lastIndex]] = index;
            m_indexToEntity[index] = m_indexToEntity[lastIndex];
        }
        
        // Clear the last slot
        m_entityToIndex.erase(entityID);
        m_indexToEntity[lastIndex] = INVALID_ENTITY_ID;
        m_size--;
    }

    /**
     * Get component by entity ID
     */
    T* get(EntityID entityID) {
        if (!hasComponent(entityID)) return nullptr;
        return &m_components[m_entityToIndex[entityID]];
    }

    const T* get(EntityID entityID) const {
        auto it = m_entityToIndex.find(entityID);
        if (it == m_entityToIndex.end()) return nullptr;
        return &m_components[it->second];
    }

    /**
     * Get component by index (for system iteration)
     */
    T& getByIndex(EntityID index) {
        assert(index < m_size && "Index out of bounds");
        return m_components[index];
    }

    const T& getByIndex(EntityID index) const {
        assert(index < m_size && "Index out of bounds");
        return m_components[index];
    }

    /**
     * Check if entity has this component
     */
    bool hasComponent(EntityID entityID) const {
        return m_entityToIndex.find(entityID) != m_entityToIndex.end();
    }

    /**
     * Get entity ID by component index
     */
    EntityID getEntity(EntityID index) const {
        assert(index < m_size && "Index out of bounds");
        return m_indexToEntity[index];
    }

    /**
     * Get the number of components
     */
    size_t size() const { return m_size; }

    /**
     * Get capacity
     */
    static constexpr size_t capacity() { return Capacity; }

    /**
     * Begin iteration (for range-based for loops)
     */
    T* begin() { return m_components.data(); }
    T* end() { return m_components.data() + m_size; }
    const T* begin() const { return m_components.data(); }
    const T* end() const { return m_components.data() + m_size; }

    /**
     * Get entity-to-index mapping
     */
    const std::unordered_map<EntityID, EntityID>& getEntityToIndexMap() const {
        return m_entityToIndex;
    }

private:
    std::array<T, Capacity> m_components;
    std::unordered_map<EntityID, EntityID> m_entityToIndex;  // EntityID -> Component index
    std::array<EntityID, Capacity> m_indexToEntity;          // Component index -> EntityID
    size_t m_size = 0;
};

/**
 * Concept for component types (compile-time check) - C++20
 * For C++17 compatibility, use static_assert instead
 */
// template<typename T>
// concept ComponentType = std::is_base_of_v<Component, T>;

} // namespace ecs
