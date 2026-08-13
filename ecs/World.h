#pragma once

#include "Entity.h"
#include "EntityManager.h"
#include "ComponentManager.h"
#include "System.h"
#include "ArchetypeManager.h"
#include "JobSystem.h"
#include "RelationshipManager.h"
#include "EventSystem.h"
#include "Serialization.h"
#include "Blueprint.h"
#include <vector>
#include <memory>
#include <algorithm>
#include <typeindex>
#include <unordered_map>
#include <functional>

namespace ecs {

/**
 * World - Main ECS container that manages entities, components, and systems
 * 
 * Enhanced with:
 * - Archetype-based storage for cache-coherent iteration
 * - Multi-threaded system execution
 * - Entity relationship queries (parent/child)
 * - Event system for component changes
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

        // Initialize managers
        m_archetypeManager.init();
        m_eventSystem.init();
        m_jobSystem.init();

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

        // Shutdown managers
        m_jobSystem.shutdown();
        m_eventSystem.shutdown();
        m_archetypeManager.shutdown();

        // Destroy all entities
        m_entityManager.destroyAll();
        m_relationshipManager.clear();

        m_initialized = false;
    }

    /**
     * Update the world
     */
    void update(float deltaTime) {
        if (!m_initialized) return;

        // Process events from previous frame
        m_eventSystem.processEvents();

        // Set managers for all systems
        for (auto& system : m_systems) {
            auto* typedSystem = dynamic_cast<TypedSystemBase*>(system.get());
            if (typedSystem) {
                typedSystem->setManagers(&m_entityManager, &m_componentManager);
            }
        }

        if (m_parallelExecution && m_jobSystem.getWorkerCount() > 0) {
            updateParallel(deltaTime);
        } else {
            updateSequential(deltaTime);
        }
    }

    /**
     * Render the world
     */
    void render() {
        if (!m_initialized) return;

        // Render all systems (rendering is typically GPU-bound)
        for (auto& system : m_systems) {
            if (system->isEnabled()) {
                system->render();
            }
        }
    }

    // ========================================================================
    // Entity Operations
    // ========================================================================

    /**
     * Create a new entity
     */
    Entity createEntity() {
        Entity entity = m_entityManager.createEntity();
        
        // Publish event
        EntityCreatedEvent event;
        event.entityID = entity.id;
        m_eventSystem.publish(event);
        
        return entity;
    }

    /**
     * Destroy an entity
     */
    void destroyEntity(Entity entity) {
        if (!entity.isValid()) return;
        
        // Publish event before destruction
        EntityDestroyedEvent event;
        event.entityID = entity.id;
        m_eventSystem.publish(event);
        
        // Remove from relationship manager
        m_relationshipManager.removeEntity(entity.id);
        
        // Remove from archetype storage so stale component data can't be
        // rediscovered by name/forEach after destruction.
        m_archetypeManager.removeEntity(entity.id);
        
        m_entityManager.destroyEntity(entity);
    }

    /**
     * Check if an entity is alive
     */
    bool isAlive(Entity entity) const {
        return m_entityManager.isAlive(entity.id);
    }

    // ========================================================================
    // Component Operations (Legacy signature-based)
    // ========================================================================

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

        // Publish event
        ComponentAddedEvent<T> addedEvent;
        addedEvent.entityID = entity.id;
        addedEvent.component = &component;
        m_eventSystem.publish(addedEvent);

        return component;
    }

    /**
     * Remove a component from an entity
     */
    template<typename T>
    void removeComponent(Entity entity) {
        // Publish event before removal
        ComponentRemovedEvent<T> event;
        event.entityID = entity.id;
        m_eventSystem.publish(event);
        
        m_componentManager.removeComponent<T>(entity.id);
        m_entityManager.removeComponentType<T>(entity.id);
    }

    /**
     * Get a component from an entity
     */
    template<typename T>
    T* getComponent(Entity entity) {
        if (T* c = m_componentManager.getComponent<T>(entity.id)) return c;
        // Archetype storage is the primary path for createEntityWithComponents;
        // fall back to it so archetype-created entities expose their components
        // through the legacy getComponent API as well.
        return m_archetypeManager.getComponent<T>(entity.id);
    }

    template<typename T>
    const T* getComponent(Entity entity) const {
        if (const T* c = m_componentManager.getComponent<T>(entity.id)) return c;
        return m_archetypeManager.getComponent<T>(entity.id);
    }

    /**
     * Check if an entity has a component
     */
    template<typename T>
    bool hasComponent(Entity entity) const {
        if (m_componentManager.hasComponent<T>(entity.id)) return true;
        // Archetype storage is the primary path for createEntityWithComponents;
        // fall back to it so archetype-created entities report their components.
        return m_archetypeManager.hasComponent<T>(entity.id);
    }

    // ========================================================================
    // Archetype-based Operations (New, cache-coherent)
    // ========================================================================

    /**
     * Create an entity with components using archetype storage
     */
    template<typename... Components>
    Entity createEntityWithComponents() {
        Entity entity = m_entityManager.createEntity();
        
        // Create entity in archetype manager
        m_archetypeManager.createEntity<Components...>(entity.id);
        
        // Update signature for legacy compatibility
        (m_entityManager.addComponentType<std::remove_reference_t<Components>>(entity.id), ...);
        
        // Publish event
        EntityCreatedEvent event;
        event.entityID = entity.id;
        m_eventSystem.publish(event);
        
        return entity;
    }

    /**
     * Add a component using archetype storage
     */
    template<typename T, typename... Args>
    T& addComponentArchetype(Entity entity, Args&&... args) {
        static_assert(std::is_base_of_v<Component, T>, "T must inherit from Component");
        
        T& component = m_archetypeManager.addComponent<T>(
            entity.id,
            std::forward<Args>(args)...
        );
        
        // Update signature for legacy compatibility
        m_entityManager.addComponentType<T>(entity.id);
        
        // Publish event
        ComponentAddedEvent<T> event;
        event.entityID = entity.id;
        event.component = &component;
        m_eventSystem.publish(event);
        
        return component;
    }

    /**
     * Get a component from archetype storage
     */
    template<typename T>
    T* getComponentArchetype(Entity entity) {
        return m_archetypeManager.getComponent<T>(entity.id);
    }

    template<typename T>
    const T* getComponentArchetype(Entity entity) const {
        return m_archetypeManager.getComponent<T>(entity.id);
    }

    /**
     * Iterate over all entities with specific components (archetype-based)
     * This provides cache-coherent iteration
     */
    template<typename... Components, typename Func>
    void forEach(Func&& callback) {
        m_archetypeManager.forEach<Components...>(std::forward<Func>(callback));
    }

    /**
     * Iterate with entity handle instead of ID
     */
    template<typename... Components, typename Func>
    void forEachEntity(Func&& callback) {
        m_archetypeManager.forEach<Components...>([callback = std::forward<Func>(callback)](EntityID id, Components&... comps) mutable {
            callback(Entity{id}, comps...);
        });
    }

    // ========================================================================
    // System Operations
    // ========================================================================

    /**
     * Add a system to the world
     */
    template<typename T, typename... Args>
    T& addSystem(Args&&... args) {
        static_assert(std::is_base_of_v<System, T>, "T must inherit from System");

        auto system = std::make_shared<T>(std::forward<Args>(args)...);
        T* rawPtr = system.get();

        m_systems.push_back(system);

        // Initialize the system if world is already initialized
        if (m_initialized) {
            rawPtr->init();
        }

        return *rawPtr;
    }

    /**
     * Add an existing system instance to the world (takes ownership)
     */
    template<typename T>
    void addSystem(T* system) {
        static_assert(std::is_base_of_v<System, T>, "T must inherit from System");
        if (!system) return;
        
        m_systems.push_back(std::shared_ptr<T>(system));
        
        // Initialize the system if world is already initialized
        if (m_initialized) {
            system->init();
        }
    }
    
    /**
     * Add a static/global system that won't be deleted on shutdown
     * The system pointer must outlive the world
     */
    template<typename T>
    void addStaticSystem(T* system) {
        static_assert(std::is_base_of_v<System, T>, "T must inherit from System");
        if (!system) return;
        
        struct StaticDeleter {
            void operator()(T* ptr) { /* do nothing - static systems outlive world */ }
        };
        
        m_systems.push_back(std::shared_ptr<T>(system, StaticDeleter()));
        
        // Initialize the system if world is already initialized
        if (m_initialized) {
            system->init();
        }
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

    // ========================================================================
    // Entity Relationship Operations
    // ========================================================================

    /**
     * Set the parent of an entity
     */
    void setParent(Entity childEntity, Entity parentEntity) {
        m_relationshipManager.setParent(childEntity.id, parentEntity.id);
    }

    /**
     * Remove the parent of an entity (orphan it)
     */
    void orphan(Entity entity) {
        m_relationshipManager.orphan(entity.id);
    }

    /**
     * Get the parent of an entity
     */
    Entity getParent(Entity entity) const {
        return Entity{m_relationshipManager.getParent(entity.id)};
    }

    /**
     * Get all children of an entity
     */
    std::vector<Entity> getChildren(Entity entity) const {
        const auto& children = m_relationshipManager.getChildren(entity.id);
        std::vector<Entity> result;
        result.reserve(children.size());
        for (auto childID : children) {
            result.push_back(Entity{childID});
        }
        return result;
    }

    /**
     * Check if an entity has a parent
     */
    bool hasParent(Entity entity) const {
        return m_relationshipManager.hasParent(entity.id);
    }

    /**
     * Check if an entity has children
     */
    bool hasChildren(Entity entity) const {
        return m_relationshipManager.hasChildren(entity.id);
    }

    /**
     * Get all descendants of an entity (depth-first)
     */
    std::vector<Entity> getDescendantsDFS(Entity entity) const {
        auto descendants = m_relationshipManager.getDescendantsDFS(entity.id);
        std::vector<Entity> result;
        result.reserve(descendants.size());
        for (auto id : descendants) {
            result.push_back(Entity{id});
        }
        return result;
    }

    /**
     * Get all descendants of an entity (breadth-first)
     */
    std::vector<Entity> getDescendantsBFS(Entity entity) const {
        auto descendants = m_relationshipManager.getDescendantsBFS(entity.id);
        std::vector<Entity> result;
        result.reserve(descendants.size());
        for (auto id : descendants) {
            result.push_back(Entity{id});
        }
        return result;
    }

    /**
     * Get the root entity of a hierarchy
     */
    Entity getRoot(Entity entity) const {
        return Entity{m_relationshipManager.getRoot(entity.id)};
    }

    /**
     * Check if an entity is a descendant of another
     */
    bool isDescendantOf(Entity entity, Entity potentialAncestor) const {
        return m_relationshipManager.isDescendantOf(entity.id, potentialAncestor.id);
    }

    /**
     * Check if an entity is an ancestor of another
     */
    bool isAncestorOf(Entity entity, Entity potentialAncestor) const {
        // entity is the candidate ancestor, potentialAncestor is the entity
        // being tested: entity is an ancestor of potentialAncestor iff
        // potentialAncestor is a descendant of entity.
        return m_relationshipManager.isDescendantOf(potentialAncestor.id, entity.id);
    }

    // ========================================================================
    // Event System Operations
    // ========================================================================

    /**
     * Subscribe to an event type
     */
    size_t subscribe(EventType eventType, EventListener listener, int priority = 0) {
        return m_eventSystem.subscribe(eventType, listener, priority);
    }

    /**
     * Subscribe to a component event
     */
    template<typename ComponentType>
    size_t subscribeComponentAdded(EventListener listener, int priority = 0) {
        return m_eventSystem.subscribeComponentAdded<ComponentType>(listener, priority);
    }

    template<typename ComponentType>
    size_t subscribeComponentRemoved(EventListener listener, int priority = 0) {
        return m_eventSystem.subscribeComponentRemoved<ComponentType>(listener, priority);
    }

    template<typename ComponentType>
    size_t subscribeComponentChanged(EventListener listener, int priority = 0) {
        return m_eventSystem.subscribeComponentChanged<ComponentType>(listener, priority);
    }

    /**
     * Subscribe once to an event
     */
    size_t subscribeOnce(EventType eventType, EventListener listener, int priority = 0) {
        return m_eventSystem.subscribeOnce(eventType, listener, priority);
    }

    /**
     * Publish an event
     */
    void publishEvent(const Event& event) {
        m_eventSystem.publish(event);
    }

    /**
     * Publish an event immediately (synchronously)
     */
    void publishEventImmediate(const Event& event) {
        m_eventSystem.publishImmediate(event);
    }

    /**
     * Get the event system
     */
    EventSystem& getEventSystem() { return m_eventSystem; }
    const EventSystem& getEventSystem() const { return m_eventSystem; }

    // ========================================================================
    // Serialization Operations
    // ========================================================================

    /**
     * Serialize the world to a JSON string
     */
    std::string serializeToString() {
        WorldSerializer<ComponentManager, EntityManager> serializer(
            m_componentManager, m_entityManager);
        return serializer.serializeToString();
    }

    /**
     * Deserialize the world from a JSON string
     */
    void deserializeFromString(const std::string& jsonString) {
        WorldSerializer<ComponentManager, EntityManager> serializer(
            m_componentManager, m_entityManager);
        DeserializeContext context;
        serializer.deserializeFromString(jsonString, context);
    }

    // ========================================================================
    // Blueprint Operations
    // ========================================================================

    /**
     * Register a blueprint
     */
    void registerBlueprint(std::unique_ptr<Blueprint> blueprint) {
        if (!blueprint) return;
        m_blueprints[blueprint->getName()] = std::move(blueprint);
    }

    /**
     * Unregister a blueprint
     */
    void unregisterBlueprint(const std::string& name) {
        m_blueprints.erase(name);
    }

    /**
     * Get a blueprint by name
     */
    Blueprint* getBlueprint(const std::string& name) {
        auto it = m_blueprints.find(name);
        return it != m_blueprints.end() ? it->second.get() : nullptr;
    }

    const Blueprint* getBlueprint(const std::string& name) const {
        auto it = m_blueprints.find(name);
        return it != m_blueprints.end() ? it->second.get() : nullptr;
    }

    /**
     * Get the names of all registered blueprints
     */
    std::vector<std::string> getBlueprintNames() const {
        std::vector<std::string> names;
        names.reserve(m_blueprints.size());
        for (const auto& [name, bp] : m_blueprints) {
            names.push_back(name);
        }
        return names;
    }

    // ========================================================================
    // Multi-threading Configuration
    // ========================================================================

    /**
     * Enable/disable parallel system execution
     */
    void setParallelExecution(bool enabled) {
        m_parallelExecution = enabled;
    }

    bool isParallelExecutionEnabled() const { return m_parallelExecution; }

    /**
     * Get the job system
     */
    JobSystem& getJobSystem() { return m_jobSystem; }
    const JobSystem& getJobSystem() const { return m_jobSystem; }

    /**
     * Add a job to the job system
     */
    JobSystem::JobHandle addJob(std::function<void()> func, uint32_t priority = 0) {
        return m_jobSystem.addJob(std::move(func), priority);
    }

    /**
     * Add a job with dependencies
     */
    JobSystem::JobHandle addJobWithDeps(
        std::function<void()> func,
        std::vector<JobSystem::JobHandle> dependencies,
        uint32_t priority = 0) {
        return m_jobSystem.addJobWithDeps(std::move(func), std::move(dependencies), priority);
    }

    /**
     * Wait for all jobs to complete
     */
    void waitForAllJobs() {
        m_jobSystem.waitForAll();
    }

    /**
     * Parallel for loop
     */
    void parallelFor(size_t begin, size_t end, std::function<void(size_t)> func) {
        m_jobSystem.parallelFor(begin, end, std::move(func));
    }

    // ========================================================================
    // Managers Access
    // ========================================================================

    EntityManager& getEntityManager() { return m_entityManager; }
    const EntityManager& getEntityManager() const { return m_entityManager; }

    ComponentManager& getComponentManager() { return m_componentManager; }
    const ComponentManager& getComponentManager() const { return m_componentManager; }

    RelationshipManager& getRelationshipManager() { return m_relationshipManager; }
    const RelationshipManager& getRelationshipManager() const { return m_relationshipManager; }

    ArchetypeManager<>& getArchetypeManager() { return m_archetypeManager; }
    const ArchetypeManager<>& getArchetypeManager() const { return m_archetypeManager; }

    // ========================================================================
    // Statistics
    // ========================================================================

    size_t getEntityCount() const {
        return m_entityManager.getLiveEntityCount();
    }

    size_t getSystemCount() const {
        return m_systems.size();
    }

    size_t getArchetypeCount() const {
        return m_archetypeManager.getArchetypeCount();
    }

    bool isInitialized() const { return m_initialized; }

private:
    void updateSequential(float deltaTime) {
        // Update all systems in priority order
        for (auto& system : m_systems) {
            if (system->isEnabled()) {
                system->update(deltaTime);
            }
        }
    }

    void updateParallel(float deltaTime) {
        // Group systems by priority for parallel execution
        std::unordered_map<int, std::vector<System*>> priorityGroups;
        
        for (auto& system : m_systems) {
            if (system->isEnabled()) {
                priorityGroups[system->getPriority()].push_back(system.get());
            }
        }
        
        // Execute each priority group in parallel
        std::vector<JobSystem::JobHandle> handles;
        
        for (auto& [priority, systems] : priorityGroups) {
            auto handle = m_jobSystem.addJob([=, &systems]() {
                for (auto* system : systems) {
                    system->update(deltaTime);
                }
            }, static_cast<uint32_t>(-priority));
            
            handles.push_back(std::move(handle));
        }
        
        // Wait for all groups to complete
        for (auto& handle : handles) {
            handle.wait();
        }
    }

    void onEntityDestroyed(EntityID entityID) {
        // Components are automatically cleaned up by ComponentManager
        m_relationshipManager.removeEntity(entityID);
        // Remove archetype storage so render systems stop drawing it
        m_archetypeManager.removeEntity(entityID);
    }

    // Core managers (legacy signature-based)
    EntityManager m_entityManager;
    ComponentManager m_componentManager;
    std::vector<std::shared_ptr<System>> m_systems;
    
    // Enhanced features
    ArchetypeManager<> m_archetypeManager;
    JobSystem m_jobSystem;
    RelationshipManager m_relationshipManager;
    EventSystem m_eventSystem;
    
    // Configuration
    bool m_parallelExecution = false;
    bool m_initialized = false;

    // Blueprints
    std::unordered_map<std::string, std::unique_ptr<Blueprint>> m_blueprints;
};

} // namespace ecs
