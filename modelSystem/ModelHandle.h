#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>

#include "Model.h"

namespace ModelSystem {

class ModelRegistry;

/**
 * ModelHandle - Safe handle for model references
 */
struct ModelHandle {
    uint32_t index = UINT32_MAX;
    uint32_t generation = 0;
    
    static constexpr uint32_t INVALID = UINT32_MAX;
    
    bool isValid() const { return index != INVALID && generation != 0; }
    bool operator==(const ModelHandle& other) const { 
        return index == other.index && generation == other.generation; 
    }
    bool operator!=(const ModelHandle& other) const { return !(*this == other); }
};

/**
 * ModelRegistry - Manages models with handles (singleton)
 */
class ModelRegistry {
public:
    struct Entry {
        Model* model = nullptr;
        uint32_t generation = 1;
        std::string path;
        bool inUse = false;
    };
    
    static ModelRegistry& getInstance() {
        static ModelRegistry inst;
        return inst;
    }
    
    ~ModelRegistry() {
        for (auto& e : m_entries) {
            delete e.model;
        }
    }
    
    ModelHandle load(const std::string& path) {
        if (path.empty()) return {};
        
        auto it = m_pathIndex.find(path);
        if (it != m_pathIndex.end()) {
            uint32_t idx = it->second;
            if (idx < m_entries.size() && m_entries[idx].inUse) {
                ModelHandle h;
                h.index = idx;
                h.generation = m_entries[idx].generation;
                return h;
            }
        }
        
        ModelHandle h;
        h.index = allocateSlot();
        h.generation = m_entries[h.index].generation;
        
        m_entries[h.index].model = new Model(path);
        m_entries[h.index].path = path;
        m_entries[h.index].inUse = true;
        
        m_pathIndex[path] = h.index;
        
        std::cout << "[ModelRegistry] Loaded: " << path << " handle=" << h.index << ":" << h.generation << "\n";
        return h;
    }
    
    ModelHandle createProcedural() {
        ModelHandle h;
        h.index = allocateSlot();
        h.generation = m_entries[h.index].generation;
        
        m_entries[h.index].model = new Model("");
        m_entries[h.index].path = "[procedural]";
        m_entries[h.index].inUse = true;
        
        return h;
    }
    
    Model* get(ModelHandle h) const {
        if (!h.isValid()) return nullptr;
        if (h.index >= m_entries.size()) return nullptr;
        if (h.generation != m_entries[h.index].generation) return nullptr;
        if (!m_entries[h.index].inUse) return nullptr;
        return m_entries[h.index].model;
    }
    
    void release(ModelHandle h) {
        if (!h.isValid()) return;
        if (h.index >= m_entries.size()) return;
        if (h.generation != m_entries[h.index].generation) return;
        
        m_entries[h.index].inUse = false;
        m_entries[h.index].generation++;
        delete m_entries[h.index].model;
        m_entries[h.index].model = nullptr;
        
        if (!m_entries[h.index].path.empty() && m_entries[h.index].path != "[procedural]") {
            m_pathIndex.erase(m_entries[h.index].path);
        }
        m_entries[h.index].path = "";
    }
    
    size_t getModelCount() const {
        size_t count = 0;
        for (const auto& e : m_entries) {
            if (e.inUse) count++;
        }
        return count;
    }
    
    void clear() {
        for (auto& e : m_entries) {
            delete e.model;
            e.model = nullptr;
            e.inUse = false;
        }
        m_entries.clear();
        m_pathIndex.clear();
    }
    
private:
    std::vector<Entry> m_entries;
    std::unordered_map<std::string, uint32_t> m_pathIndex;
    
    uint32_t allocateSlot() {
        for (uint32_t i = 0; i < m_entries.size(); i++) {
            if (!m_entries[i].inUse) {
                m_entries[i].inUse = true;
                return i;
            }
        }
        
        Entry e;
        e.generation = 1;
        e.inUse = true;
        m_entries.push_back(e);
        return static_cast<uint32_t>(m_entries.size() - 1);
    }
};

/**
 * MeshHandle - Safe handle for procedural meshes
 */
struct MeshHandle {
    uint32_t index = UINT32_MAX;
    uint32_t generation = 0;
    
    static constexpr uint32_t INVALID = UINT32_MAX;
    static constexpr uint32_t CUBE = 0;
    static constexpr uint32_t SPHERE = 1;
    static constexpr uint32_t PLANE = 2;
    static constexpr uint32_t CYLINDER = 3;
    static constexpr uint32_t CONE = 4;
    static constexpr uint32_t TORUS = 5;
    
    bool isValid() const { return index != INVALID; }
    bool operator==(const MeshHandle& other) const { return index == other.index; }
    bool operator!=(const MeshHandle& other) const { return index != other.index; }
};

/**
 * MeshRegistry - Manages procedural meshes
 */
class MeshRegistry {
public:
    struct Entry {
        uint32_t vao = 0;
        uint32_t vbo = 0;
        uint32_t ebo = 0;
        uint32_t indexCount = 0;
        uint32_t generation = 1;
        int meshType = -1;
    };
    
    MeshRegistry() = default;
    ~MeshRegistry() = default;
    
    MeshHandle registerMesh(int meshType, uint32_t vao, uint32_t vbo, uint32_t ebo, uint32_t indexCount) {
        MeshHandle h;
        h.index = static_cast<uint32_t>(m_entries.size());
        
        Entry e;
        e.vao = vao;
        e.vbo = vbo;
        e.ebo = ebo;
        e.indexCount = indexCount;
        e.meshType = meshType;
        m_entries.push_back(e);
        
        return h;
    }
    
    const Entry* get(MeshHandle h) const {
        if (!h.isValid()) return nullptr;
        if (h.index >= m_entries.size()) return nullptr;
        return &m_entries[h.index];
    }
    
private:
    std::vector<Entry> m_entries;
};

} // namespace ModelSystem