#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

namespace novaboot::memory {

/// Arena allocator for per-connection memory management.
///
/// Uses bump-pointer allocation from large mmap'd blocks. All allocations
/// are freed in bulk when the arena is reset (connection close).
/// Satisfies std::pmr::memory_resource for STL container compatibility.
///
/// Thread-safety: NOT thread-safe. Designed for thread-per-core model
/// where each connection is owned by exactly one thread.
class ArenaAllocator final : public std::pmr::memory_resource {
public:
    /// Default block size: 64KB (fits in L1/L2 cache nicely)
    static constexpr std::size_t kDefaultBlockSize = 64 * 1024;

    /// Threshold above which we do a dedicated mmap for a single allocation
    static constexpr std::size_t kLargeAllocThreshold = 32 * 1024;

    explicit ArenaAllocator(std::size_t block_size = kDefaultBlockSize);
    ~ArenaAllocator() override;

    // Non-copyable, non-movable (addresses must remain stable)
    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;
    ArenaAllocator(ArenaAllocator&&) = delete;
    ArenaAllocator& operator=(ArenaAllocator&&) = delete;

    /// Reset the arena — invalidates ALL previous allocations.
    /// O(1) for the common case (single block), O(n) for multi-block.
    void reset() noexcept;

    /// Returns total bytes allocated from the OS (blocks + large allocs)
    [[nodiscard]] std::size_t total_allocated() const noexcept;

    /// Returns bytes currently used within the arena
    [[nodiscard]] std::size_t bytes_used() const noexcept;

protected:
    // std::pmr::memory_resource interface
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* ptr, std::size_t bytes,
                       std::size_t alignment) override;
    bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override;

private:
    struct Block {
        std::uint8_t* data;
        std::size_t   size;
    };

    struct LargeAlloc {
        void*       ptr;
        std::size_t size;
    };

    void allocate_new_block();
    static void* map_pages(std::size_t size);
    static void  unmap_pages(void* ptr, std::size_t size);

    std::size_t block_size_;

    // Current block state
    std::uint8_t* current_ptr_  = nullptr;
    std::uint8_t* current_end_  = nullptr;

    // All blocks (including current)
    std::vector<Block> blocks_;

    // Oversized allocations that got their own mmap
    std::vector<LargeAlloc> large_allocs_;
};

} // namespace novaboot::memory
