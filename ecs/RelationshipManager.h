#pragma once

#include "Entity.h"
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <functional>

namespace ecs {

/**
 * Entity Relationship Component - Stores parent-child hierarchy information
 * 
 * This component enables entity hierarchies (scenes graphs) where
 * children inherit transforms from parents and can be queried together.
 */
struct RelationshipComponent {
    EntityID parent = INVALID_ENTITY_ID;
    std::vector<EntityID> children;
    
    /**
     * Check if this entity has a parent
     */
    bool hasParent() const { return parent != INVALID_ENTITY_ID; }
    
    /**
     * Check if this entity has any children
     */
    bool hasChildren() const { return !children.empty(); }
    
    /**
     * Get the number of children
     */
    size_t childCount() const { return children.size(); }
    
    /**
     * Add a child to this entity
     */
    void addChild(EntityID childID) {
        if (!hasChild(childID)) {
            children.push_back(childID);
        }
    }
    
    /**
     * Remove a child from this entity
     */
    void removeChild(EntityID childID) {
        children.erase(
            std::remove(children.begin(), children.end(), childID),
            children.end()
        );
    }
    
    /**
     * Check if this entity has a specific child
     */
    bool hasChild(EntityID childID) const {
        for (auto child : children) {
            if (child == childID) return true;
        }
        return false;
    }
    
    /**
     * Remove all children
     */
    void removeAllChildren() {
        children.clear();
    }
};

/**
 * Entity Relationship Manager - Manages parent-child hierarchies
 * 
 * Provides efficient queries for entity hierarchies including:
 * - Parent/child lookups
 * - Tree traversal (depth-first, breadth-first)
 * - Subtree operations
 * - Transform propagation
 */
class RelationshipManager {
public:
    RelationshipManager() = default;
    ~RelationshipManager() = default;

    // Prevent copying
    RelationshipManager(const RelationshipManager&) = delete;
    RelationshipManager& operator=(const RelationshipManager&) = delete;

    /**
     * Set the parent of an entity
     * @param entityID The entity to set the parent for
     * @param parentID The new parent (INVALID_ENTITY_ID to remove parent)
     */
    void setParent(EntityID entityID, EntityID parentID) {
        auto it = m_relationships.find(entityID);
        
        if (it == m_relationships.end()) {
            // Create new relationship
            RelationshipComponent rel;
            rel.parent = parentID;
            m_relationships[entityID] = rel;
        } else {
            it->second.parent = parentID;
        }
        
        // Update parent's children list
        if (parentID != INVALID_ENTITY_ID) {
            auto parentIt = m_relationships.find(parentID);
            if (parentIt == m_relationships.end()) {
                RelationshipComponent parentRel;
                parentRel.addChild(entityID);
                m_relationships[parentID] = parentRel;
            } else {
                parentIt->second.addChild(entityID);
            }
        }
        
        // Notify event listeners
        notifyParentChanged(entityID, parentID);
    }

    /**
     * Remove the parent of an entity (orphan it)
     */
    void orphan(EntityID entityID) {
        setParent(entityID, INVALID_ENTITY_ID);
    }

    /**
     * Get the parent of an entity
     */
    EntityID getParent(EntityID entityID) const {
        auto it = m_relationships.find(entityID);
        if (it == m_relationships.end()) {
            return INVALID_ENTITY_ID;
        }
        return it->second.parent;
    }

    /**
     * Get all children of an entity
     */
    const std::vector<EntityID>& getChildren(EntityID entityID) const {
        static const std::vector<EntityID> empty;
        auto it = m_relationships.find(entityID);
        if (it == m_relationships.end()) {
            return empty;
        }
        return it->second.children;
    }

    /**
     * Check if an entity has a parent
     */
    bool hasParent(EntityID entityID) const {
        auto it = m_relationships.find(entityID);
        return it != m_relationships.end() && it->second.parent != INVALID_ENTITY_ID;
    }

    /**
     * Check if an entity has children
     */
    bool hasChildren(EntityID entityID) const {
        auto it = m_relationships.find(entityID);
        return it != m_relationships.end() && !it->second.children.empty();
    }

    /**
     * Check if an entity is a descendant of another entity
     */
    bool isDescendantOf(EntityID entityID, EntityID potentialAncestor) const {
        EntityID current = entityID;
        while (current != INVALID_ENTITY_ID) {
            current = getParent(current);
            if (current == potentialAncestor) {
                return true;
            }
        }
        return false;
    }

    /**
     * Check if an entity is an ancestor of another entity
     */
    bool isAncestorOf(EntityID potentialAncestor, EntityID entityID) const {
        return isDescendantOf(entityID, potentialAncestor);
    }

    /**
     * Get all ancestors of an entity (from parent to root)
     */
    std::vector<EntityID> getAncestors(EntityID entityID) const {
        std::vector<EntityID> ancestors;
        EntityID current = getParent(entityID);
        
        while (current != INVALID_ENTITY_ID) {
            ancestors.push_back(current);
            current = getParent(current);
        }
        
        return ancestors;
    }

    /**
     * Get all descendants of an entity (depth-first traversal)
     */
    std::vector<EntityID> getDescendantsDFS(EntityID entityID) const {
        std::vector<EntityID> descendants;
        collectDescendantsDFS(entityID, descendants);
        return descendants;
    }

    /**
     * Get all descendants of an entity (breadth-first traversal)
     */
    std::vector<EntityID> getDescendantsBFS(EntityID entityID) const {
        std::vector<EntityID> descendants;
        collectDescendantsBFS(entityID, descendants);
        return descendants;
    }

    /**
     * Get the depth of an entity in the hierarchy (0 = root)
     */
    int getDepth(EntityID entityID) const {
        int depth = 0;
        EntityID current = getParent(entityID);
        
        while (current != INVALID_ENTITY_ID) {
            depth++;
            current = getParent(current);
        }
        
        return depth;
    }

    /**
     * Get the root entity of a hierarchy
     */
    EntityID getRoot(EntityID entityID) const {
        EntityID current = entityID;
        EntityID parent = getParent(current);
        
        while (parent != INVALID_ENTITY_ID) {
            current = parent;
            parent = getParent(current);
        }
        
        return current;
    }

    /**
     * Remove an entity from the relationship manager
     * Also removes it from parent's children list
     */
    void removeEntity(EntityID entityID) {
        // Remove from parent's children
        EntityID parentID = getParent(entityID);
        if (parentID != INVALID_ENTITY_ID) {
            auto it = m_relationships.find(parentID);
            if (it != m_relationships.end()) {
                it->second.removeChild(entityID);
            }
        }
        
        // Remove all children relationships
        auto it = m_relationships.find(entityID);
        if (it != m_relationships.end()) {
            // Orphan all children
            for (auto childID : it->second.children) {
                auto childIt = m_relationships.find(childID);
                if (childIt != m_relationships.end()) {
                    childIt->second.parent = INVALID_ENTITY_ID;
                }
            }
        }
        
        // Remove the entity's relationship
        m_relationships.erase(entityID);
    }

    /**
     * Clear all relationships
     */
    void clear() {
        m_relationships.clear();
    }

    /**
     * Get the number of entities with relationships
     */
    size_t getEntityCount() const {
        return m_relationships.size();
    }

    /**
     * Register a callback for parent changes
     */
    using ParentChangedCallback = std::function<void(EntityID entityID, EntityID newParentID)>;
    void addParentChangedCallback(ParentChangedCallback callback) {
        m_parentChangedCallbacks.push_back(std::move(callback));
    }

private:
    void collectDescendantsDFS(EntityID entityID, std::vector<EntityID>& out) const {
        auto it = m_relationships.find(entityID);
        if (it == m_relationships.end()) {
            return;
        }
        
        for (auto childID : it->second.children) {
            out.push_back(childID);
            collectDescendantsDFS(childID, out);
        }
    }

    void collectDescendantsBFS(EntityID entityID, std::vector<EntityID>& out) const {
        std::vector<EntityID> queue;
        auto it = m_relationships.find(entityID);
        if (it == m_relationships.end()) {
            return;
        }
        
        // Add all children to queue
        for (auto childID : it->second.children) {
            queue.push_back(childID);
        }
        
        // Process queue
        size_t index = 0;
        while (index < queue.size()) {
            EntityID current = queue[index++];
            out.push_back(current);
            
            // Add children of current to queue
            auto childIt = m_relationships.find(current);
            if (childIt != m_relationships.end()) {
                for (auto childID : childIt->second.children) {
                    queue.push_back(childID);
                }
            }
        }
    }

    void notifyParentChanged(EntityID entityID, EntityID newParentID) {
        for (auto& callback : m_parentChangedCallbacks) {
            callback(entityID, newParentID);
        }
    }

    std::unordered_map<EntityID, RelationshipComponent> m_relationships;
    std::vector<ParentChangedCallback> m_parentChangedCallbacks;
};

/**
 * Helper class for iterating over entity hierarchies
 */
class HierarchyIterator {
public:
    enum class Order {
        DepthFirst,
        BreadthFirst,
        PreOrder,    // Process parent before children
        PostOrder    // Process parent after children
    };

    HierarchyIterator(const RelationshipManager& relManager, EntityID root)
        : m_relManager(relManager)
        , m_root(root)
    {
    }

    /**
     * Iterate over all descendants using the specified order
     * @param callback Function called for each entity (returns false to stop)
     */
    void iterate(Order order, std::function<bool(EntityID)> callback) const {
        switch (order) {
            case Order::DepthFirst:
                iterateDFS(m_root, callback);
                break;
            case Order::BreadthFirst:
                iterateBFS(m_root, callback);
                break;
            case Order::PreOrder:
                iteratePreOrder(m_root, callback);
                break;
            case Order::PostOrder:
                iteratePostOrder(m_root, callback);
                break;
        }
    }

private:
    bool iterateDFS(EntityID entityID, std::function<bool(EntityID)>& callback) const {
        const auto& children = m_relManager.getChildren(entityID);
        
        for (auto childID : children) {
            if (!callback(childID)) return false;
            if (!iterateDFS(childID, callback)) return false;
        }
        
        return true;
    }

    bool iterateBFS(EntityID entityID, std::function<bool(EntityID)>& callback) const {
        std::vector<EntityID> queue;
        const auto& children = m_relManager.getChildren(entityID);
        
        for (auto childID : children) {
            queue.push_back(childID);
        }
        
        size_t index = 0;
        while (index < queue.size()) {
            EntityID current = queue[index++];
            if (!callback(current)) return false;
            
            const auto& childChildren = m_relManager.getChildren(current);
            for (auto childID : childChildren) {
                queue.push_back(childID);
            }
        }
        
        return true;
    }

    bool iteratePreOrder(EntityID entityID, std::function<bool(EntityID)>& callback) const {
        if (!callback(entityID)) return false;
        
        const auto& children = m_relManager.getChildren(entityID);
        for (auto childID : children) {
            if (!iteratePreOrder(childID, callback)) return false;
        }
        
        return true;
    }

    bool iteratePostOrder(EntityID entityID, std::function<bool(EntityID)>& callback) const {
        const auto& children = m_relManager.getChildren(entityID);
        for (auto childID : children) {
            if (!iteratePostOrder(childID, callback)) return false;
        }
        
        return callback(entityID);
    }

    const RelationshipManager& m_relManager;
    EntityID m_root;
};

} // namespace ecs
