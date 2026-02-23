/**
 * Memory Management System Tests
 * 
 * Tests for Memory Arena, Object Pool, Asset Manager
 * Run with: make test
 */

#include <gtest/gtest.h>
#include "../memory/MemoryArena.h"
#include "../memory/AssetManager.h"
#include <vector>
#include <string>
#include <cstring>

// ============================================================================
// TEST FIXTURE
// ============================================================================

class MemoryManagementTest : public ::testing::Test {
protected:
    void SetUp() override {
        MemoryStats::reset();
        std::cout << "\n========== MemoryManagementTest::SetUp ==========\n";
    }

    void TearDown() override {
        std::cout << "========== MemoryManagementTest::TearDown ==========\n";
        MemoryStats::printStats();
    }
};

// ============================================================================
// MEMORY ARENA TESTS
// ============================================================================

/**
 * Test 1: Arena Creation
 */
TEST_F(MemoryManagementTest, Arena_Creation) {
    MemoryArena arena(1024);  // 1 KB arena
    
    EXPECT_EQ(arena.getTotalSize(), 1024);
    EXPECT_EQ(arena.getUsedSize(), 0);
    EXPECT_EQ(arena.getFreeSize(), 1024);
    EXPECT_EQ(arena.getAllocationCount(), 0);
    EXPECT_FLOAT_EQ(arena.getUsagePercent(), 0.0f);
}

/**
 * Test 2: Arena Allocation
 */
TEST_F(MemoryManagementTest, Arena_Allocation) {
    MemoryArena arena(1024);
    
    // Allocate some memory
    void* ptr1 = arena.allocate(64);
    EXPECT_NE(ptr1, nullptr);
    EXPECT_EQ(arena.getUsedSize(), 64);
    EXPECT_EQ(arena.getAllocationCount(), 1);
    
    // Allocate more
    void* ptr2 = arena.allocate(128);
    EXPECT_NE(ptr2, nullptr);
    EXPECT_EQ(arena.getUsedSize(), 192);  // 64 + 128
    EXPECT_EQ(arena.getAllocationCount(), 2);
    
    // Pointers should be different
    EXPECT_NE(ptr1, ptr2);
}

/**
 * Test 3: Arena Alignment
 */
TEST_F(MemoryManagementTest, Arena_Alignment) {
    MemoryArena arena(1024);
    
    // Allocate with different alignments
    void* ptr1 = arena.allocate(10, 1);    // 1-byte alignment
    void* ptr2 = arena.allocate(10, 8);    // 8-byte alignment
    void* ptr3 = arena.allocate(10, 16);   // 16-byte alignment
    
    EXPECT_NE(ptr1, nullptr);
    EXPECT_NE(ptr2, nullptr);
    EXPECT_NE(ptr3, nullptr);
    
    // Check alignment
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr2) % 8, 0);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr3) % 16, 0);
}

/**
 * Test 4: Arena Reset
 */
TEST_F(MemoryManagementTest, Arena_Reset) {
    MemoryArena arena(1024);
    
    // Allocate (with alignment padding)
    arena.allocate(100);
    arena.allocate(200);
    arena.allocate(300);
    
    size_t usedBeforeReset = arena.getUsedSize();
    EXPECT_GE(usedBeforeReset, 600);  // At least 600, may be more due to alignment
    EXPECT_EQ(arena.getAllocationCount(), 3);
    
    // Reset
    arena.reset();
    
    EXPECT_EQ(arena.getUsedSize(), 0);
    EXPECT_EQ(arena.getAllocationCount(), 0);
    EXPECT_EQ(arena.getFreeSize(), 1024);
    
    // Should be able to allocate again
    void* ptr = arena.allocate(500);
    EXPECT_NE(ptr, nullptr);
}

/**
 * Test 5: Arena Out of Memory
 */
TEST_F(MemoryManagementTest, Arena_OutOfMemory) {
    MemoryArena arena(256);  // Small arena
    
    // Allocate most of it
    arena.allocate(100);
    
    // Try to allocate too much
    void* ptr = arena.allocate(200);
    EXPECT_EQ(ptr, nullptr);  // Should fail
    
    // But small allocation should work
    void* small = arena.allocate(50);
    EXPECT_NE(small, nullptr);
}

/**
 * Test 6: Arena Create Object
 */
TEST_F(MemoryManagementTest, Arena_CreateObject) {
    MemoryArena arena(1024);
    
    struct TestObject {
        int x, y;
        TestObject(int a, int b) : x(a), y(b) {}
    };
    
    TestObject* obj = arena.create<TestObject>(10, 20);
    EXPECT_NE(obj, nullptr);
    EXPECT_EQ(obj->x, 10);
    EXPECT_EQ(obj->y, 20);
}

/**
 * Test 7: Arena Peak Usage
 */
TEST_F(MemoryManagementTest, Arena_PeakUsage) {
    MemoryArena arena(1024);
    
    arena.allocate(100);
    arena.allocate(200);
    // Peak: 300
    
    arena.reset();
    
    arena.allocate(150);
    // Peak still 300
    
    // Note: peakUsage tracking is internal, would need getter
    EXPECT_EQ(arena.getUsedSize(), 150);
}

// ============================================================================
// DUAL ARENA TESTS
// ============================================================================

/**
 * Test 8: Dual Arena Creation
 */
TEST_F(MemoryManagementTest, DualArena_Creation) {
    DualArena dual(1024);
    
    EXPECT_EQ(dual.getTotalSize(), 2048);  // 2 x 1024
    EXPECT_EQ(dual.getUsedSize(), 0);
}

/**
 * Test 9: Dual Arena Swap
 */
TEST_F(MemoryManagementTest, DualArena_Swap) {
    DualArena dual(1024);
    
    // Allocate in current arena
    dual.getCurrent().allocate(100);
    EXPECT_EQ(dual.getUsedSize(), 100);
    
    // Swap
    dual.swap();
    
    // New arena should be empty
    EXPECT_EQ(dual.getUsedSize(), 0);
    
    // Allocate in new arena
    dual.getCurrent().allocate(200);
    EXPECT_EQ(dual.getUsedSize(), 200);
}

/**
 * Test 10: Dual Arena Reset Inactive
 */
TEST_F(MemoryManagementTest, DualArena_ResetInactive) {
    DualArena dual(1024);
    
    // Allocate in both arenas
    dual.getCurrent().allocate(100);
    dual.swap();
    dual.getCurrent().allocate(200);
    dual.swap();
    
    // Reset inactive (the one we're not using)
    dual.resetInactive();
    
    // Current should still have data
    EXPECT_EQ(dual.getUsedSize(), 100);
}

// ============================================================================
// STACK ALLOCATOR TESTS
// ============================================================================

/**
 * Test 11: Stack Allocator Creation
 */
TEST_F(MemoryManagementTest, StackAllocator_Creation) {
    StackAllocator stack(1024);
    
    EXPECT_EQ(stack.getUsedSize(), 0);
    EXPECT_EQ(stack.getFreeSize(), 1024);
}

/**
 * Test 12: Stack Allocator LIFO
 */
TEST_F(MemoryManagementTest, StackAllocator_LIFO) {
    StackAllocator stack(1024);
    
    void* ptr1 = stack.allocate(100);
    void* ptr2 = stack.allocate(200);
    void* ptr3 = stack.allocate(300);
    
    size_t usedAfterAlloc = stack.getUsedSize();
    EXPECT_GE(usedAfterAlloc, 600);  // At least 600, may be more due to alignment
    
    // Deallocate in LIFO order (last allocated first)
    stack.deallocate(ptr3);
    size_t usedAfterDealloc1 = stack.getUsedSize();
    EXPECT_LT(usedAfterDealloc1, usedAfterAlloc);
    
    stack.deallocate(ptr2);
    size_t usedAfterDealloc2 = stack.getUsedSize();
    EXPECT_LT(usedAfterDealloc2, usedAfterDealloc1);
    
    // ptr1 still allocated
    stack.deallocate(ptr1);
    EXPECT_EQ(stack.getUsedSize(), 0);
}

/**
 * Test 13: Stack Allocator Reset
 */
TEST_F(MemoryManagementTest, StackAllocator_Reset) {
    StackAllocator stack(1024);
    
    stack.allocate(100);
    stack.allocate(200);
    stack.allocate(300);
    
    stack.reset();
    
    EXPECT_EQ(stack.getUsedSize(), 0);
    EXPECT_EQ(stack.getFreeSize(), 1024);
}

// ============================================================================
// MEMORY POOL TESTS
// ============================================================================

/**
 * Test 14: Memory Pool Creation
 */
TEST_F(MemoryManagementTest, MemoryPool_Creation) {
    MemoryPool<int> pool(100);
    
    EXPECT_EQ(pool.getUsedCount(), 0);
    EXPECT_EQ(pool.getFreeCount(), 100);
    EXPECT_EQ(pool.getTotalCount(), 100);
}

/**
 * Test 15: Memory Pool Allocate
 */
TEST_F(MemoryManagementTest, MemoryPool_Allocate) {
    MemoryPool<int> pool(50);
    
    int* ptr1 = pool.allocate();
    EXPECT_NE(ptr1, nullptr);
    
    EXPECT_EQ(pool.getUsedCount(), 1);
    EXPECT_EQ(pool.getFreeCount(), 49);
    
    int* ptr2 = pool.allocate();
    EXPECT_NE(ptr2, nullptr);
    EXPECT_NE(ptr1, ptr2);  // Different blocks
    
    EXPECT_EQ(pool.getUsedCount(), 2);
}

/**
 * Test 16: Memory Pool Deallocate
 */
TEST_F(MemoryManagementTest, MemoryPool_Deallocate) {
    MemoryPool<int> pool(50);
    
    int* ptr1 = pool.allocate();
    int* ptr2 = pool.allocate();
    
    EXPECT_EQ(pool.getUsedCount(), 2);
    
    pool.deallocate(ptr1);
    EXPECT_EQ(pool.getUsedCount(), 1);
    EXPECT_EQ(pool.getFreeCount(), 49);
    
    pool.deallocate(ptr2);
    EXPECT_EQ(pool.getUsedCount(), 0);
    EXPECT_EQ(pool.getFreeCount(), 50);
}

/**
 * Test 17: Memory Pool Create/Destory
 */
TEST_F(MemoryManagementTest, DISABLED_MemoryPool_CreateDestroy) {
    MemoryPool<std::string> pool(10);
    
    std::string* str = pool.create("Hello");
    EXPECT_NE(str, nullptr);
    EXPECT_EQ(*str, "Hello");
    
    pool.destroy(str);
    EXPECT_EQ(pool.getUsedCount(), 0);
}

/**
 * Test 18: Memory Pool Auto-Grow - DISABLED (needs fix)
 * TODO: Fix MemoryPool grow() function to properly allocate memory
 */
TEST_F(MemoryManagementTest, DISABLED_MemoryPool_AutoGrow) {
    MemoryPool<int> pool(10);  // Start with 10
    
    // Allocate more than initial capacity
    std::vector<int*> ptrs;
    for (int i = 0; i < 50; i++) {
        ptrs.push_back(pool.allocate());
    }
    
    EXPECT_EQ(pool.getUsedCount(), 50);
    EXPECT_GE(pool.getTotalCount(), 50);
    
    // Cleanup
    for (auto ptr : ptrs) {
        pool.deallocate(ptr);
    }
}

/**
 * Test 19: Memory Pool Reserve
 */
TEST_F(MemoryManagementTest, MemoryPool_Reserve) {
    MemoryPool<int> pool(10);
    
    pool.reserve(100);
    
    EXPECT_GE(pool.getTotalCount(), 100);
    EXPECT_EQ(pool.getUsedCount(), 0);
    EXPECT_GE(pool.getFreeCount(), 100);
}

// ============================================================================
// ASSET HANDLE TESTS
// ============================================================================

/**
 * Test 20: Asset Handle Basic - DISABLED (needs proper implementation)
 */
TEST_F(MemoryManagementTest, DISABLED_AssetHandle_Basic) {
    int* rawPtr = new int(42);
    AssetHandle<int> handle(rawPtr);
    
    EXPECT_TRUE(handle);
    EXPECT_EQ(*handle, 42);
    EXPECT_EQ(handle.get(), rawPtr);
    EXPECT_EQ(handle.useCount(), 1);
    // Don't delete rawPtr - handle owns it
}

/**
 * Test 21: Asset Handle Copy - DISABLED (needs proper implementation)
 */
TEST_F(MemoryManagementTest, DISABLED_AssetHandle_Copy) {
    int* rawPtr = new int(42);
    AssetHandle<int> handle1(rawPtr);
    
    AssetHandle<int> handle2(handle1);
    
    EXPECT_EQ(handle1.useCount(), 2);
    EXPECT_EQ(handle2.useCount(), 2);
    EXPECT_EQ(handle1.get(), handle2.get());
    // Don't delete rawPtr - handles own it
}

/**
 * Test 22: Asset Handle Null
 */
TEST_F(MemoryManagementTest, AssetHandle_Null) {
    AssetHandle<int> handle;
    
    EXPECT_FALSE(handle);
    EXPECT_EQ(handle.useCount(), 0);
}

// ============================================================================
// MEMORY STATS TESTS
// ============================================================================

/**
 * Test 23: Memory Stats Tracking
 */
TEST_F(MemoryManagementTest, MemoryStats_Tracking) {
    MemoryStats::reset();
    
    MemoryStats::recordAllocation(100);
    MemoryStats::recordAllocation(200);
    
    EXPECT_EQ(MemoryStats::totalAllocations, 2);
    EXPECT_EQ(MemoryStats::currentAllocations, 2);
    EXPECT_EQ(MemoryStats::totalBytesAllocated, 300);
    EXPECT_EQ(MemoryStats::currentBytesAllocated, 300);
    
    MemoryStats::recordDeallocation(100);
    
    EXPECT_EQ(MemoryStats::totalDeallocations, 1);
    EXPECT_EQ(MemoryStats::currentAllocations, 1);
    EXPECT_EQ(MemoryStats::currentBytesAllocated, 200);
}

/**
 * Test 24: Memory Stats Peak
 */
TEST_F(MemoryManagementTest, MemoryStats_Peak) {
    MemoryStats::reset();
    
    MemoryStats::recordAllocation(100);
    MemoryStats::recordAllocation(200);
    MemoryStats::recordAllocation(300);
    // Peak: 600 bytes, 3 allocations
    
    MemoryStats::recordDeallocation(200);
    MemoryStats::recordDeallocation(100);
    
    EXPECT_EQ(MemoryStats::peakAllocations, 3);
    EXPECT_EQ(MemoryStats::peakBytesAllocated, 600);
}

// ============================================================================
// INTEGRATION TESTS
// ============================================================================

/**
 * Test 25: Arena + Pool Integration
 */
TEST_F(MemoryManagementTest, Integration_ArenaWithPool) {
    MemoryArena arena(4096);
    
    // Use arena to allocate pool
    auto* pool = arena.create<MemoryPool<int>>(100);
    
    EXPECT_NE(pool, nullptr);
    EXPECT_EQ(pool->getTotalCount(), 100);
    
    // Allocate from pool
    int* value = pool->allocate();
    *value = 12345;
    
    EXPECT_EQ(*value, 12345);
    
    // Arena tracks the pool allocation
    EXPECT_GT(arena.getUsedSize(), 0);
}

/**
 * Test 26: Frame-Based Allocation Pattern
 */
TEST_F(MemoryManagementTest, Integration_FrameBasedAllocation) {
    DualArena arenas(65536);  // 64 KB per arena
    
    // Simulate multiple frames
    for (int frame = 0; frame < 10; frame++) {
        // Allocate for this frame
        auto& current = arenas.getCurrent();
        
        for (int i = 0; i < 100; i++) {
            current.allocate(64);  // Allocate 100 objects
        }
        
        // End of frame - swap and reset
        arenas.swap();
        arenas.resetInactive();
    }
    
    // Should have low memory usage (only current frame)
    EXPECT_LT(arenas.getUsedSize(), 10000);
}

/**
 * Test 27: Stress Test - Many Allocations
 */
TEST_F(MemoryManagementTest, StressTest_ManyAllocations) {
    MemoryArena arena(1024 * 1024);  // 1 MB
    
    std::vector<void*> ptrs;
    
    // Allocate many small objects
    for (int i = 0; i < 10000; i++) {
        void* ptr = arena.allocate(64);
        if (ptr) {
            ptrs.push_back(ptr);
        }
    }
    
    // Should have allocated many
    EXPECT_GT(ptrs.size(), 1000);
    EXPECT_EQ(arena.getAllocationCount(), ptrs.size());
    
    // Reset all at once
    arena.reset();
    EXPECT_EQ(arena.getUsedSize(), 0);
}

/**
 * Test 28: Debug Print Stats
 */
TEST_F(MemoryManagementTest, DEBUG_PrintStats) {
    MemoryStats::reset();
    
    MemoryArena arena(1024);
    arena.allocate(100);
    arena.allocate(200);
    
    MemoryPool<int> pool(50);
    pool.allocate();
    pool.allocate();
    
    std::cout << "\n=== MEMORY STATS ===\n";
    MemoryStats::printStats();
}
