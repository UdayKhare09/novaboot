#include <gtest/gtest.h>
#include "novaboot/memory/arena_allocator.h"
#include <vector>

using namespace novaboot::memory;

TEST(ArenaAllocatorTest, BasicAllocation) {
    ArenaAllocator arena(4096);
    EXPECT_EQ(arena.bytes_used(), 0);

    void* p1 = arena.allocate(10, 8);
    ASSERT_NE(p1, nullptr);
    EXPECT_TRUE((reinterpret_cast<uintptr_t>(p1) % 8) == 0);
    EXPECT_GE(arena.bytes_used(), 10);

    void* p2 = arena.allocate(20, 16);
    ASSERT_NE(p2, nullptr);
    EXPECT_TRUE((reinterpret_cast<uintptr_t>(p2) % 16) == 0);
    EXPECT_GE(arena.bytes_used(), 30);
}

TEST(ArenaAllocatorTest, ResetAndReuse) {
    ArenaAllocator arena(4096);
    void* p1 = arena.allocate(100, 8);
    size_t used_before = arena.bytes_used();
    EXPECT_GT(used_before, 0);

    arena.reset();
    EXPECT_EQ(arena.bytes_used(), 0);

    void* p2 = arena.allocate(100, 8);
    // Should reuse the first block and get the same or similar address
    EXPECT_EQ(p2, p1);
}

TEST(ArenaAllocatorTest, LargeAllocation) {
    ArenaAllocator arena(4096);
    // Request that exceeds large allocation threshold (32KB)
    void* p1 = arena.allocate(40000, 8);
    ASSERT_NE(p1, nullptr);
    EXPECT_GE(arena.total_allocated(), 40000);
}

TEST(ArenaAllocatorTest, MultipleBlocks) {
    ArenaAllocator arena(1024); // Small block size to force new block allocations
    std::vector<void*> ptrs;
    for (int i = 0; i < 10; ++i) {
        void* p = arena.allocate(256, 8);
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }
    EXPECT_GT(arena.total_allocated(), 1024);
}
