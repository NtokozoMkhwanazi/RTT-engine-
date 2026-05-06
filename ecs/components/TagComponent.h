#pragma once

#include "../ECS.h"
#include "../DynamicTypes.h"
#include <string>
#include <vector>
#include <algorithm>

namespace ecs {

constexpr size_t MAX_NAME_LENGTH = 64;
constexpr size_t MAX_TAG_LENGTH = 32;

struct NameComponent : public Component {
    char name[MAX_NAME_LENGTH];
    
    NameComponent() { name[0] = '\0'; }
    NameComponent(const std::string& n) { setName(n); }
    
    void setName(const std::string& n) {
        std::strncpy(name, n.c_str(), MAX_NAME_LENGTH - 1);
        name[MAX_NAME_LENGTH - 1] = '\0';
    }
    
    const char* getName() const { return name; }
    bool hasName() const { return name[0] != '\0'; }
};

struct TagComponent : public Component {
    char tag[MAX_TAG_LENGTH];
    
    TagComponent() { tag[0] = '\0'; }
    TagComponent(const std::string& t) { setTag(t); }
    
    void setTag(const std::string& t) {
        std::strncpy(tag, t.c_str(), MAX_TAG_LENGTH - 1);
        tag[MAX_TAG_LENGTH - 1] = '\0';
    }
    
    const char* getTag() const { return tag; }
    bool hasTag() const { return tag[0] != '\0'; }
    
    bool operator==(const TagComponent& other) const {
        return std::strcmp(tag, other.tag) == 0;
    }
};

struct TagsComponent : public Component {
    char tags[8][MAX_TAG_LENGTH];
    size_t count = 0;
    
    TagsComponent() = default;
    
    void addTag(const std::string& tag) {
        if (count >= 8) return;
        if (hasTag(tag)) return;
        std::strncpy(tags[count], tag.c_str(), MAX_TAG_LENGTH - 1);
        tags[count][MAX_TAG_LENGTH - 1] = '\0';
        count++;
    }
    
    void removeTag(const std::string& tag) {
        for (size_t i = 0; i < count; i++) {
            if (std::strcmp(tags[i], tag.c_str()) == 0) {
                for (size_t j = i; j < count - 1; j++) {
                    std::strcpy(tags[j], tags[j + 1]);
                }
                count--;
                return;
            }
        }
    }
    
    bool hasTag(const std::string& tag) const {
        for (size_t i = 0; i < count; i++) {
            if (std::strcmp(tags[i], tag.c_str()) == 0) {
                return true;
            }
        }
        return false;
    }
    
    void clearTags() { count = 0; }
    bool empty() const { return count == 0; }
    size_t size() const { return count; }
};

struct ParentComponent : public Component {
    Entity parent{INVALID_ENTITY_ID};
    
    ParentComponent() = default;
    ParentComponent(Entity p) : parent(p) {}
};

struct ChildrenComponent : public Component {
    Entity children[16];
    size_t count = 0;
    
    ChildrenComponent() = default;
    
    void addChild(Entity child) {
        if (count >= 16) return;
        if (hasChild(child)) return;
        children[count++] = child;
    }
    
    void removeChild(Entity child) {
        for (size_t i = 0; i < count; i++) {
            if (children[i] == child) {
                for (size_t j = i; j < count - 1; j++) {
                    children[j] = children[j + 1];
                }
                count--;
                return;
            }
        }
    }
    
    bool hasChild(Entity child) const {
        for (size_t i = 0; i < count; i++) {
            if (children[i] == child) return true;
        }
        return false;
    }
    
    bool empty() const { return count == 0; }
    size_t size() const { return count; }
};

struct LifetimeComponent : public Component {
    float lifetime = 1.0f;
    float elapsed = 0.0f;
    bool destroyOnExpire = true;
    
    LifetimeComponent() = default;
    LifetimeComponent(float life, bool destroy = true) 
        : lifetime(life), destroyOnExpire(destroy) {}
    
    bool update(float deltaTime) {
        elapsed += deltaTime;
        return elapsed >= lifetime;
    }
    
    float getRemaining() const {
        return std::max(0.0f, lifetime - elapsed);
    }
    
    float getNormalized() const {
        if (lifetime <= 0.0f) return 0.0f;
        return 1.0f - (elapsed / lifetime);
    }
};

struct ActiveComponent : public Component {
    bool active = true;
    
    ActiveComponent() = default;
    ActiveComponent(bool a) : active(a) {}
};

} // namespace ecs
