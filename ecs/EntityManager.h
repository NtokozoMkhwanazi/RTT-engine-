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
        
        // Stamp the slot's current generation so the handle carries the epoch it
        // was created in (Phase B). A slot's gen starts at 0 and is bumped by
        // destroyEntity, so a recycled id always gets a fresh gen.
        Entity entity{id, m_generations[id]};
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
        // Bump the slot's generation so any handle still holding the OLD gen is
        // disambiguated from the entity this id is recycled to (Phase B). Mod 256
        // is safe in practice: with MAX_ENTITIES=10000, the same slot would have
        // to be destroyed+recreated 256 times — impossible within a session.
        ++m_generations[entity.id];
        
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
     * Slot generation for an entity id (Phase B). Used by World::valid() to
     * disambiguate a destroyed handle from an id that was later recycled. Only
     * ever called on a live id (valid() short-circuits via isValid() + isAlive
     * first; live ids are always < MAX_ENTITIES, so the in-range guard is
     * belt-and-suspenders against a stray caller).
     */
    uint8_t generation(EntityID entityID) const {
        return entityID < MAX_ENTITIES ? m_generations[entityID] : uint8_t{0};
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
    // Per-slot generation epoch (Phase B): 0 for fresh slots, ++ per destroy.
    // Bumped in destroyEntity() so a recycled id always carries a new gen.
    std::array<uint8_t, MAX_ENTITIES> m_generations{};
    std::vector<std::function<void(EntityID)>> m_destructionCallbacks;
};

} // namespace ecs
