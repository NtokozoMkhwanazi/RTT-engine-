#pragma once
#include <unordered_map>
#include <typeindex>
#include <memory>
#include <string>
#include "Component.h"

class Entity {
public:
    using Id = uint32_t;
    Entity(Id id = 0) : id(id) {}

    template<typename T, typename... Args>
    T* addComponent(Args&&... args) {
        auto ti = std::type_index(typeid(T));
        auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = static_cast<T*>(ptr.get());
        components[ti] = std::move(ptr);
        return raw;
    }

    template<typename T>
    T* getComponent() {
        auto ti = std::type_index(typeid(T));
        auto it = components.find(ti);
        if (it == components.end()) return nullptr;
        return static_cast<T*>(it->second.get());
    }

    template<typename T>
    bool has() const {
        return components.find(std::type_index(typeid(T))) != components.end();
    }

    Id id;
    std::string name;

private:
    std::unordered_map<std::type_index, ComponentPtr> components;
};
