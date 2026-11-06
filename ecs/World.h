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
#include <map>
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
     * Id-only destroy (Phase B helper): bare-id sites that only hold an EntityID
     * (undo snapshot, selected-entity) reconstruct a gen-aware Entity via
     * generation(id) and delegate. Behavior == destroyEntity(Entity).
     */
    void destroyEntity(EntityID entityID) {
        destroyEntity(Entity{entityID, m_entityManager.generation(entityID)});
    }

    /**
     * Gen-aware liveness for a full Entity handle (Phase B): the handle must
     * carry the slot's current generation, so a destroyed handle — or one whose
     * id was recycled to a later entity — is rejected even though the id is now
     * live again under a different gen. Drives the getComponent/hasComponent/
     * removeComponent guards and valid().
     */
    bool isAlive(Entity entity) const {
        if (!entity.isValid()) return false;
        return m_entityManager.isAlive(entity.id) &&
               m_entityManager.generation(entity.id) == entity.gen;
    }

    /**
     * Id-only liveness (Phase B helper): for call sites that only hold a bare
     * EntityID (selection id, undo snapshot id, serialized id, relationship link)
     * and want the pre-gen "is this id live" semantics. These sites are migrated
     * to this overload so they keep working for recycled entities instead of
     * being null'd by the gen-aware Entity overload above.
     */
    bool isAlive(EntityID entityID) const {
        return m_entityManager.isAlive(entityID);
    }

    /**
     * Slot generation for a bare id (Phase B): lets bare-id sites build a full
     * gen-aware Entity, e.g. World::getComponent(Entity{id, generation(id)}).
     */
    uint8_t generation(EntityID entityID) const {
        return m_entityManager.generation(entityID);
    }

    /**
     * Liveness check for an entity handle (Phase B). Entity::isValid() only
     * rejects the INVALID_ENTITY_ID sentinel; this ALSO rejects (a) a destroyed
     * entity (removed from the live set but not yet recycled) and (b) a stale
     * handle whose gen != the slot's current generation — i.e. the entity that
     * REUSED this id after destruction. Cheap: a hash-set lookup + a byte
     * compare. (Cheap gen-aware check on the hot getComponent path is deferred
     * behind the id-only isAlive() guard above; valid() is the strict, opt-in
     * disambiguation API for callers that hold a full Entity handle.)
     */
    bool valid(Entity entity) const {
        if (!entity.isValid()) return false;
        return m_entityManager.isAlive(entity.id) &&
               m_entityManager.generation(entity.id) == entity.gen;
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
        // Guard against dangling handles: a destroyed entity has no component to
        // remove and its id may have been recycled (see valid()). Early-out before
        // the ComponentManager path, whose remove() asserts on an absent entity.
        if (!isAlive(entity)) return;
        // Publish event before removal
        ComponentRemovedEvent<T> event;
        event.entityID = entity.id;
        m_eventSystem.publish(event);
        
        // #3 runtime sync: reconcile BOTH stores so removal is consistent regardless
        // of which store created the component. ArchetypeManager::removeComponent is
        // a no-op for entities it doesn't own; guard the ComponentManager call (its
        // ComponentArray::remove asserts if the entity isn't present) to avoid a crash
        // on archetype-only entities. No dual WRITES -> no duplicate-component hazard.
        if (m_componentManager.hasComponent<T>(entity.id)) {
            m_componentManager.removeComponent<T>(entity.id);
        }
        m_archetypeManager.removeComponent<T>(entity.id);
        m_entityManager.removeComponentType<T>(entity.id);
    }

    // Phase B id-only overload: bare-id sites that reconstruct ecs::Entity{id}
    // and then remove a component. Lenient (id-only liveness) == Phase-A behavior.
    template<typename T>
    void removeComponent(EntityID entityID) {
        if (!isAlive(entityID)) return;
        ComponentRemovedEvent<T> event;
        event.entityID = entityID;
        m_eventSystem.publish(event);
        if (m_componentManager.hasComponent<T>(entityID)) {
            m_componentManager.removeComponent<T>(entityID);
        }
        m_archetypeManager.removeComponent<T>(entityID);
        m_entityManager.removeComponentType<T>(entityID);
    }

    /**
     * Get a component from an entity
     */
    template<typename T>
    T* getComponent(Entity entity) {
        if (!isAlive(entity)) return nullptr;
        if (T* c = m_componentManager.getComponent<T>(entity.id)) return c;
        // Archetype storage is the primary path for createEntityWithComponents;
        // fall back to it so archetype-created entities expose their components
        // through the legacy getComponent API as well.
        return m_archetypeManager.getComponent<T>(entity.id);
    }

    template<typename T>
    const T* getComponent(Entity entity) const {
        if (!isAlive(entity)) return nullptr;
        if (const T* c = m_componentManager.getComponent<T>(entity.id)) return c;
        return m_archetypeManager.getComponent<T>(entity.id);
    }

    // --- Phase B id-only overloads (lenient: "is this bare id live") -------
    // Bare-id reconstruction sites (ecs::Entity{id}) are migrated to these so
    // they keep working for recycled entities instead of being nulled by the
    // gen-aware Entity overload above. Behavior == Phase-A id-only guard.
    template<typename T>
    T* getComponent(EntityID entityID) {
        if (!isAlive(entityID)) return nullptr;
        if (T* c = m_componentManager.getComponent<T>(entityID)) return c;
        return m_archetypeManager.getComponent<T>(entityID);
    }

    template<typename T>
    const T* getComponent(EntityID entityID) const {
        if (!isAlive(entityID)) return nullptr;
        if (const T* c = m_componentManager.getComponent<T>(entityID)) return c;
        return m_archetypeManager.getComponent<T>(entityID);
    }

    /**
     * Check if an entity has a component
     */
    template<typename T>
    bool hasComponent(Entity entity) const {
        if (!isAlive(entity)) return false;
        if (m_componentManager.hasComponent<T>(entity.id)) return true;
        // Archetype storage is the primary path for createEntityWithComponents;
        // fall back to it so archetype-created entities report their components.
        return m_archetypeManager.hasComponent<T>(entity.id);
    }

    template<typename T>
    bool hasComponent(EntityID entityID) const {
        if (!isAlive(entityID)) return false;
        if (m_componentManager.hasComponent<T>(entityID)) return true;
        return m_archetypeManager.hasComponent<T>(entityID);
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
        if (!isAlive(entity)) return nullptr;
        return m_archetypeManager.getComponent<T>(entity.id);
    }

    template<typename T>
    const T* getComponentArchetype(Entity entity) const {
        if (!isAlive(entity)) return nullptr;
        return m_archetypeManager.getComponent<T>(entity.id);
    }

    // Phase B id-only overloads (see getComponent id-only note).
    template<typename T>
    T* getComponentArchetype(EntityID entityID) {
        if (!isAlive(entityID)) return nullptr;
        return m_archetypeManager.getComponent<T>(entityID);
    }

    template<typename T>
    const T* getComponentArchetype(EntityID entityID) const {
        if (!isAlive(entityID)) return nullptr;
        return m_archetypeManager.getComponent<T>(entityID);
    }

    /**
     * Iterate over all entities with specific components (archetype-based)
     * This provides cache-coherent iteration
     */
    template<typename... Components, typename Func>
    void forEach(Func&& callback) {
        m_archetypeManager.forEach<Components...>(callback);
        // #3 runtime sync: also cover the legacy ComponentManager store (entities added
        // via World::addComponent). AM-owned entities are skipped by the helper
        // (ArchetypeManager::forEach above already visited them). No dual writes ->
        // no duplicate-component hazard.
        forEachLegacyComponents<Components...>([&callback](EntityID eid, Components&... comps) {
            callback(eid, comps...);
        });
    }

    /**
     * Iterate with entity handle instead of ID
     */
    template<typename... Components, typename Func>
    void forEachEntity(Func&& callback) {
        forEach<Components...>([callback = std::forward<Func>(callback)](EntityID id, Components&... comps) mutable {
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

    // Phase B id-only overload: bare-id call sites (undo_redo) reconstruct from
    // EntityID. setParent is id-based at RelationshipManager, so no gen check.
    void setParent(EntityID childID, EntityID parentID) {
        m_relationshipManager.setParent(childID, parentID);
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
        EntityID p = m_relationshipManager.getParent(entity.id);
        return Entity{p, m_entityManager.generation(p)};
    }

    // Phase B id-only overload: gen-stamp the returned parent so callers that
    // later gen-check it (valid/getComponent) keep working.
    Entity getParent(EntityID childID) const {
        EntityID p = m_relationshipManager.getParent(childID);
        return Entity{p, m_entityManager.generation(p)};
    }

    /**
     * Get all children of an entity
     */
    std::vector<Entity> getChildren(Entity entity) const {
        const auto& children = m_relationshipManager.getChildren(entity.id);
        std::vector<Entity> result;
        result.reserve(children.size());
        for (auto childID : children) {
            result.push_back(Entity{childID, m_entityManager.generation(childID)});
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
        EntityID r = m_relationshipManager.getRoot(entity.id);
        return Entity{r, m_entityManager.generation(r)};
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
            m_componentManager, m_entityManager, &m_archetypeManager);
        return serializer.serializeToString();
    }

    /**
     * Deserialize the world from a JSON string
     */
    void deserializeFromString(const std::string& jsonString) {
        WorldSerializer<ComponentManager, EntityManager> serializer(
            m_componentManager, m_entityManager, &m_archetypeManager);
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
        // Pipeline-safety model (TODO Fix 1): systems are bucketed by priority into
        // STRICTLY ORDERED blocks (std::map sorts ascending; per System::getPriority,
        // lower priority runs first), drained fully (barrier) before the next block
        // starts. Within a block, independent systems run as one job per system in
        // parallel. Each lambda captures its system pointer + deltaTime BY VALUE so no
        // job references updateParallel's stack — the old [=, &systems] capture was a
        // ref into the local map kept alive only by the removed outer wait: a latent
        // use-after-scope hazard. The per-group barrier also fixes cross-priority
        // ordering, which the old concurrent-groups model did not enforce.
        std::map<int, std::vector<System*>> priorityGroups;

        for (auto& system : m_systems) {
            if (system->isEnabled()) {
                priorityGroups[system->getPriority()].push_back(system.get());
            }
        }

        for (auto& [priority, systemsInGroup] : priorityGroups) {
            std::vector<JobSystem::JobHandle> groupHandles;
            groupHandles.reserve(systemsInGroup.size());

            for (auto* system : systemsInGroup) {
                // BY-VALUE capture: self-contained job (system ptr + dt copied in).
                auto handle = m_jobSystem.addJob([system, deltaTime]() {
                    system->update(deltaTime);
                }, static_cast<uint32_t>(-priority));
                groupHandles.push_back(std::move(handle));
            }

            // Barrier: all systems at this priority complete before the next
            // priority block starts.
            for (auto& handle : groupHandles) {
                handle.wait();
            }
        }
    }

    void onEntityDestroyed(EntityID entityID) {
        // Components are automatically cleaned up by ComponentManager
        m_relationshipManager.removeEntity(entityID);
        // Remove archetype storage so render systems stop drawing it
        m_archetypeManager.removeEntity(entityID);
    }

    // #3 runtime sync: read-path unification for the legacy ComponentManager store.
    // Lets forEach also see entities added via World::addComponent.
    template<typename C0, typename... Cs> struct FirstOfT { using T = C0; };

    template<typename... Components>
    bool allComponentManagerHas(EntityID entityID) const {
        return (m_componentManager.hasComponent<Components>(entityID) && ...);
    }

    template<typename... Components, typename Func>
    void forEachLegacyComponents(Func&& callback) {
        static_assert(sizeof...(Components) >= 1, "forEach requires at least one component type");
        using FirstT = typename FirstOfT<Components...>::T;

        ComponentArray<FirstT>* array = m_componentManager.getComponentArray<FirstT>();
        if (!array) return;  // no ComponentManager entities of FirstT registered

        for (FirstT& first : *array) {
            EntityID entityID = first.entity.id;
            if (m_archetypeManager.hasComponent<FirstT>(entityID)) continue;  // dedup AM
            if (!allComponentManagerHas<Components...>(entityID)) continue;   // require all Cs
            callback(entityID, *m_componentManager.getComponent<Components>(entityID)...);
        }
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
