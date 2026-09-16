#pragma once

#include "Entity.h"
#include "EntityManager.h"
#include "ComponentManager.h"
#include <vector>
#include <functional>
#include <memory>

namespace ecs {

/**
 * System Signature Filter - Defines which entities a system processes
 */
struct SystemFilter {
    Signature required;    // Components that MUST be present
    Signature excluded;    // Components that MUST NOT be present
    Signature any;         // At least ONE of these must be present (optional)
    
    SystemFilter() = default;
    
    /**
     * Set required components
     */
    template<typename... Components>
    static SystemFilter require() {
        SystemFilter filter;
        (filter.required.set(getComponentTypeID<Components>()), ...);
        return filter;
    }
    
    /**
     * Set excluded components
     */
    template<typename... Components>
    static SystemFilter exclude() {
        SystemFilter filter;
        (filter.excluded.set(getComponentTypeID<Components>()), ...);
        return filter;
    }
    
    /**
     * Set "any of" components
     */
    template<typename... Components>
    static SystemFilter anyOf() {
        SystemFilter filter;
        (filter.any.set(getComponentTypeID<Components>()), ...);
        return filter;
    }
    
    /**
     * Combine filters
     */
    SystemFilter& withRequired(const SystemFilter& other) {
        required |= other.required;
        return *this;
    }
    
    SystemFilter& withExcluded(const SystemFilter& other) {
        excluded |= other.excluded;
        return *this;
    }
    
    SystemFilter& withAny(const SystemFilter& other) {
        any |= other.any;
        return *this;
    }
    
    /**
     * Check if an entity matches this filter
     */
    bool matches(const Signature& entitySignature) const {
        // Check required components
        if ((entitySignature & required) != required) {
            return false;
        }
        
        // Check excluded components
        if ((entitySignature & excluded).any()) {
            return false;
        }
        
        // Check "any of" components
        if (any.any() && (entitySignature & any).none()) {
            return false;
        }
        
        return true;
    }
};

/**
 * Base System class
 */
class System {
public:
    System() = default;
    virtual ~System() = default;
    
    /**
     * Set the system filter
     */
    void setFilter(const SystemFilter& filter) { m_filter = filter; }
    
    /**
     * Get the system filter
     */
    const SystemFilter& getFilter() const { return m_filter; }
    
    /**
     * Check if an entity matches the filter
     */
    bool matchesEntity(const Signature& signature) const {
        return m_filter.matches(signature);
    }
    
    /**
     * Initialize the system (called once at startup)
     */
    virtual void init() {}
    
    /**
     * Update the system (called every frame)
     */
    virtual void update(float deltaTime) = 0;
    
    /**
     * Render the system (called every frame, after update)
     */
    virtual void render() {}
    
    /**
     * Shutdown the system (called once at cleanup)
     */
    virtual void shutdown() {}
    
    /**
     * Get system name (for debugging)
     */
    virtual const char* getName() const = 0;
    
    /**
     * Set system priority (lower = runs first)
     */
    void setPriority(int priority) { m_priority = priority; }
    int getPriority() const { return m_priority; }
    
    /**
     * Enable/disable the system
     */
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }
    void enable() { m_enabled = true; }
    void disable() { m_enabled = false; }

protected:
    SystemFilter m_filter;
    int m_priority = 0;
    bool m_enabled = true;
};

/**
 * TypedSystem base class (for dynamic_cast in World)
 */
class TypedSystemBase {
public:
    virtual ~TypedSystemBase() = default;
    virtual void setManagers(EntityManager* em, ComponentManager* cm) = 0;
};

/**
 * Typed System - Provides type-safe access to components
 */
template<typename... Components>
class TypedSystem : public System, public TypedSystemBase {
public:
    /**
     * Set managers (called by World before update)
     */
    void setManagers(EntityManager* em, ComponentManager* cm) override {
        m_entityManager = em;
        m_componentManager = cm;
    }
    
    /**
     * Get a component for an entity
     */
    template<typename T>
    T* getComponent(EntityID entityID) {
        if (!m_componentManager) return nullptr;
        return m_componentManager->getComponent<T>(entityID);
    }
    
    /**
     * Check if an entity has a component
     */
    template<typename T>
    bool hasComponent(EntityID entityID) {
        if (!m_componentManager) return false;
        return m_componentManager->hasComponent<T>(entityID);
    }
    
    /**
     * Iterate over all matching entities
     */
    template<typename Func>
    void forEach(EntityManager& entityManager, ComponentManager& componentManager, Func&& func) {
        for (EntityID entityID : entityManager.getLiveEntities()) {
            if (!matchesEntity(entityManager.getSignature(entityID))) {
                continue;
            }
            
            // Get all components
            auto components = std::make_tuple(
                componentManager.getComponent<Components>(entityID)...
            );
            
            // Check if all components exist
            bool allExist = std::apply([](auto*... comps) {
                return ((comps != nullptr) && ...);
            }, components);
            
            if (!allExist) continue;
            
            // Call the function with entity ID and components
            std::apply([this, &func, entityID](auto*... comps) {
                func(entityID, *comps...);
            }, components);
        }
    }
    
protected:
    EntityManager* m_entityManager = nullptr;
    ComponentManager* m_componentManager = nullptr;
};

} // namespace ecs
