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
#include <fstream>
#include <sstream>

namespace ecs {

/**
 * Enhanced World - Main ECS container with advanced features
 * 
 * Features:
 * - Archetype-based storage for cache-coherent iteration
 * - Multi-threaded system execution
 * - Entity hierarchies (parent/child)
 * - Event system for component changes
 * - Serialization (JSON format)
 * - Blueprint/prefab system
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
            // Parallel execution using job system
            updateParallel(deltaTime);
        } else {
            // Sequential execution
            updateSequential(deltaTime);
        }
    }

    /**
     * Render the world
     */
    void render() {
        if (!m_initialized) return;
        
        // Render all systems (rendering is typically GPU-bound, so sequential is fine)
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
        ComponentAddedEvent<T> event;
        event.entityID = entity.id;
        event.component = &component;
        m_eventSystem.publish(event);
        
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

    // ========================================================================
    // Archetype-based Operations (New, cache-coherent)
    // ========================================================================

    /**
     * Create an entity with components using archetype storage
     */
    template<typename... Components>
    Entity createEntityWithComponents(Components&&... components) {
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
     * Iterate over all entities with specific components (archetype-based)
     * This provides cache-coherent iteration
     */
    template<typename... Components>
    void forEach(std::function<void(EntityID, Components&...)> callback) {
        m_archetypeManager.forEach<Components...>(callback);
    }

    template<typename... Components>
    void forEach(std::function<void(EntityID, const Components&...)> callback) const {
        m_archetypeManager.forEach<Components...>(callback);
    }

    /**
     * Iterate with entity handle instead of ID
     */
    template<typename... Components>
    void forEachEntity(std::function<void(Entity, Components&...)> callback) {
        m_archetypeManager.forEach<Components...>([callback](EntityID id, Components&... comps) {
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

    // ========================================================================
    // Entity Relationship Operations
    // ========================================================================

    /**
     * Set the parent of an entity
     */
    void setParent(Entity childEntity, Entity parentEntity, bool maintainWorldTransform = true) {
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
     * Publish an event
     */
    void publishEvent(const Event& event) {
        m_eventSystem.publish(event);
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
    std::string serializeToString() const {
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

    /**
     * Save the world to a file
     */
    bool saveToFile(const std::string& filename) const {
        WorldSerializer<ComponentManager, EntityManager> serializer(
            m_componentManager, m_entityManager);
        return serializer.saveToFile(filename);
    }

    /**
     * Load the world from a file
     */
    bool loadFromFile(const std::string& filename) {
        WorldSerializer<ComponentManager, EntityManager> serializer(
            m_componentManager, m_entityManager);
        DeserializeContext context;
        
        if (!serializer.loadFromFile(filename, context)) {
            return false;
        }
        
        return true;
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
     * Instantiate a blueprint
     */
    BlueprintInstance instantiateBlueprint(const std::string& name,
                                            const glm::vec3& position = glm::vec3(0),
                                            const glm::quat& rotation = glm::quat(1, 0, 0, 0),
                                            const glm::vec3& scale = glm::vec3(1)) {
        BlueprintInstance instance;
        instance.blueprint = getBlueprint(name);
        instance.instanceName = name;
        instance.position = position;
        instance.rotation = rotation;
        instance.scale = scale;
        
        if (!instance.blueprint) {
            return instance;
        }
        
        // Create entities from blueprint
        const auto& entities = instance.blueprint->getEntities();
        instance.entities.reserve(entities.size());
        
        for (const auto& bpEntity : entities) {
            Entity entity = createEntity();
            
            // Set name if available
            if (!bpEntity.name.empty()) {
                // Would need NameComponent to be available
            }
            
            instance.entities.push_back(entity.id);
        }
        
        m_blueprintInstances.push_back(instance);
        return instance;
    }

    /**
     * Destroy a blueprint instance
     */
    void destroyBlueprintInstance(BlueprintInstance& instance) {
        for (EntityID entityID : instance.entities) {
            destroyEntity(Entity{entityID});
        }
        instance.entities.clear();
        instance.blueprint = nullptr;
        
        m_blueprintInstances.erase(
            std::remove_if(m_blueprintInstances.begin(), m_blueprintInstances.end(),
                [&instance](const BlueprintInstance& inst) {
                    return &inst == &instance;
                }),
            m_blueprintInstances.end()
        );
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

    // ========================================================================
    // Managers Access
    // ========================================================================

    EntityManager& getEntityManager() { return m_entityManager; }
    const EntityManager& getEntityManager() const { return m_entityManager; }

    ComponentManager& getComponentManager() { return m_componentManager; }
    const ComponentManager& getComponentManager() const { return m_componentManager; }

    RelationshipManager& getRelationshipManager() { return m_relationshipManager; }
    const RelationshipManager& getRelationshipManager() const { return m_relationshipManager; }

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
        return 0;  // Would need to expose from archetype manager
    }

    size_t getBlueprintCount() const {
        return m_blueprints.size();
    }

    size_t getBlueprintInstanceCount() const {
        return m_blueprintInstances.size();
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
            }, static_cast<uint32_t>(-priority));  // Higher priority = lower number
            
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
    }

    // Core managers
    EntityManager m_entityManager;
    ComponentManager m_componentManager;
    std::vector<std::unique_ptr<System>> m_systems;
    
    // Enhanced features
    ArchetypeManager<> m_archetypeManager;
    JobSystem m_jobSystem;
    RelationshipManager m_relationshipManager;
    EventSystem m_eventSystem;
    
    // Blueprints
    std::unordered_map<std::string, std::unique_ptr<Blueprint>> m_blueprints;
    std::vector<BlueprintInstance> m_blueprintInstances;
    
    // Configuration
    bool m_parallelExecution = false;
    bool m_initialized = false;
};

} // namespace ecs
