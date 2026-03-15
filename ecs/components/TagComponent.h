#pragma once

#include "../ECS.h"
#include <string>
#include <vector>
#include <algorithm>

namespace ecs {

/**
 * Name Component - Entity name for debugging
 */
struct NameComponent : public Component {
    std::string name;
    
    NameComponent() = default;
    NameComponent(const std::string& n) : name(n) {}
};

/**
 * Tag Component - Simple tag for entity categorization
 */
struct TagComponent : public Component {
    std::string tag;
    
    TagComponent() = default;
    TagComponent(const std::string& t) : tag(t) {}
    
    bool operator==(const TagComponent& other) const {
        return tag == other.tag;
    }
};

/**
 * Tags Component - Multiple tags per entity
 */
struct TagsComponent : public Component {
    std::vector<std::string> tags;
    
    TagsComponent() = default;
    
    void addTag(const std::string& tag) {
        if (!hasTag(tag)) {
            tags.push_back(tag);
        }
    }
    
    void removeTag(const std::string& tag) {
        tags.erase(std::remove(tags.begin(), tags.end(), tag), tags.end());
    }
    
    bool hasTag(const std::string& tag) const {
        return std::find(tags.begin(), tags.end(), tag) != tags.end();
    }
    
    void clearTags() {
        tags.clear();
    }
};

/**
 * Parent Component - Entity hierarchy
 */
struct ParentComponent : public Component {
    Entity parent{INVALID_ENTITY_ID};
    
    ParentComponent() = default;
    ParentComponent(Entity p) : parent(p) {}
};

/**
 * Children Component - List of child entities
 */
struct ChildrenComponent : public Component {
    std::vector<Entity> children;
    
    void addChild(Entity child) {
        children.push_back(child);
    }
    
    void removeChild(Entity child) {
        children.erase(
            std::remove(children.begin(), children.end(), child),
            children.end()
        );
    }
    
    bool hasChild(Entity child) const {
        return std::find(children.begin(), children.end(), child) != children.end();
    }
};

/**
 * Lifetime Component - Auto-destroy entity after time
 */
struct LifetimeComponent : public Component {
    float lifetime = 1.0f;
    float elapsed = 0.0f;
    bool destroyOnExpire = true;
    
    LifetimeComponent() = default;
    LifetimeComponent(float life, bool destroy = true) 
        : lifetime(life), destroyOnExpire(destroy) {}
    
    /**
     * Update lifetime
     * @return true if expired
     */
    bool update(float deltaTime) {
        elapsed += deltaTime;
        return elapsed >= lifetime;
    }
    
    /**
     * Get remaining lifetime
     */
    float getRemaining() const {
        return std::max(0.0f, lifetime - elapsed);
    }
    
    /**
     * Get normalized lifetime (0 = expired, 1 = fresh)
     */
    float getNormalized() const {
        if (lifetime <= 0.0f) return 0.0f;
        return 1.0f - (elapsed / lifetime);
    }
};

/**
 * Active Component - Enable/disable entity
 */
struct ActiveComponent : public Component {
    bool active = true;
    
    ActiveComponent() = default;
    ActiveComponent(bool a) : active(a) {}
};

} // namespace ecs
