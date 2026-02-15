#pragma once
#include <vector>
#include <queue>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <typeindex>
#include <atomic>

// Generic object pool template
template<typename T>
class ObjectPool {
public:
    ObjectPool(size_t initialSize = 10) {
        grow(initialSize);
    }

    ~ObjectPool() {
        clear();
    }

    // Acquire an object from the pool
    std::shared_ptr<T> acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (freeObjects_.empty()) {
            // Pool is exhausted, grow it
            size_t currentSize = objects_.size();
            grow(currentSize); // Double the size
        }

        T* obj = freeObjects_.front();
        freeObjects_.pop();
        
        // Create a shared_ptr with custom deleter that returns object to pool
        return std::shared_ptr<T>(obj, [this](T* ptr) {
            release(ptr);
        });
    }

    // Release an object back to the pool
    void release(T* obj) {
        if (obj) {
            obj->reset(); // Reset object state if T has a reset() method
            std::lock_guard<std::mutex> lock(mutex_);
            freeObjects_.push(obj);
        }
    }

    // Get current pool statistics
    size_t getTotalObjects() const { return objects_.size(); }
    size_t getFreeObjects() const { return freeObjects_.size(); }
    size_t getUsedObjects() const { return getTotalObjects() - getFreeObjects(); }

private:
    void grow(size_t count) {
        for (size_t i = 0; i < count; ++i) {
            T* newObj = new T();
            objects_.emplace_back(newObj);
            freeObjects_.push(newObj);
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        objects_.clear();
        while (!freeObjects_.empty()) {
            freeObjects_.pop();
        }
    }

    std::vector<std::unique_ptr<T>> objects_;
    std::queue<T*> freeObjects_;
    mutable std::mutex mutex_;
};

// Memory tracker for debugging
class MemoryTracker {
public:
    static MemoryTracker& getInstance() {
        static MemoryTracker instance;
        return instance;
    }

    void recordAllocation(size_t size, const char* typeName) {
        std::lock_guard<std::mutex> lock(mutex_);
        allocatedBytes_ += size;
        allocationCount_++;
        
        allocations_[typeName]++;
    }

    void recordDeallocation(size_t size, const char* typeName) {
        std::lock_guard<std::mutex> lock(mutex_);
        deallocatedBytes_ += size;
        deallocationCount_++;
        
        if (allocations_[typeName] > 0) {
            allocations_[typeName]--;
        }
    }

    size_t getCurrentAllocatedBytes() const { return allocatedBytes_; }
    size_t getAllocationCount() const { return allocationCount_; }
    size_t getDeallocationCount() const { return deallocationCount_; }
    
    void printStatistics() const {
        printf("Memory Statistics:\n");
        printf("  Allocated bytes: %zu\n", allocatedBytes_.load());
        printf("  Deallocated bytes: %zu\n", deallocatedBytes_.load());
        printf("  Net allocated: %zd\n", 
               static_cast<ptrdiff_t>(allocatedBytes_) - 
               static_cast<ptrdiff_t>(deallocatedBytes_));
        printf("  Allocation count: %zu\n", allocationCount_.load());
        printf("  Deallocation count: %zu\n", deallocationCount_.load());
    }

private:
    MemoryTracker() = default;
    ~MemoryTracker() = default;

    mutable std::mutex mutex_;
    std::atomic<size_t> allocatedBytes_{0};
    std::atomic<size_t> deallocatedBytes_{0};
    std::atomic<size_t> allocationCount_{0};
    std::atomic<size_t> deallocationCount_{0};
    std::unordered_map<const char*, size_t> allocations_;
};

// Memory-managed base class
class MemoryManagedObject {
public:
    virtual ~MemoryManagedObject() = default;
    
    // Override this in derived classes to reset object state
    virtual void reset() {}
    
protected:
    MemoryManagedObject() {
        MemoryTracker::getInstance().recordAllocation(sizeof(*this), typeid(*this).name());
    }
    
    MemoryManagedObject(const MemoryManagedObject&) {
        MemoryTracker::getInstance().recordAllocation(sizeof(*this), typeid(*this).name());
    }
    
    MemoryManagedObject& operator=(const MemoryManagedObject&) {
        return *this;
    }
};

// Memory manager singleton
class MemoryManager {
public:
    static MemoryManager& getInstance() {
        static MemoryManager instance;
        return instance;
    }

    template<typename T>
    ObjectPool<T>& getObjectPool(size_t initialSize = 10) {
        std::type_index typeIdx = std::type_index(typeid(T));
        
        auto it = objectPools_.find(typeIdx);
        if (it != objectPools_.end()) {
            return *static_cast<ObjectPool<T>*>(it->second.get());
        }
        
        // Create new pool
        auto pool = std::make_unique<ObjectPool<T>>(initialSize);
        ObjectPool<T>* poolPtr = pool.get();
        objectPools_[typeIdx] = std::move(pool);
        
        return *poolPtr;
    }

    void printMemoryStats() const {
        MemoryTracker::getInstance().printStatistics();
    }

private:
    MemoryManager() = default;
    ~MemoryManager() = default;

    std::unordered_map<std::type_index, std::unique_ptr<void>> objectPools_;
    
    mutable std::mutex mutex_;
};