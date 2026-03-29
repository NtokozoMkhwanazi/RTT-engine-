#pragma once

#include "Entity.h"
#include "Component.h"
#include <unordered_map>
#include <memory>
#include <string>
#include <typeindex>
#include <vector>
#include <functional>

namespace ecs {

/**
 * Component Manager - Manages all component arrays
 * Uses a simple type-erasure approach with void pointers
 */
class ComponentManager {
private:
    // Type-erased interface for component arrays
    class IComponentArray {
    public:
        virtual ~IComponentArray() = default;
        virtual void remove(EntityID entityID) = 0;
        virtual bool hasComponent(EntityID entityID) const = 0;
    };

    template<typename T>
    class ComponentArrayHolder : public IComponentArray {
    public:
        ComponentArray<T> array;

        void remove(EntityID entityID) override {
            array.remove(entityID);
        }

        bool hasComponent(EntityID entityID) const override {
            return array.hasComponent(entityID);
        }
    };

public:
    ComponentManager() = default;

    /**
     * Add a component to an entity
     */
    template<typename T, typename... Args>
    T& addComponent(EntityID entityID, Args&&... args) {
        ComponentTypeID typeID = getComponentTypeID<T>();

        // Get or create the component array for this type
        auto it = m_componentArrays.find(typeID);
        if (it == m_componentArrays.end()) {
            // Create new array for this component type
            auto holder = std::make_unique<ComponentArrayHolder<T>>();
            m_componentArrays[typeID] = std::move(holder);
            it = m_componentArrays.find(typeID);
        }

        // Cast to the correct type and insert
        auto* holder = static_cast<ComponentArrayHolder<T>*>(it->second.get());
        return holder->array.insert(entityID, std::forward<Args>(args)...);
    }

    /**
     * Remove a component from an entity
     */
    template<typename T>
    void removeComponent(EntityID entityID) {
        ComponentTypeID typeID = getComponentTypeID<T>();

        auto it = m_componentArrays.find(typeID);
        if (it == m_componentArrays.end()) {
            return;  // Component type not registered
        }

        auto* holder = static_cast<ComponentArrayHolder<T>*>(it->second.get());
        holder->array.remove(entityID);
    }

    /**
     * Get a component from an entity
     */
    template<typename T>
    T* getComponent(EntityID entityID) {
        ComponentTypeID typeID = getComponentTypeID<T>();

        auto it = m_componentArrays.find(typeID);
        if (it == m_componentArrays.end()) {
            return nullptr;
        }

        auto* holder = static_cast<ComponentArrayHolder<T>*>(it->second.get());
        return holder->array.get(entityID);
    }

    template<typename T>
    const T* getComponent(EntityID entityID) const {
        ComponentTypeID typeID = getComponentTypeID<T>();

        auto it = m_componentArrays.find(typeID);
        if (it == m_componentArrays.end()) {
            return nullptr;
        }

        const auto* holder = static_cast<const ComponentArrayHolder<T>*>(it->second.get());
        return holder->array.get(entityID);
    }

    /**
     * Check if an entity has a component
     */
    template<typename T>
    bool hasComponent(EntityID entityID) const {
        ComponentTypeID typeID = getComponentTypeID<T>();

        auto it = m_componentArrays.find(typeID);
        if (it == m_componentArrays.end()) {
            return false;
        }

        const auto* holder = static_cast<const ComponentArrayHolder<T>*>(it->second.get());
        return holder->array.hasComponent(entityID);
    }

    /**
     * Get component array for iteration
     */
    template<typename T>
    ComponentArray<T>* getComponentArray() {
        ComponentTypeID typeID = getComponentTypeID<T>();

        auto it = m_componentArrays.find(typeID);
        if (it == m_componentArrays.end()) {
            return nullptr;
        }

        auto* holder = static_cast<ComponentArrayHolder<T>*>(it->second.get());
        return &holder->array;
    }

    template<typename T>
    const ComponentArray<T>* getComponentArray() const {
        ComponentTypeID typeID = getComponentTypeID<T>();

        auto it = m_componentArrays.find(typeID);
        if (it == m_componentArrays.end()) {
            return nullptr;
        }

        const auto* holder = static_cast<const ComponentArrayHolder<T>*>(it->second.get());
        return &holder->array;
    }

    /**
     * Get all registered component type IDs
     */
    std::vector<ComponentTypeID> getRegisteredTypeIDs() const {
        std::vector<ComponentTypeID> ids;
        ids.reserve(m_componentArrays.size());
        for (const auto& pair : m_componentArrays) {
            ids.push_back(pair.first);
        }
        return ids;
    }

private:
    std::unordered_map<ComponentTypeID, std::unique_ptr<IComponentArray>> m_componentArrays;
};

} // namespace ecs
