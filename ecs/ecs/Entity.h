#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <typeinfo>

namespace ecs {

/**
 * Entity ID type
 */
using EntityID = uint32_t;

/**
 * Invalid entity ID constant
 */
constexpr EntityID INVALID_ENTITY_ID = UINT32_MAX;

/**
 * Maximum number of entities
 */
constexpr EntityID MAX_ENTITIES = 10000;

/**
 * Entity handle - lightweight reference to an entity
 */
struct Entity {
    EntityID id = INVALID_ENTITY_ID;
    bool isValid() const { return id != INVALID_ENTITY_ID; }
    bool operator==(const Entity& other) const { return id == other.id; }
    bool operator!=(const Entity& other) const { return id != other.id; }
    bool operator<(const Entity& other) const { return id < other.id; }
};

/**
 * Component type index
 */
using ComponentTypeID = size_t;

/**
 * Get a unique type ID for each component type.
 * Returns the registered ID for type T. Must be called after registration.
 */
template<typename T>
ComponentTypeID getComponentTypeID();

/**
 * Register a component type and return its unique ID.
 * Must be called exactly once per type, ideally in a single translation unit.
 */
template<typename T>
ComponentTypeID registerComponentTypeID();

/**
 * Get component type name (for debugging)
 */
template<typename T>
inline const char* getComponentTypeName() {
    return typeid(T).name();
}

} // namespace ecs
