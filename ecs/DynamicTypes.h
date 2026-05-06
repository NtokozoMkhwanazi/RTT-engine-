#pragma once

#include "DynamicPool.h"
#include <vector>
#include <string>
#include <cstring>
#include <initializer_list>

namespace ecs {

class DynamicPool;

template<typename T>
class DynamicVector {
public:
    using ValueType = T;
    using SizeType = size_t;
    using Reference = T&;
    using ConstReference = const T&;
    using Iterator = T*;
    using ConstIterator = const T*;

    DynamicVector() : m_handle(DynamicPool::INVALID_HANDLE), m_pool(nullptr) {}
    
    explicit DynamicVector(DynamicPool* pool) 
        : m_handle(DynamicPool::INVALID_HANDLE), m_pool(pool) {}
    
    DynamicVector(DynamicPool* pool, DynamicPool::Handle handle)
        : m_handle(handle), m_pool(pool) {}
    
    DynamicVector(std::initializer_list<T> init) : m_handle(DynamicPool::INVALID_HANDLE), m_pool(nullptr) {
        assign(init);
    }
    
    DynamicVector(const DynamicVector& other) : m_handle(DynamicPool::INVALID_HANDLE), m_pool(other.m_pool) {
        if (other.m_handle != DynamicPool::INVALID_HANDLE && m_pool) {
            const auto* vec = m_pool->getVector<T>(other.m_handle);
            if (vec) {
                m_handle = m_pool->allocateVector(*vec);
            }
        }
    }
    
    DynamicVector& operator=(const DynamicVector& other) {
        if (this != &other) {
            clear();
            m_pool = other.m_pool;
            if (other.m_handle != DynamicPool::INVALID_HANDLE && m_pool) {
                const auto* vec = m_pool->getVector<T>(other.m_handle);
                if (vec) {
                    m_handle = m_pool->allocateVector(*vec);
                }
            }
        }
        return *this;
    }
    
    DynamicVector(DynamicVector&& other) noexcept
        : m_handle(other.m_handle), m_pool(other.m_pool) {
        other.m_handle = DynamicPool::INVALID_HANDLE;
        other.m_pool = nullptr;
    }
    
    DynamicVector& operator=(DynamicVector&& other) noexcept {
        if (this != &other) {
            clear();
            m_handle = other.m_handle;
            m_pool = other.m_pool;
            other.m_handle = DynamicPool::INVALID_HANDLE;
            other.m_pool = nullptr;
        }
        return *this;
    }
    
    void setPool(DynamicPool* pool) { m_pool = pool; }
    
    DynamicPool::Handle getHandle() const { return m_handle; }
    void setHandle(DynamicPool::Handle h) { m_handle = h; }
    
    void assign(const std::vector<T>& vec) {
        if (m_pool) {
            if (m_handle != DynamicPool::INVALID_HANDLE) {
                m_pool->deallocate(m_handle);
            }
            m_handle = m_pool->allocateVector(vec);
        }
    }
    
    void assign(std::vector<T>&& vec) {
        if (m_pool) {
            if (m_handle != DynamicPool::INVALID_HANDLE) {
                m_pool->deallocate(m_handle);
            }
            m_handle = m_pool->allocateVector(std::move(vec));
        }
    }
    
    void assign(std::initializer_list<T> init) {
        std::vector<T> vec(init);
        assign(std::move(vec));
    }
    
    void clear() {
        if (m_pool && m_handle != DynamicPool::INVALID_HANDLE) {
            m_pool->deallocate(m_handle);
            m_handle = DynamicPool::INVALID_HANDLE;
        }
    }
    
    bool empty() const {
        if (m_pool && m_handle != DynamicPool::INVALID_HANDLE) {
            const auto* vec = m_pool->getVector<T>(m_handle);
            return !vec || vec->empty();
        }
        return true;
    }
    
    size_t size() const {
        if (m_pool && m_handle != DynamicPool::INVALID_HANDLE) {
            const auto* vec = m_pool->getVector<T>(m_handle);
            return vec ? vec->size() : 0;
        }
        return 0;
    }
    
    void resize(size_t newSize) {
        if (!m_pool) return;
        auto* vec = getVector();
        if (vec) {
            vec->resize(newSize);
        } else if (newSize > 0) {
            std::vector<T> newVec(newSize);
            m_handle = m_pool->allocateVector(std::move(newVec));
        }
    }
    
    void push_back(const T& value) {
        auto* vec = getVector();
        if (vec) {
            vec->push_back(value);
        }
    }
    
    void push_back(T&& value) {
        auto* vec = getVector();
        if (vec) {
            vec->push_back(std::move(value));
        }
    }
    
    void pop_back() {
        auto* vec = getVector();
        if (vec) {
            vec->pop_back();
        }
    }
    
    T& operator[](size_t index) {
        static T dummy = T();
        auto* vec = getVector();
        if (vec && index < vec->size()) {
            return (*vec)[index];
        }
        return dummy;
    }
    
    const T& operator[](size_t index) const {
        static const T dummy = T();
        auto* vec = getVector();
        if (vec && index < vec->size()) {
            return (*vec)[index];
        }
        return dummy;
    }
    
    T* data() {
        auto* vec = getVector();
        return vec ? vec->data() : nullptr;
    }
    
    const T* data() const {
        const auto* vec = getVector();
        return vec ? vec->data() : nullptr;
    }
    
    Iterator begin() {
        auto* vec = getVector();
        return vec ? vec->data() : nullptr;
    }
    
    Iterator end() {
        auto* vec = getVector();
        return vec ? vec->data() + vec->size() : nullptr;
    }
    
    ConstIterator begin() const {
        const auto* vec = getVector();
        return vec ? vec->data() : nullptr;
    }
    
    ConstIterator end() const {
        const auto* vec = getVector();
        return vec ? vec->data() + vec->size() : nullptr;
    }
    
    bool isValid() const { return m_pool && m_handle != DynamicPool::INVALID_HANDLE; }

private:
    std::vector<T>* getVector() {
        if (!m_pool || m_handle == DynamicPool::INVALID_HANDLE) return nullptr;
        return m_pool->getVector<T>(m_handle);
    }
    
    const std::vector<T>* getVector() const {
        if (!m_pool || m_handle == DynamicPool::INVALID_HANDLE) return nullptr;
        return m_pool->getVector<T>(m_handle);
    }
    
    DynamicPool::Handle m_handle;
    DynamicPool* m_pool = nullptr;
};

class DynamicString {
public:
    using ValueType = char;
    using SizeType = size_t;
    using Reference = char&;
    using ConstReference = const char&;
    using Iterator = char*;
    using ConstIterator = const char*;
    static constexpr size_t SSO_LIMIT = 23;

    DynamicString() : m_handle(DynamicPool::INVALID_HANDLE), m_pool(nullptr), m_size(0) {}
    
    explicit DynamicString(DynamicPool* pool) 
        : m_handle(DynamicPool::INVALID_HANDLE), m_pool(pool), m_size(0) {}
    
    DynamicString(DynamicPool* pool, DynamicPool::Handle handle)
        : m_handle(handle), m_pool(pool), m_size(0) {
        if (m_pool && m_handle != DynamicPool::INVALID_HANDLE) {
            if (m_pool->isSmallString(m_handle)) {
                m_size = m_pool->getSmallStringSize(m_handle);
            } else {
                const auto* str = m_pool->getString(m_handle);
                m_size = str ? str->size() : 0;
            }
        }
    }
    
    DynamicString(const char* cstr) : m_handle(DynamicPool::INVALID_HANDLE), m_pool(nullptr), m_size(0) {
        assign(cstr);
    }
    
    DynamicString(const std::string& str) : m_handle(DynamicPool::INVALID_HANDLE), m_pool(nullptr), m_size(0) {
        assign(str);
    }
    
    DynamicString(const DynamicString& other)
        : m_handle(DynamicPool::INVALID_HANDLE), m_pool(other.m_pool), m_size(other.m_size) {
        if (other.m_handle != DynamicPool::INVALID_HANDLE && m_pool) {
            if (m_pool->isSmallString(other.m_handle)) {
                m_handle = m_pool->allocateString(std::string(m_pool->getSmallStringData(other.m_handle), m_size));
            } else {
                const auto* str = m_pool->getString(other.m_handle);
                if (str) {
                    m_handle = m_pool->allocateString(*str);
                }
            }
        }
    }
    
    DynamicString& operator=(const DynamicString& other) {
        if (this != &other) {
            clear();
            m_pool = other.m_pool;
            m_size = other.m_size;
            if (other.m_handle != DynamicPool::INVALID_HANDLE && m_pool) {
                if (m_pool->isSmallString(other.m_handle)) {
                    m_handle = m_pool->allocateString(std::string(m_pool->getSmallStringData(other.m_handle), m_size));
                } else {
                    const auto* str = m_pool->getString(other.m_handle);
                    if (str) {
                        m_handle = m_pool->allocateString(*str);
                    }
                }
            }
        }
        return *this;
    }
    
    DynamicString(DynamicString&& other) noexcept
        : m_handle(other.m_handle), m_pool(other.m_pool), m_size(other.m_size) {
        other.m_handle = DynamicPool::INVALID_HANDLE;
        other.m_pool = nullptr;
        other.m_size = 0;
    }
    
    DynamicString& operator=(DynamicString&& other) noexcept {
        if (this != &other) {
            clear();
            m_handle = other.m_handle;
            m_pool = other.m_pool;
            m_size = other.m_size;
            other.m_handle = DynamicPool::INVALID_HANDLE;
            other.m_pool = nullptr;
            other.m_size = 0;
        }
        return *this;
    }
    
    void setPool(DynamicPool* pool) { m_pool = pool; }
    DynamicPool::Handle getHandle() const { return m_handle; }
    void setHandle(DynamicPool::Handle h) { 
        m_handle = h;
        if (m_pool && m_handle != DynamicPool::INVALID_HANDLE) {
            if (m_pool->isSmallString(m_handle)) {
                m_size = m_pool->getSmallStringSize(m_handle);
            } else {
                const auto* str = m_pool->getString(m_handle);
                m_size = str ? str->size() : 0;
            }
        }
    }
    
    void assign(const char* cstr) {
        if (cstr) {
            assign(std::string(cstr));
        }
    }
    
    void assign(const std::string& str) {
        if (m_pool) {
            if (m_handle != DynamicPool::INVALID_HANDLE) {
                m_pool->deallocate(m_handle);
            }
            m_handle = m_pool->allocateString(str);
            m_size = str.size();
        } else {
            m_size = str.size();
        }
    }
    
    void assign(std::string&& str) {
        if (m_pool) {
            if (m_handle != DynamicPool::INVALID_HANDLE) {
                m_pool->deallocate(m_handle);
            }
            m_handle = m_pool->allocateString(std::move(str));
            m_size = str.size();
        } else {
            m_size = str.size();
        }
    }
    
    void clear() {
        if (m_pool && m_handle != DynamicPool::INVALID_HANDLE) {
            m_pool->deallocate(m_handle);
        }
        m_handle = DynamicPool::INVALID_HANDLE;
        m_size = 0;
    }
    
    bool empty() const { return m_size == 0; }
    size_t size() const { return m_size; }
    
    const char* c_str() const {
        static const char empty[] = "";
        if (!m_pool || m_handle == DynamicPool::INVALID_HANDLE) return empty;
        if (m_pool->isSmallString(m_handle)) {
            return m_pool->getSmallStringData(m_handle);
        }
        const auto* str = m_pool->getString(m_handle);
        return str ? str->c_str() : empty;
    }
    
    std::string toString() const {
        return std::string(c_str(), m_size);
    }
    
    char& operator[](size_t index) {
        static char dummy = '\0';
        if (index >= m_size) return dummy;
        if (!m_pool || m_handle == DynamicPool::INVALID_HANDLE) return dummy;
        if (m_pool->isSmallString(m_handle)) {
            return const_cast<char*>(m_pool->getSmallStringData(m_handle))[index];
        }
        auto* str = m_pool->getString(m_handle);
        if (str && index < str->size()) {
            return (*str)[index];
        }
        return dummy;
    }
    
    const char& operator[](size_t index) const {
        static const char dummy = '\0';
        if (index >= m_size) return dummy;
        if (!m_pool || m_handle == DynamicPool::INVALID_HANDLE) return dummy;
        if (m_pool->isSmallString(m_handle)) {
            return m_pool->getSmallStringData(m_handle)[index];
        }
        const auto* str = m_pool->getString(m_handle);
        if (str && index < str->size()) {
            return (*str)[index];
        }
        return dummy;
    }
    
    DynamicString& operator+=(const std::string& rhs) {
        std::string temp(c_str());
        temp += rhs;
        assign(std::move(temp));
        return *this;
    }
    
    DynamicString& operator+=(const char* rhs) {
        std::string temp(c_str());
        temp += rhs;
        assign(std::move(temp));
        return *this;
    }
    
    bool operator==(const DynamicString& other) const {
        if (m_size != other.m_size) return false;
        return std::strncmp(c_str(), other.c_str(), m_size) == 0;
    }
    
    bool operator==(const std::string& other) const {
        if (m_size != other.size()) return false;
        return std::strncmp(c_str(), other.c_str(), m_size) == 0;
    }
    
    bool operator==(const char* other) const {
        if (!other) return empty();
        size_t len = std::strlen(other);
        if (m_size != len) return false;
        return std::strncmp(c_str(), other, m_size) == 0;
    }
    
    bool operator!=(const DynamicString& other) const { return !(*this == other); }
    bool operator!=(const std::string& other) const { return !(*this == other); }
    bool operator!=(const char* other) const { return !(*this == other); }
    
    bool isValid() const { return m_pool && m_handle != DynamicPool::INVALID_HANDLE && m_size > 0; }

private:
    DynamicPool::Handle m_handle;
    DynamicPool* m_pool = nullptr;
    size_t m_size = 0;
};

inline bool operator==(const std::string& lhs, const DynamicString& rhs) { return rhs == lhs; }
inline bool operator==(const char* lhs, const DynamicString& rhs) { return rhs == lhs; }

} // namespace ecs
