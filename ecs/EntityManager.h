#pragma once

#include "Entity.h"
#include <vector>
#include <queue>
#include <bitset>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>

namespace ecs {

/**
 * Signature - Bitset representing which components an entity has
 */
using Signature = std::bitset<256>;  // Supports up to 256 component types

/**
 * Entity Manager - Creates, destroys, and tracks entities
 */
class EntityManager {
public:
    EntityManager() {
        // Initialize all entities as available
        for (EntityID i = 0; i < MAX_ENTITIES; ++i) {
            m_availableEntities.push(i);
        }
    }

    /**
     * Create a new entity
     */
    Entity createEntity() {
        assert(m_availableEntities.size() > 0 && "No available entities");
        
        EntityID id = m_availableEntities.front();
        m_availableEntities.pop();
        
        Entity entity{id};
        m_liveEntities.insert(id);
        m_signatures[id] = Signature();
        
        return entity;
    }

    /**
     * Destroy an entity
     */
    void destroyEntity(Entity entity) {
        assert(entity.isValid() && m_liveEntities.count(entity.id) && "Invalid entity");
        
        m_liveEntities.erase(entity.id);
        m_signatures[entity.id].reset();
        m_availableEntities.push(entity.id);
        
        // Notify destruction callbacks
        for (auto& callback : m_destructionCallbacks) {
            callback(entity.id);
        }
    }

    /**
     * Get the signature of an entity
     */
    const Signature& getSignature(EntityID entityID) const {
        assert(m_liveEntities.count(entityID) && "Invalid entity");
        return m_signatures[entityID];
    }

    /**
     * Set the signature of an entity
     */
    void setSignature(EntityID entityID, Signature signature) {
        assert(m_liveEntities.count(entityID) && "Invalid entity");
        m_signatures[entityID] = signature;
    }

    /**
     * Add a component type bit to an entity's signature
     */
    template<typename T>
    void addComponentType(EntityID entityID) {
        assert(m_liveEntities.count(entityID) && "Invalid entity");
        m_signatures[entityID].set(getComponentTypeID<T>());
    }

    /**
     * Remove a component type bit from an entity's signature
     */
    template<typename T>
    void removeComponentType(EntityID entityID) {
        assert(m_liveEntities.count(entityID) && "Invalid entity");
        m_signatures[entityID].reset(getComponentTypeID<T>());
    }

    /**
     * Check if entity is alive
     */
    bool isAlive(EntityID entityID) const {
        return m_liveEntities.count(entityID) > 0;
    }

    /**
     * Get all live entities
     */
    const std::unordered_set<EntityID>& getLiveEntities() const {
        return m_liveEntities;
    }

    /**
     * Get the number of live entities
     */
    size_t getLiveEntityCount() const {
        return m_liveEntities.size();
    }

    /**
     * Get the number of available entities
     */
    size_t getAvailableEntityCount() const {
        return m_availableEntities.size();
    }

    /**
     * Register an entity destruction callback
     */
    void registerDestructionCallback(std::function<void(EntityID)> callback) {
        m_destructionCallbacks.push_back(std::move(callback));
    }

    /**
     * Destroy all entities
     */
    void destroyAll() {
        for (EntityID id : m_liveEntities) {
            m_signatures[id].reset();
            m_availableEntities.push(id);
        }
        m_liveEntities.clear();
    }

private:
    std::queue<EntityID> m_availableEntities;
    std::unordered_set<EntityID> m_liveEntities;
    std::array<Signature, MAX_ENTITIES> m_signatures;
    std::vector<std::function<void(EntityID)>> m_destructionCallbacks;
};

} // namespace ecs
