#include "MemoryArena.h"
#include <algorithm>
#include <iomanip>

// ============================================================================
// Memory Stats Implementation
// ============================================================================

size_t MemoryStats::totalAllocations = 0;
size_t MemoryStats::totalDeallocations = 0;
size_t MemoryStats::currentAllocations = 0;
size_t MemoryStats::peakAllocations = 0;
size_t MemoryStats::totalBytesAllocated = 0;
size_t MemoryStats::currentBytesAllocated = 0;
size_t MemoryStats::peakBytesAllocated = 0;

void MemoryStats::recordAllocation(size_t bytes) {
    ++totalAllocations;
    ++currentAllocations;
    totalBytesAllocated += bytes;
    currentBytesAllocated += bytes;
    
    if (currentAllocations > peakAllocations) {
        peakAllocations = currentAllocations;
    }
    if (currentBytesAllocated > peakBytesAllocated) {
        peakBytesAllocated = currentBytesAllocated;
    }
}

void MemoryStats::recordDeallocation(size_t bytes) {
    ++totalDeallocations;
    --currentAllocations;
    currentBytesAllocated -= bytes;
}

void MemoryStats::printStats() {
    std::cout << "\n========== MEMORY STATISTICS ==========\n";
    std::cout << "Allocations: " << totalAllocations << " total, "
              << currentAllocations << " current, "
              << peakAllocations << " peak\n";
    std::cout << "Deallocations: " << totalDeallocations << " total\n";
    std::cout << "Bytes allocated: " << totalBytesAllocated << " total, "
              << currentBytesAllocated << " current, "
              << peakBytesAllocated << " peak\n";
    
    if (totalAllocations > 0) {
        std::cout << "Average allocation size: " 
                  << (totalBytesAllocated / totalAllocations) << " bytes\n";
    }
    
    float efficiency = (totalDeallocations > 0) ? 
        (100.0f * totalDeallocations / totalAllocations) : 0.0f;
    std::cout << "Memory efficiency: " << efficiency << "%\n";
    std::cout << "========================================\n\n";
}

void MemoryStats::reset() {
    totalAllocations = 0;
    totalDeallocations = 0;
    currentAllocations = 0;
    peakAllocations = 0;
    totalBytesAllocated = 0;
    currentBytesAllocated = 0;
    peakBytesAllocated = 0;
}

// ============================================================================
// Memory Arena Implementation
// ============================================================================

MemoryArena::MemoryArena(size_t sizeBytes)
    : totalSize(sizeBytes)
    , offset(0)
    , allocationCount(0)
    , peakUsage(0)
{
    // Allocate aligned buffer
    buffer = new uint8_t[sizeBytes];
    
    #ifdef DEBUG_MEMORY
    std::cout << "[MemoryArena] Created arena: " << sizeBytes << " bytes ("
              << (sizeBytes / 1024) << " KB)\n";
    #endif
}

MemoryArena::~MemoryArena() {
    #ifdef DEBUG_MEMORY
    std::cout << "[MemoryArena] Destroyed arena: " << getUsedSize() 
              << " bytes used, " << allocationCount << " allocations\n";
    #endif
    
    delete[] buffer;
}

void* MemoryArena::allocate(size_t bytes, size_t alignment) {
    // Align the current offset
    size_t alignedOffset = (offset + alignment - 1) & ~(alignment - 1);
    
    // Check if we have enough space
    if (alignedOffset + bytes > totalSize) {
        std::cerr << "[MemoryArena] Out of memory! Requested: " << bytes
                  << " bytes, Available: " << (totalSize - alignedOffset)
                  << " bytes\n";
        return nullptr;
    }
    
    // Bump allocate
    void* ptr = buffer + alignedOffset;
    offset = alignedOffset + bytes;
    ++allocationCount;
    
    // Track peak usage
    if (offset > peakUsage) {
        peakUsage = offset;
    }
    
    TRACK_ALLOC(bytes);
    
    #ifdef DEBUG_MEMORY
    std::cout << "[MemoryArena] Allocated " << bytes << " bytes at offset "
              << alignedOffset << " (total used: " << offset << ")\n";
    #endif
    
    return ptr;
}

void MemoryArena::reset() {
    #ifdef DEBUG_MEMORY
    std::cout << "[MemoryArena] Reset: " << allocationCount 
              << " allocations freed\n";
    #endif
    
    offset = 0;
    allocationCount = 0;
}

// ============================================================================
// Dual Arena Implementation
// ============================================================================

DualArena::DualArena(size_t sizePerArena)
    : arenas{MemoryArena(sizePerArena), MemoryArena(sizePerArena)}
    , currentIndex(0)
{
}

DualArena::~DualArena() {
}

void DualArena::swap() {
    currentIndex = 1 - currentIndex;
}

void DualArena::resetInactive() {
    arenas[1 - currentIndex].reset();
}

// ============================================================================
// Stack Allocator Implementation
// ============================================================================

StackAllocator::StackAllocator(size_t sizeBytes)
    : totalSize(sizeBytes)
    , offset(0)
{
    buffer = new uint8_t[sizeBytes];
    
    #ifdef DEBUG_MEMORY
    std::cout << "[StackAllocator] Created: " << sizeBytes << " bytes\n";
    #endif
}

StackAllocator::~StackAllocator() {
    delete[] buffer;
}

void* StackAllocator::allocate(size_t bytes, size_t alignment) {
    size_t alignedOffset = (offset + alignment - 1) & ~(alignment - 1);
    
    if (alignedOffset + bytes > totalSize) {
        std::cerr << "[StackAllocator] Out of memory!\n";
        return nullptr;
    }
    
    void* ptr = buffer + alignedOffset;
    offset = alignedOffset + bytes;
    
    TRACK_ALLOC(bytes);
    
    return ptr;
}

void StackAllocator::deallocate(void* ptr) {
    if (!ptr) return;

    // Calculate the offset of this pointer
    size_t ptrOffset = reinterpret_cast<uint8_t*>(ptr) - buffer;

    // Can only deallocate if it's the last allocation (LIFO)
    // The pointer should be at or near the current offset (accounting for alignment)
    if (ptrOffset >= offset) {
        std::cerr << "[StackAllocator] Can only deallocate last allocation!\n";
        return;
    }
    
    // Reset to this position
    offset = ptrOffset;

    TRACK_FREE(totalSize - ptrOffset);
}

void StackAllocator::reset() {
    offset = 0;
}
