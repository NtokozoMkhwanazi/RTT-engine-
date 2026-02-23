#pragma once
/**
 * Memory Arena - Fast Frame-Based Allocation
 * 
 * AAA Game Standard: Allocate many small objects quickly,
 * free them all at once at end of frame/level.
 * 
 * Usage:
 *   MemoryArena arena(1024 * 1024);  // 1MB arena
 *   void* ptr = arena.allocate(64);   // Fast bump allocation
 *   arena.reset();                     // Free all at once
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <memory>

class MemoryArena {
public:
    explicit MemoryArena(size_t sizeBytes);
    ~MemoryArena();

    // Allocate memory (fast bump allocation)
    void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t));
    
    // Allocate and construct object
    template<typename T, typename... Args>
    T* create(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        return new (mem) T(std::forward<Args>(args)...);
    }

    // Free all allocated memory (O(1) - just reset pointer)
    void reset();

    // Get statistics
    size_t getTotalSize() const { return totalSize; }
    size_t getUsedSize() const { return offset; }
    size_t getFreeSize() const { return totalSize - offset; }
    float getUsagePercent() const { return 100.0f * offset / totalSize; }
    size_t getAllocationCount() const { return allocationCount; }

    // Check if allocation would fit
    bool canAllocate(size_t bytes) const { return offset + bytes <= totalSize; }

private:
    uint8_t* buffer;
    size_t totalSize;
    size_t offset;
    size_t allocationCount;
    size_t peakUsage;
};

// ============================================================================
// Dual Arena System - Ping-pong buffers for frame-based allocation
// ============================================================================

class DualArena {
public:
    explicit DualArena(size_t sizePerArena);
    ~DualArena();

    // Get current arena for writing
    MemoryArena& getCurrent() { return arenas[currentIndex]; }
    
    // Switch to other arena (call at end of frame)
    void swap();
    
    // Reset the inactive arena (safe to do while using active)
    void resetInactive();

    // Get statistics
    size_t getTotalSize() const { return arenas[0].getTotalSize() + arenas[1].getTotalSize(); }
    size_t getUsedSize() const { return arenas[currentIndex].getUsedSize(); }

private:
    MemoryArena arenas[2];
    int currentIndex;
};

// ============================================================================
// Stack Allocator - LIFO allocation pattern
// ============================================================================

class StackAllocator {
public:
    explicit StackAllocator(size_t sizeBytes);
    ~StackAllocator();

    void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t));
    void deallocate(void* ptr);  // Must be last allocation (LIFO)
    void reset();

    size_t getUsedSize() const { return offset; }
    size_t getFreeSize() const { return totalSize - offset; }

private:
    uint8_t* buffer;
    size_t totalSize;
    size_t offset;
};

// ============================================================================
// Memory Pool - Fixed-size block allocation
// ============================================================================

template<typename T>
class MemoryPool {
public:
    explicit MemoryPool(size_t initialCapacity = 256) {
        grow(initialCapacity);
    }

    ~MemoryPool() {
        // Return all blocks to free list
        for (Block* block : allBlocks) {
            delete[] reinterpret_cast<uint8_t*>(block);
        }
    }

    // Allocate object (O(1) if free list not empty)
    T* allocate() {
        if (freeList) {
            Block* block = freeList;
            freeList = block->next;
            --freeCount;
            ++usedCount;
            return reinterpret_cast<T*>(block);
        }
        
        // Grow pool if empty
        grow(blockSize);
        return allocate();
    }

    // Free object (O(1) - just add to free list)
    void deallocate(T* ptr) {
        Block* block = reinterpret_cast<Block*>(ptr);
        block->next = freeList;
        freeList = block;
        ++freeCount;
        --usedCount;
    }

    // Construct object in-place
    template<typename... Args>
    T* create(Args&&... args) {
        T* obj = allocate();
        return new (obj) T(std::forward<Args>(args)...);
    }

    // Destroy object and return to pool
    void destroy(T* obj) {
        obj->~T();
        deallocate(obj);
    }

    // Statistics
    size_t getUsedCount() const { return usedCount; }
    size_t getFreeCount() const { return freeCount; }
    size_t getTotalCount() const { return usedCount + freeCount; }
    size_t getMemoryUsage() const { return getTotalCount() * sizeof(T); }

    // Reserve capacity
    void reserve(size_t count) {
        while (getTotalCount() < count) {
            grow(blockSize);
        }
    }

private:
    struct Block {
        Block* next;
        // Memory for T follows
    };

    void grow(size_t count) {
        size_t bytes = count * sizeof(T);
        Block* newBlock = reinterpret_cast<Block*>(new uint8_t[bytes]);
        allBlocks.push_back(newBlock);

        // Add all new blocks to free list
        for (size_t i = 0; i < count; ++i) {
            Block* block = reinterpret_cast<Block*>(
                reinterpret_cast<uint8_t*>(newBlock) + i * sizeof(T)
            );
            block->next = freeList;
            freeList = block;
            ++freeCount;
        }
    }

    Block* freeList = nullptr;
    std::vector<Block*> allBlocks;
    size_t usedCount = 0;
    size_t freeCount = 0;
    size_t blockSize = 256;
};

// ============================================================================
// Global Memory Stats (for profiling)
// ============================================================================

struct MemoryStats {
    static size_t totalAllocations;
    static size_t totalDeallocations;
    static size_t currentAllocations;
    static size_t peakAllocations;
    static size_t totalBytesAllocated;
    static size_t currentBytesAllocated;
    static size_t peakBytesAllocated;

    static void recordAllocation(size_t bytes);
    static void recordDeallocation(size_t bytes);
    static void printStats();
    static void reset();
};

// ============================================================================
// Debug macros
// ============================================================================

#ifdef DEBUG_MEMORY
    #define TRACK_ALLOC(size) MemoryStats::recordAllocation(size)
    #define TRACK_FREE(size) MemoryStats::recordDeallocation(size)
#else
    #define TRACK_ALLOC(size)
    #define TRACK_FREE(size)
#endif
