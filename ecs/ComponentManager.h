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
 */
class ComponentManager {
private:
    /**
     * Type-erased wrapper for component arrays (must be defined first)
     */
    struct ComponentArrayWrapperBase {
        virtual ~ComponentArrayWrapperBase() = default;
        virtual void remove(EntityID entityID) = 0;
        virtual bool hasComponent(EntityID entityID) const = 0;
    };

    template<typename T>
    struct ComponentArrayWrapper : ComponentArrayWrapperBase {
        ComponentArray<T> array;

        template<typename... Args>
        T& insert(EntityID entityID, EntityID index, Args&&... args) {
            return array.insert(entityID, index, std::forward<Args>(args)...);
        }

        T* get(EntityID entityID) { return array.get(entityID); }
        const T* get(EntityID entityID) const { return array.get(entityID); }

        void remove(EntityID entityID) override { array.remove(entityID); }
        bool hasComponent(EntityID entityID) const override {
            return array.hasComponent(entityID);
        }

        // Iteration support
        T* begin() { return array.begin(); }
        T* end() { return array.end(); }
        const T* begin() const { return array.begin(); }
        const T* end() const { return array.end(); }

        size_t size() const { return array.size(); }

        T& getByIndex(EntityID index) { return array.get(index); }
        const T& getByIndex(EntityID index) const { return array.get(index); }
        EntityID getEntityByIndex(EntityID index) const { return array.getEntity(index); }
    };

public:
    ComponentManager() = default;

    /**
     * Register a component type
     */
    template<typename T>
    void registerComponentType() {
        ComponentTypeID typeID = getComponentTypeID<T>();

        if (m_componentArrays.find(typeID) != m_componentArrays.end()) {
            return;  // Already registered
        }

        m_componentArrays[typeID] = std::make_unique<ComponentArrayWrapper<T>>();
        m_componentNames[typeID] = typeid(T).name();
    }

    /**
     * Add a component to an entity
     */
    template<typename T, typename... Args>
    T& addComponent(EntityID entityID, Args&&... args) {
        ComponentTypeID typeID = getComponentTypeID<T>();

        if (m_componentArrays.find(typeID) == m_componentArrays.end()) {
            registerComponentType<T>();
        }

        auto* array = static_cast<ComponentArrayWrapper<T>*>(m_componentArrays[typeID].get());
        return array->insert(entityID, entityID, std::forward<Args>(args)...);
    }

    /**
     * Remove a component from an entity
     */
    template<typename T>
    void removeComponent(EntityID entityID) {
        ComponentTypeID typeID = getComponentTypeID<T>();

        if (m_componentArrays.find(typeID) == m_componentArrays.end()) {
            return;  // Component type not registered
        }

        auto* array = static_cast<ComponentArrayWrapper<T>*>(m_componentArrays[typeID].get());
        array->remove(entityID);
    }

    /**
     * Get a component from an entity
     */
    template<typename T>
    T* getComponent(EntityID entityID) {
        ComponentTypeID typeID = getComponentTypeID<T>();

        if (m_componentArrays.find(typeID) == m_componentArrays.end()) {
            return nullptr;
        }

        auto* array = static_cast<ComponentArrayWrapper<T>*>(m_componentArrays[typeID].get());
        return array->get(entityID);
    }

    template<typename T>
    const T* getComponent(EntityID entityID) const {
        ComponentTypeID typeID = getComponentTypeID<T>();

        auto it = m_componentArrays.find(typeID);
        if (it == m_componentArrays.end()) {
            return nullptr;
        }

        const auto* array = static_cast<const ComponentArrayWrapper<T>*>(it->second.get());
        return array->get(entityID);
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

        const auto* array = static_cast<const ComponentArrayWrapper<T>*>(it->second.get());
        return array->hasComponent(entityID);
    }

    /**
     * Get component array for iteration (type-erased)
     */
    template<typename T>
    ComponentArrayWrapper<T>* getComponentArray() {
        ComponentTypeID typeID = getComponentTypeID<T>();

        if (m_componentArrays.find(typeID) == m_componentArrays.end()) {
            return nullptr;
        }

        return static_cast<ComponentArrayWrapper<T>*>(m_componentArrays[typeID].get());
    }

    /**
     * Get the name of a component type
     */
    const char* getComponentTypeName(ComponentTypeID typeID) const {
        auto it = m_componentNames.find(typeID);
        if (it == m_componentNames.end()) {
            return "Unknown";
        }
        return it->second.c_str();
    }

    /**
     * Get all registered component type IDs
     */
    std::vector<ComponentTypeID> getRegisteredTypeIDs() const {
        std::vector<ComponentTypeID> ids;
        ids.reserve(m_componentArrays.size());
        for (const auto& [id, _] : m_componentArrays) {
            ids.push_back(id);
        }
        return ids;
    }

private:
    std::unordered_map<ComponentTypeID, std::unique_ptr<ComponentArrayWrapperBase>> m_componentArrays;
    std::unordered_map<ComponentTypeID, std::string> m_componentNames;
};

} // namespace ecs
