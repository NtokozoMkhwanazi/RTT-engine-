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
 * Provides O(1) access by component index
 */
template<typename T, size_t Capacity = MAX_ENTITIES>
class ComponentArray {
public:
    using ValueType = T;
    static constexpr size_t CAPACITY = Capacity;

    ComponentArray() {
        m_components.reserve(Capacity);
        m_indexToEntity.resize(Capacity, INVALID_ENTITY_ID);
    }

    /**
     * Insert a component for an entity
     */
    template<typename... Args>
    T& insert(EntityID entityID, Args&&... args) {
        assert(m_size < Capacity && "Component array full");

        // Check if component already exists - if so, return existing component
        auto it = m_entityToIndex.find(entityID);
        if (it != m_entityToIndex.end()) {
            return m_components[it->second];
        }

        // Use m_size as the index (dense array)
        EntityID index = static_cast<EntityID>(m_size);

        // Emplace new component at end
        m_components.emplace_back(std::forward<Args>(args)...);
        m_components.back().entity = Entity{entityID};
        m_entityToIndex[entityID] = index;
        m_indexToEntity[index] = entityID;
        m_size++;

        return m_components.back();
    }

    /**
     * Remove a component by entity ID
     */
    void remove(EntityID entityID) {
        assert(hasComponent(entityID) && "Component does not exist");

        EntityID index = m_entityToIndex[entityID];
        EntityID lastIndex = static_cast<EntityID>(m_size - 1);

        // If not the last element, swap with last
        if (index != lastIndex) {
            m_components[index] = std::move(m_components[lastIndex]);
            m_components[index].entity = Entity{m_indexToEntity[lastIndex]};

            // Update mappings for the swapped entity
            EntityID swappedEntityID = m_indexToEntity[lastIndex];
            m_entityToIndex[swappedEntityID] = index;
            m_indexToEntity[index] = swappedEntityID;
        }

        // Remove the last element
        m_components.pop_back();
        m_entityToIndex.erase(entityID);
        m_indexToEntity[lastIndex] = INVALID_ENTITY_ID;
        m_size--;
    }

    /**
     * Get component by entity ID
     */
    T* get(EntityID entityID) {
        auto it = m_entityToIndex.find(entityID);
        if (it == m_entityToIndex.end()) return nullptr;
        return &m_components[it->second];
    }

    const T* get(EntityID entityID) const {
        auto it = m_entityToIndex.find(entityID);
        if (it == m_entityToIndex.end()) return nullptr;
        return &m_components[it->second];
    }

    /**
     * Check if entity has this component
     */
    bool hasComponent(EntityID entityID) const {
        return m_entityToIndex.find(entityID) != m_entityToIndex.end();
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
     * Get entity ID by component index
     */
    EntityID getEntity(EntityID index) const {
        assert(index < m_size && "Index out of bounds");
        return m_indexToEntity[index];
    }

private:
    std::vector<T> m_components;
    std::unordered_map<EntityID, EntityID> m_entityToIndex;
    std::vector<EntityID> m_indexToEntity;  // Use vector instead of array to avoid stack overflow
    size_t m_size = 0;
};

} // namespace ecs
