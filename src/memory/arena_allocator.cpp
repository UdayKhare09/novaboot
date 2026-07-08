#include "novaboot/memory/arena_allocator.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <sys/mman.h>

namespace novaboot::memory {

ArenaAllocator::ArenaAllocator(std::size_t block_size)
    : block_size_(std::max(block_size, std::size_t{4096})) {
    allocate_new_block();
}

ArenaAllocator::~ArenaAllocator() {
    for (auto& block : blocks_) {
        unmap_pages(block.data, block.size);
    }
    for (auto& la : large_allocs_) {
        unmap_pages(la.ptr, la.size);
    }
}

void ArenaAllocator::reset() noexcept {
    // Free all blocks except the first one
    for (std::size_t i = 1; i < blocks_.size(); ++i) {
        unmap_pages(blocks_[i].data, blocks_[i].size);
    }

    // Free all large allocations
    for (auto& la : large_allocs_) {
        unmap_pages(la.ptr, la.size);
    }
    large_allocs_.clear();

    if (!blocks_.empty()) {
        // Keep the first block, reset pointer to its start
        blocks_.resize(1);
        current_ptr_ = blocks_[0].data;
        current_end_ = blocks_[0].data + blocks_[0].size;
    }
}

std::size_t ArenaAllocator::total_allocated() const noexcept {
    std::size_t total = 0;
    for (const auto& block : blocks_) {
        total += block.size;
    }
    for (const auto& la : large_allocs_) {
        total += la.size;
    }
    return total;
}

std::size_t ArenaAllocator::bytes_used() const noexcept {
    std::size_t used = 0;

    // All blocks except the current one are fully used
    if (blocks_.size() > 1) {
        for (std::size_t i = 0; i < blocks_.size() - 1; ++i) {
            used += blocks_[i].size;
        }
    }

    // Current block: used portion
    if (!blocks_.empty()) {
        used += static_cast<std::size_t>(
            current_ptr_ - blocks_.back().data);
    }

    // Large allocs are fully used
    for (const auto& la : large_allocs_) {
        used += la.size;
    }

    return used;
}

void* ArenaAllocator::do_allocate(std::size_t bytes, std::size_t alignment) {
    if (bytes == 0) {
        bytes = 1;
    }

    // Large allocations get their own mmap
    if (bytes >= kLargeAllocThreshold) {
        // Round up to page size
        const std::size_t page_size = 4096;
        const std::size_t alloc_size =
            (bytes + page_size - 1) & ~(page_size - 1);
        void* ptr = map_pages(alloc_size);
        large_allocs_.push_back({ptr, alloc_size});
        return ptr;
    }

    // Align the current pointer
    auto* aligned = reinterpret_cast<std::uint8_t*>(
        (reinterpret_cast<std::uintptr_t>(current_ptr_) + alignment - 1) &
        ~(alignment - 1));

    if (aligned + bytes > current_end_) {
        // Current block exhausted — allocate a new one
        // If the request is larger than block_size_, allocate a bigger block
        if (bytes + alignment > block_size_) {
            const std::size_t page_size = 4096;
            const std::size_t needed = bytes + alignment;
            const std::size_t alloc_size =
                (needed + page_size - 1) & ~(page_size - 1);
            void* ptr = map_pages(alloc_size);
            large_allocs_.push_back({ptr, alloc_size});
            return ptr;
        }

        allocate_new_block();
        aligned = reinterpret_cast<std::uint8_t*>(
            (reinterpret_cast<std::uintptr_t>(current_ptr_) + alignment - 1) &
            ~(alignment - 1));
    }

    current_ptr_ = aligned + bytes;
    return aligned;
}

void ArenaAllocator::do_deallocate(
    [[maybe_unused]] void* ptr,
    [[maybe_unused]] std::size_t bytes,
    [[maybe_unused]] std::size_t alignment) {
    // No-op: arena allocations are freed in bulk via reset()
}

bool ArenaAllocator::do_is_equal(
    const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

void ArenaAllocator::allocate_new_block() {
    void* ptr = map_pages(block_size_);
    auto* data = static_cast<std::uint8_t*>(ptr);
    blocks_.push_back({data, block_size_});
    current_ptr_ = data;
    current_end_ = data + block_size_;
}

void* ArenaAllocator::map_pages(std::size_t size) {
    void* ptr = ::mmap(nullptr, size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                       -1, 0);
    if (ptr == MAP_FAILED) {
        throw std::bad_alloc();
    }
    return ptr;
}

void ArenaAllocator::unmap_pages(void* ptr, std::size_t size) {
    ::munmap(ptr, size);
}

} // namespace novaboot::memory
