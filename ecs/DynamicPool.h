#pragma once

#include "Entity.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <cstring>
#include <mutex>
#include <cassert>

namespace ecs {

/**
 * DynamicPool - Manages heap-allocated data for archetype components
 * 
 * Instead of storing std::vector/std::string directly in archetype chunks,
 * components store handles that index into this pool. This allows:
 * - Fixed-size archetype slots (no buffer overflows)
 * - Proper copy/move semantics for dynamic data
 * - Efficient memory management with arenas
 */
class DynamicPool {
public:
    using Handle = uint32_t;
    static constexpr Handle INVALID_HANDLE = 0;
    
    struct Entry {
        uint8_t* data = nullptr;
        size_t size = 0;
        size_t capacity = 0;
        bool isSmallString = false;
    };
    
    DynamicPool() {
        m_freeList.reserve(1024);
    }
    
    ~DynamicPool() {
        for (auto& entry : m_entries) {
            if (entry.data && !entry.isSmallString) {
                delete[] entry.data;
            }
        }
    }
    
    Handle allocate(size_t size) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        if (!m_freeList.empty()) {
            Handle h = m_freeList.back();
            m_freeList.pop_back();
            
            auto& entry = m_entries[h];
            if (entry.data && !entry.isSmallString) {
                delete[] entry.data;
            }
            
            entry.data = new uint8_t[size];
            entry.size = 0;
            entry.capacity = size;
            entry.isSmallString = false;
            
            return h;
        }
        
        Handle h = static_cast<Handle>(m_entries.size());
        Entry entry;
        entry.data = new uint8_t[size];
        entry.size = 0;
        entry.capacity = size;
        m_entries.push_back(entry);
        
        return h;
    }
    
    template<typename T>
    Handle allocateVector(const std::vector<T>& vec) {
        Handle h = allocate(sizeof(std::vector<T>));
        new (getData(h)) std::vector<T>(vec);
        return h;
    }
    
    template<typename T>
    Handle allocateVector(std::vector<T>&& vec) {
        Handle h = allocate(sizeof(std::vector<T>));
        new (getData(h)) std::vector<T>(std::move(vec));
        return h;
    }
    
    Handle allocateString(const std::string& str) {
        static constexpr size_t SSO_LIMIT = 23;
        
        if (str.size() <= SSO_LIMIT) {
            Handle h = allocate(SSO_LIMIT + 1);
            auto& entry = m_entries[h];
            entry.isSmallString = true;
            std::memcpy(entry.data, str.c_str(), str.size());
            entry.data[str.size()] = '\0';
            entry.size = str.size();
            entry.capacity = SSO_LIMIT + 1;
            return h;
        }
        
        Handle h = allocate(sizeof(std::string));
        new (getData(h)) std::string(str);
        return h;
    }
    
    Handle allocateString(std::string&& str) {
        static constexpr size_t SSO_LIMIT = 23;
        
        if (str.size() <= SSO_LIMIT) {
            Handle h = allocate(SSO_LIMIT + 1);
            auto& entry = m_entries[h];
            entry.isSmallString = true;
            std::memcpy(entry.data, str.c_str(), str.size());
            entry.data[str.size()] = '\0';
            entry.size = str.size();
            entry.capacity = SSO_LIMIT + 1;
            return h;
        }
        
        Handle h = allocate(sizeof(std::string));
        new (getData(h)) std::string(std::move(str));
        return h;
    }
    
    void deallocate(Handle h) {
        if (h == INVALID_HANDLE) return;
        
        std::lock_guard<std::mutex> lock(m_mutex);
        assert(h < m_entries.size() && "Invalid handle");
        
        auto& entry = m_entries[h];
        
        if (entry.isSmallString) {
            entry.data = nullptr;
        } else if (entry.data) {
            delete[] entry.data;
        }
        
        entry.data = nullptr;
        entry.size = 0;
        entry.capacity = 0;
        entry.isSmallString = false;
        
        m_freeList.push_back(h);
    }
    
    void* getData(Handle h) {
        assert(h < m_entries.size() && "Invalid handle");
        return m_entries[h].data;
    }
    
    const void* getData(Handle h) const {
        assert(h < m_entries.size() && "Invalid handle");
        return m_entries[h].data;
    }
    
    template<typename T>
    std::vector<T>* getVector(Handle h) {
        if (h == INVALID_HANDLE) return nullptr;
        return static_cast<std::vector<T>*>(getData(h));
    }
    
    template<typename T>
    const std::vector<T>* getVector(Handle h) const {
        if (h == INVALID_HANDLE) return nullptr;
        return static_cast<const std::vector<T>*>(getData(h));
    }
    
    std::string* getString(Handle h) {
        if (h == INVALID_HANDLE) return nullptr;
        auto& entry = m_entries[h];
        if (entry.isSmallString) {
            return nullptr;
        }
        return static_cast<std::string*>(getData(h));
    }
    
    const std::string* getString(Handle h) const {
        if (h == INVALID_HANDLE) return nullptr;
        auto& entry = m_entries[h];
        if (entry.isSmallString) {
            return nullptr;
        }
        return static_cast<const std::string*>(getData(h));
    }
    
    bool isSmallString(Handle h) const {
        if (h == INVALID_HANDLE) return false;
        return m_entries[h].isSmallString;
    }
    
    size_t getSmallStringSize(Handle h) const {
        if (h == INVALID_HANDLE) return 0;
        return m_entries[h].size;
    }
    
    const char* getSmallStringData(Handle h) const {
        if (h == INVALID_HANDLE) return nullptr;
        return reinterpret_cast<const char*>(m_entries[h].data);
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& entry : m_entries) {
            if (entry.data && !entry.isSmallString) {
                delete[] entry.data;
            }
        }
        m_entries.clear();
        m_freeList.clear();
    }
    
    size_t getEntryCount() const { return m_entries.size(); }
    size_t getActiveCount() const { return m_entries.size() - m_freeList.size(); }

private:
    std::vector<Entry> m_entries;
    std::vector<Handle> m_freeList;
    mutable std::mutex m_mutex;
};

class World;

} // namespace ecs
