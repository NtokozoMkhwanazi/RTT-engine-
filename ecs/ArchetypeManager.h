#pragma once

#include "Archetype.h"
#include "Entity.h"
#include "Component.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <algorithm>

namespace ecs {

/**
 * Component Registry - Maps component type IDs to their sizes and alignment
 */
class ComponentRegistry {
public:
    using CreateFunc = void* (*)();
    using DestroyFunc = void (*)(void*);
    using CopyFunc = void (*)(void*, const void*);
    using MoveFunc = void (*)(void*, void*);

    struct ComponentInfo {
        size_t size;
        size_t alignment;
        CreateFunc create;
        DestroyFunc destroy;
        CopyFunc copy;
        MoveFunc move;
        const char* name;
    };

    /**
     * Register a component type
     */
    template<typename T>
    static void registerType() {
        ComponentTypeID typeID = getComponentTypeID<T>();
        
        auto it = s_registry.find(typeID);
        if (it != s_registry.end()) {
            return;  // Already registered
        }

        ComponentInfo info;
        info.size = sizeof(T);
        info.alignment = alignof(T);
        info.create = []() -> void* { return new T(); };
        info.destroy = [](void* ptr) { delete static_cast<T*>(ptr); };
        info.copy = [](void* dst, const void* src) { 
            new (dst) T(*static_cast<const T*>(src)); 
        };
        info.move = [](void* dst, void* src) {
            new (dst) T(std::move(*static_cast<T*>(src)));
        };
        info.name = typeid(T).name();

        s_registry[typeID] = info;
    }

    /**
     * Get component info
     */
    static const ComponentInfo& getInfo(ComponentTypeID typeID) {
        auto it = s_registry.find(typeID);
        if (it == s_registry.end()) {
            // Auto-register if not found
            // This is a fallback - types should be registered explicitly
            static ComponentInfo defaultInfo = []() {
                ComponentInfo info;
                info.size = 64;  // Default size
                info.alignment = 8;
                info.create = nullptr;
                info.destroy = nullptr;
                info.copy = nullptr;
                info.move = nullptr;
                info.name = "Unknown";
                return info;
            }();
            return defaultInfo;
        }
        return it->second;
    }

    /**
     * Get component size
     */
    static size_t getSize(ComponentTypeID typeID) {
        return getInfo(typeID).size;
    }

    /**
     * Get component alignment
     */
    static size_t getAlignment(ComponentTypeID typeID) {
        return getInfo(typeID).alignment;
    }

    /**
     * Get component name
     */
    static const char* getName(ComponentTypeID typeID) {
        return getInfo(typeID).name;
    }

    /**
     * Check if a type is registered
     */
    static bool isRegistered(ComponentTypeID typeID) {
        return s_registry.find(typeID) != s_registry.end();
    }

    /**
     * Clear all registrations (for cleanup)
     */
    static void clear() {
        s_registry.clear();
    }

private:
    static std::unordered_map<ComponentTypeID, ComponentInfo> s_registry;
};

// Static member definition
inline std::unordered_map<ComponentTypeID, ComponentRegistry::ComponentInfo> ComponentRegistry::s_registry;

/**
 * Archetype Manager - Manages all archetypes and entity-component storage
 * 
 * This replaces the signature-based ComponentManager with archetype-based storage
 * for better cache coherence and iteration performance.
 */
template<size_t ChunkSize = 256>
class ArchetypeManager {
public:
    using ArchetypeDataType = ArchetypeData<ChunkSize>;

    ArchetypeManager() = default;
    ~ArchetypeManager() = default;

    // Prevent copying
    ArchetypeManager(const ArchetypeManager&) = delete;
    ArchetypeManager& operator=(const ArchetypeManager&) = delete;

    /**
     * Initialize the manager
     */
    void init() {
        m_initialized = true;
    }

    /**
     * Shutdown the manager
     */
    void shutdown() {
        m_archetypes.clear();
        m_entityLocations.clear();
        m_initialized = false;
    }

    /**
     * Register a component type
     */
    template<typename T>
    void registerComponent() {
        ComponentRegistry::registerType<T>();
    }

    /**
     * Create an entity with initial components
     */
    template<typename... Components>
    EntityID createEntity(EntityID entityID) {
        // Build the archetype from component types
        Archetype::ComponentTypeList componentTypes = {
            getComponentTypeID<Components>()...
        };
        std::sort(componentTypes.begin(), componentTypes.end());
        
        Archetype archetype(componentTypes);
        
        // Get or create the archetype
        ArchetypeDataType* archetypeData = getOrCreateArchetype(archetype);
        
        // Add entity to the archetype
        auto [chunkIndex, indexInChunk] = archetypeData->addEntity(entityID);
        
        // Store entity location for fast lookup
        m_entityLocations[entityID] = EntityLocation{
            archetypeData,
            chunkIndex,
            indexInChunk
        };
        
        // Initialize components with default values
        initializeComponents<Components...>(entityID, chunkIndex, indexInChunk);
        
        return entityID;
    }

    /**
     * Add a component to an entity
     */
    template<typename T, typename... Args>
    T& addComponent(EntityID entityID, Args&&... args) {
        auto it = m_entityLocations.find(entityID);
        if (it == m_entityLocations.end()) {
            // Entity doesn't exist - create it with just this component
            return createEntityWithComponent<T>(entityID, std::forward<Args>(args)...);
        }
        
        EntityLocation& loc = it->second;
        ComponentTypeID typeID = getComponentTypeID<T>();
        
        // Check if the archetype already has this component
        if (loc.archetypeData->hasComponentType(typeID)) {
            // Component already exists, just get it
            return *getComponentPtr<T>(entityID);
        }
        
        // Need to move entity to a new archetype with the added component
        return moveEntityAndAddComponent<T>(entityID, std::forward<Args>(args)...);
    }

    /**
     * Remove a component from an entity
     */
    template<typename T>
    void removeComponent(EntityID entityID) {
        auto it = m_entityLocations.find(entityID);
        if (it == m_entityLocations.end()) {
            return;  // Entity doesn't exist
        }
        
        EntityLocation& loc = it->second;
        ComponentTypeID typeID = getComponentTypeID<T>();
        
        // Check if the archetype has this component
        if (!loc.archetypeData->hasComponentType(typeID)) {
            return;  // Component doesn't exist
        }
        
        // Need to move entity to a new archetype without this component
        moveEntityAndRemoveComponent<T>(entityID);
    }

    /**
     * Get a component from an entity
     */
    template<typename T>
    T* getComponent(EntityID entityID) {
        auto it = m_entityLocations.find(entityID);
        if (it == m_entityLocations.end()) {
            return nullptr;
        }
        
        return getComponentPtr<T>(entityID);
    }

    template<typename T>
    const T* getComponent(EntityID entityID) const {
        auto it = m_entityLocations.find(entityID);
        if (it == m_entityLocations.end()) {
            return nullptr;
        }
        
        return getComponentPtr<T>(entityID);
    }

    /**
     * Check if an entity has a component
     */
    template<typename T>
    bool hasComponent(EntityID entityID) const {
        auto it = m_entityLocations.find(entityID);
        if (it == m_entityLocations.end()) {
            return false;
        }
        
        return it->second.archetypeData->hasComponentType(getComponentTypeID<T>());
    }

    /**
     * Check if an entity has all specified components
     */
    template<typename... Components>
    bool hasComponents(EntityID entityID) const {
        auto it = m_entityLocations.find(entityID);
        if (it == m_entityLocations.end()) {
            return false;
        }
        
        return (hasComponent<Components>(entityID) && ...);
    }

    /**
     * Check if an entity has any of the specified components
     */
    template<typename... Components>
    bool hasAnyComponent(EntityID entityID) const {
        auto it = m_entityLocations.find(entityID);
        if (it == m_entityLocations.end()) {
            return false;
        }
        
        return (hasComponent<Components>(entityID) || ...);
    }

    /**
     * Remove an entity and all its components
     */
    void removeEntity(EntityID entityID) {
        auto it = m_entityLocations.find(entityID);
        if (it == m_entityLocations.end()) {
            return;
        }
        
        EntityLocation& loc = it->second;
        loc.archetypeData->removeEntity(entityID);
        m_entityLocations.erase(it);
    }

    /**
     * Iterate over all entities with specific components
     * This is the primary way systems access entities
     */
    template<typename... Components, typename Func>
    void forEach(Func&& callback) {
        Archetype::ComponentTypeList requiredTypes = {
            getComponentTypeID<Components>()...
        };
        std::sort(requiredTypes.begin(), requiredTypes.end());

        // Find all matching archetypes
        for (auto& [archetype, archetypeData] : m_archetypes) {
            if (!archetypeMatches(archetype, requiredTypes)) {
                continue;
            }

            // Iterate over all chunks in this archetype
            for (auto& chunk : archetypeData.getChunks()) {
                for (size_t i = chunk.begin(); i < chunk.end(); ++i) {
                    EntityID entityID = chunk.getEntity(i);

                    // Get all components as a tuple of pointers
                    auto comps = getComponentsFromChunk<Components...>(chunk, i);

                    // Apply callback with dereferenced components
                    std::apply([&callback, entityID](auto*... c) {
                        callback(entityID, *c...);
                    }, comps);
                }
            }
        }
    }

    /**
     * Get the number of archetypes
     */
    size_t getArchetypeCount() const {
        return m_archetypes.size();
    }

    /**
     * Check if the manager is initialized
     */
    bool isInitialized() const { return m_initialized; }

private:
    struct EntityLocation {
        ArchetypeDataType* archetypeData;
        size_t chunkIndex;
        size_t indexInChunk;
    };

    /**
     * Get or create an archetype
     */
    ArchetypeDataType* getOrCreateArchetype(const Archetype& archetype) {
        auto it = m_archetypes.find(archetype);
        if (it != m_archetypes.end()) {
            return &it->second;
        }
        
        auto [newIt, _] = m_archetypes.emplace(archetype, ArchetypeDataType(archetype));
        return &newIt->second;
    }

    /**
     * Check if an archetype matches required component types
     */
    bool archetypeMatches(const Archetype& archetype, 
                          const Archetype::ComponentTypeList& requiredTypes) const {
        for (auto typeID : requiredTypes) {
            if (!archetype.hasComponentType(typeID)) {
                return false;
            }
        }
        return true;
    }

    /**
     * Create an entity with a single component
     */
    template<typename T, typename... Args>
    T& createEntityWithComponent(EntityID entityID, Args&&... args) {
        Archetype::ComponentTypeList componentTypes = { getComponentTypeID<T>() };
        Archetype archetype(componentTypes);
        
        ArchetypeDataType* archetypeData = getOrCreateArchetype(archetype);
        auto [chunkIndex, indexInChunk] = archetypeData->addEntity(entityID);
        
        m_entityLocations[entityID] = EntityLocation{
            archetypeData,
            chunkIndex,
            indexInChunk
        };
        
        // Initialize the component
        T* comp = getComponentPtr<T>(entityID);
        new (comp) T(std::forward<Args>(args)...);
        return *comp;
    }

    /**
     * Move an entity to a new archetype and add a component
     */
    template<typename T, typename... Args>
    T& moveEntityAndAddComponent(EntityID entityID, Args&&... args) {
        auto it = m_entityLocations.find(entityID);
        EntityLocation& loc = it->second;
        
        // Get the new archetype
        Archetype::ComponentTypeList newComponentTypes = 
            loc.archetypeData->getArchetype().getWithAddedComponent(getComponentTypeID<T>());
        Archetype newArchetype(newComponentTypes);
        
        ArchetypeDataType* newArchetypeData = getOrCreateArchetype(newArchetype);
        auto [newChunkIndex, newIndexInChunk] = newArchetypeData->addEntity(entityID);
        
        // Copy all existing components to the new location
        copyComponentsToNewLocation(entityID, loc, newArchetypeData, newChunkIndex, newIndexInChunk);
        
        // Initialize the new component
        T* newComp = newArchetypeData->getChunks()[newChunkIndex].template getComponent<T>(newIndexInChunk);
        new (newComp) T(std::forward<Args>(args)...);
        
        // Remove from old location
        loc.archetypeData->getChunks()[loc.chunkIndex].removeAt(loc.indexInChunk);
        
        // Update entity location
        m_entityLocations[entityID] = EntityLocation{
            newArchetypeData,
            newChunkIndex,
            newIndexInChunk
        };
        
        // Clean up empty chunks/archetypes if needed
        cleanupEmptyArchetypes();
        
        return *newComp;
    }

    /**
     * Move an entity to a new archetype and remove a component
     */
    template<typename T>
    void moveEntityAndRemoveComponent(EntityID entityID) {
        auto it = m_entityLocations.find(entityID);
        EntityLocation& loc = it->second;
        
        // Get the new archetype
        Archetype::ComponentTypeList newComponentTypes = 
            loc.archetypeData->getArchetype().getWithRemovedComponent(getComponentTypeID<T>());
        
        // If no components left, just remove the entity
        if (newComponentTypes.empty()) {
            loc.archetypeData->getChunks()[loc.chunkIndex].removeAt(loc.indexInChunk);
            m_entityLocations.erase(it);
            cleanupEmptyArchetypes();
            return;
        }
        
        Archetype newArchetype(newComponentTypes);
        ArchetypeDataType* newArchetypeData = getOrCreateArchetype(newArchetype);
        auto [newChunkIndex, newIndexInChunk] = newArchetypeData->addEntity(entityID);
        
        // Copy all components except the removed one
        copyComponentsToNewLocation(entityID, loc, newArchetypeData, newChunkIndex, newIndexInChunk,
                                    getComponentTypeID<T>());
        
        // Remove from old location
        loc.archetypeData->getChunks()[loc.chunkIndex].removeAt(loc.indexInChunk);
        
        // Update entity location
        m_entityLocations[entityID] = EntityLocation{
            newArchetypeData,
            newChunkIndex,
            newIndexInChunk
        };
        
        // Clean up empty archetypes
        cleanupEmptyArchetypes();
    }

    /**
     * Copy components from old location to new location
     */
    void copyComponentsToNewLocation(EntityID entityID,
                                      EntityLocation& oldLoc,
                                      ArchetypeDataType* newArchetypeData,
                                      size_t newChunkIndex,
                                      size_t newIndexInChunk,
                                      ComponentTypeID excludeType = INVALID_ENTITY_ID) {
        auto& oldChunk = oldLoc.archetypeData->getChunks()[oldLoc.chunkIndex];
        auto& newChunk = newArchetypeData->getChunks()[newChunkIndex];
        
        for (auto typeID : oldChunk.getComponentTypes()) {
            if (typeID == excludeType) {
                continue;  // Skip the excluded component
            }
            
            // Get component info for proper copy
            const auto& info = ComponentRegistry::getInfo(typeID);
            
            void* oldComp = oldChunk.getComponentData(oldLoc.indexInChunk, typeID);
            void* newComp = newChunk.getComponentData(newIndexInChunk, typeID);
            
            if (info.copy) {
                info.copy(newComp, oldComp);
            } else {
                // Fallback to memcpy if no copy function registered
                std::memcpy(newComp, oldComp, info.size);
            }
        }
    }

    /**
     * Get component pointer for an entity
     */
    template<typename T>
    T* getComponentPtr(EntityID entityID) {
        auto it = m_entityLocations.find(entityID);
        if (it == m_entityLocations.end()) {
            return nullptr;
        }

        EntityLocation& loc = it->second;
        auto& chunk = loc.archetypeData->getChunks()[loc.chunkIndex];
        return chunk.template getComponent<T>(loc.indexInChunk);
    }

    template<typename T>
    const T* getComponentPtr(EntityID entityID) const {
        auto it = m_entityLocations.find(entityID);
        if (it == m_entityLocations.end()) {
            return nullptr;
        }

        const EntityLocation& loc = it->second;
        const auto& chunk = loc.archetypeData->getChunks()[loc.chunkIndex];
        return chunk.template getComponent<T>(loc.indexInChunk);
    }

    /**
     * Get multiple components from a chunk (non-const version)
     */
    template<typename... Components>
    auto getComponentsFromChunk(Chunk<ChunkSize>& chunk, size_t index) {
        return std::make_tuple(chunk.template getComponent<Components>(index)...);
    }

    /**
     * Get multiple components from a chunk (const version)
     */
    template<typename... Components>
    auto getComponentsFromChunk(const Chunk<ChunkSize>& chunk, size_t index) const {
        return std::make_tuple(chunk.template getComponent<Components>(index)...);
    }

    /**
     * Initialize components for a new entity
     */
    template<typename... Components>
    void initializeComponents(EntityID entityID, size_t chunkIndex, size_t indexInChunk) {
        // Components are default-initialized by the chunk
        // This function can be used for custom initialization if needed
    }

    /**
     * Clean up empty chunks and archetypes
     */
    void cleanupEmptyArchetypes() {
        // Remove empty chunks
        for (auto it = m_archetypes.begin(); it != m_archetypes.end(); ) {
            auto& archetypeData = it->second;
            auto& chunks = archetypeData.getChunks();
            
            chunks.erase(
                std::remove_if(chunks.begin(), chunks.end(),
                    [](const auto& chunk) { return chunk.size() == 0; }),
                chunks.end()
            );
            
            // Remove empty archetype
            if (chunks.empty()) {
                it = m_archetypes.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::unordered_map<Archetype, ArchetypeDataType, ArchetypeHash> m_archetypes;
    std::unordered_map<EntityID, EntityLocation> m_entityLocations;
    bool m_initialized = false;
};

} // namespace ecs
