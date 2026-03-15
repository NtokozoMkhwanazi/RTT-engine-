#pragma once

#include "Entity.h"
#include "EntityManager.h"
#include "ComponentManager.h"
#include "System.h"
#include <vector>
#include <memory>
#include <algorithm>
#include <typeindex>
#include <unordered_map>

namespace ecs {

/**
 * World - Main ECS container that manages entities, components, and systems
 */
class World {
public:
    World() = default;
    ~World() { shutdown(); }
    
    // Prevent copying
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    
    // Allow moving
    World(World&&) = default;
    World& operator=(World&&) = default;
    
    /**
     * Initialize the world
     */
    void init() {
        m_initialized = true;
        
        // Initialize all systems
        for (auto& system : m_systems) {
            system->init();
        }
        
        // Register entity destruction callback
        m_entityManager.registerDestructionCallback(
            [this](EntityID entityID) {
                onEntityDestroyed(entityID);
            }
        );
    }
    
    /**
     * Shutdown the world
     */
    void shutdown() {
        // Shutdown all systems in reverse order
        for (auto it = m_systems.rbegin(); it != m_systems.rend(); ++it) {
            it->get()->shutdown();
        }
        m_systems.clear();
        
        // Destroy all entities
        m_entityManager.destroyAll();
        
        m_initialized = false;
    }
    
    /**
     * Update the world
     */
    void update(float deltaTime) {
        if (!m_initialized) return;
        
        // Set managers for all systems
        for (auto& system : m_systems) {
            auto* typedSystem = dynamic_cast<TypedSystemBase*>(system.get());
            if (typedSystem) {
                typedSystem->setManagers(&m_entityManager, &m_componentManager);
            }
        }
        
        // Update all systems in priority order
        for (auto& system : m_systems) {
            if (system->isEnabled()) {
                system->update(deltaTime);
            }
        }
    }
    
    /**
     * Render the world
     */
    void render() {
        if (!m_initialized) return;
        
        // Render all systems
        for (auto& system : m_systems) {
            if (system->isEnabled()) {
                system->render();
            }
        }
    }
    
    /**
     * Create a new entity
     */
    Entity createEntity() {
        return m_entityManager.createEntity();
    }
    
    /**
     * Destroy an entity
     */
    void destroyEntity(Entity entity) {
        m_entityManager.destroyEntity(entity);
    }
    
    /**
     * Check if an entity is alive
     */
    bool isAlive(Entity entity) const {
        return m_entityManager.isAlive(entity.id);
    }
    
    /**
     * Add a component to an entity
     */
    template<typename T, typename... Args>
    T& addComponent(Entity entity, Args&&... args) {
        static_assert(std::is_base_of_v<Component, T>, "T must inherit from Component");
        
        T& component = m_componentManager.addComponent<T>(
            entity.id, 
            std::forward<Args>(args)...
        );
        
        // Update entity signature
        m_entityManager.addComponentType<T>(entity.id);
        
        return component;
    }
    
    /**
     * Remove a component from an entity
     */
    template<typename T>
    void removeComponent(Entity entity) {
        m_componentManager.removeComponent<T>(entity.id);
        m_entityManager.removeComponentType<T>(entity.id);
    }
    
    /**
     * Get a component from an entity
     */
    template<typename T>
    T* getComponent(Entity entity) {
        return m_componentManager.getComponent<T>(entity.id);
    }
    
    template<typename T>
    const T* getComponent(Entity entity) const {
        return m_componentManager.getComponent<T>(entity.id);
    }
    
    /**
     * Check if an entity has a component
     */
    template<typename T>
    bool hasComponent(Entity entity) const {
        return m_componentManager.hasComponent<T>(entity.id);
    }
    
    /**
     * Add a system to the world
     */
    template<typename T, typename... Args>
    T& addSystem(Args&&... args) {
        static_assert(std::is_base_of_v<System, T>, "T must inherit from System");
        
        auto system = std::make_unique<T>(std::forward<Args>(args)...);
        T* rawPtr = system.get();
        
        m_systems.push_back(std::move(system));
        
        // Initialize the system if world is already initialized
        if (m_initialized) {
            rawPtr->init();
        }
        
        return *rawPtr;
    }
    
    /**
     * Get a system by type
     */
    template<typename T>
    T* getSystem() {
        for (auto& system : m_systems) {
            if (typeid(*system) == typeid(T)) {
                return static_cast<T*>(system.get());
            }
        }
        return nullptr;
    }
    
    template<typename T>
    const T* getSystem() const {
        for (const auto& system : m_systems) {
            if (typeid(*system) == typeid(T)) {
                return static_cast<const T*>(system.get());
            }
        }
        return nullptr;
    }
    
    /**
     * Get the entity manager
     */
    EntityManager& getEntityManager() { return m_entityManager; }
    const EntityManager& getEntityManager() const { return m_entityManager; }
    
    /**
     * Get the component manager
     */
    ComponentManager& getComponentManager() { return m_componentManager; }
    const ComponentManager& getComponentManager() const { return m_componentManager; }
    
    /**
     * Get entity count
     */
    size_t getEntityCount() const {
        return m_entityManager.getLiveEntityCount();
    }
    
    /**
     * Get system count
     */
    size_t getSystemCount() const {
        return m_systems.size();
    }
    
    /**
     * Check if world is initialized
     */
    bool isInitialized() const { return m_initialized; }

private:
    /**
     * Called when an entity is destroyed
     */
    void onEntityDestroyed(EntityID entityID) {
        // Components are automatically cleaned up by ComponentManager
        // when entities are destroyed via the destruction callback
    }
    
    EntityManager m_entityManager;
    ComponentManager m_componentManager;
    std::vector<std::unique_ptr<System>> m_systems;
    bool m_initialized = false;
};

} // namespace ecs
