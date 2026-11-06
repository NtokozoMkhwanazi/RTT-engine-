#pragma once
/**
 * blueprint_system.h
 *
 * Bridges the ecs::World's blueprint registry with the editor's authoring UI.
 * Provides:
 *   - Blueprint instantiation: create live entities from a registered Blueprint
 *     by applying each BlueprintEntity's ComponentOverrides and wiring up
 *     parent-child relationships.
 *   - Entity -> Blueprint capture: walk an entity hierarchy (root + children)
 *     and serialize each component's fields into ComponentOverride data,
 *     producing a reusable Blueprint asset (the "flesh out" requested by
 *     forUI.txt line 228).
 *
 * This replaces the stub `BlueprintManager::createBlueprintFromEntity` and the
 * limited `EntityManager::SpawnBlueprint` / `CreateBlueprintFromSelected`
 * with a single, general-purpose system that handles every serialisable
 * component type.
 */

#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "ecs/ECS.h"          // ecs::World, ecs::Entity, ecs::EntityID, INVALID_ENTITY_ID
#include "ecs/Blueprint.h"    // ecs::Blueprint, ecs::BlueprintEntity, ecs::BlueprintInstance, ComponentOverride

namespace Editor {

/**
 * BlueprintSystem — wraps ecs::World's blueprint registry and adds
 * instantiation + entity-capture capabilities used by the editor UI.
 */
class BlueprintSystem {
public:
    explicit BlueprintSystem(ecs::World& world);

    // --- Blueprint registry (delegates to ecs::World) ---

    /** True when a blueprint named `name` is registered. */
    bool hasBlueprint(const std::string& name) const;

    /** Register (or replace) a blueprint. Stored by getName(). */
    void registerBlueprint(std::unique_ptr<ecs::Blueprint> blueprint);

    /** Look up a blueprint by name (mutable). */
    ecs::Blueprint* getBlueprint(const std::string& name);

    /** Look up a blueprint by name (read-only). */
    const ecs::Blueprint* getBlueprint(const std::string& name) const;

    /** All registered blueprint names. */
    std::vector<std::string> getBlueprintNames() const;

    // --- Instantiation ---

    /**
     * Instantiate a registered blueprint at a world position/orientation.
     * Creates one live entity per BlueprintEntity, applies every
     * ComponentOverride, offsets the root transform by (position, rotation,
     * scale), and re-establishes parent-child links.
     * @return BlueprintInstance whose entities vector is populated on success.
     */
    ecs::BlueprintInstance instantiate(
        const std::string& name,
        const glm::vec3&   position = glm::vec3(0),
        const glm::quat&   rotation = glm::quat(1, 0, 0, 0),
        const glm::vec3&   scale    = glm::vec3(1));

    // --- Entity → Blueprint capture ---

    /**
     * Walk the entity hierarchy rooted at `rootEntity` and serialize every
     * known component (Transform, Name, Model, Light, Mesh, Tag) into
     * ComponentOverrides on a BlueprintEntity, preserving parent-child links
     * via BlueprintEntity::parentIndex.
     * @return A ready-to-save Blueprint, or nullptr if the entity is dead.
     */
    std::unique_ptr<ecs::Blueprint> createBlueprintFromEntity(
        ecs::EntityID rootEntity,
        const std::string& name);

    // --- Manager accessor (forUI.txt API: editor.blueprintSystem().getManager()) ---
    BlueprintSystem&       getManager()       noexcept { return *this; }
    const BlueprintSystem& getManager() const noexcept { return *this; }

private:
    ecs::World& m_world;
};

} // namespace Editor
