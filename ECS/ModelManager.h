#pragma once
#include <unordered_map>
#include <memory>
#include <string>
#include "Model.h"

class ModelManager {
public:
    static ModelManager& instance() {
        static ModelManager mm;
        return mm;
    }

    std::shared_ptr<Model> load(const std::string& path) {
        auto it = cache.find(path);
        if (it != cache.end()) return it->second;
        auto m = std::make_shared<Model>(path);
        cache[path] = m;
        return m;
    }

private:
    std::unordered_map<std::string, std::shared_ptr<Model>> cache;
    ModelManager() = default;
};
