#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <vector>

#include <sys/mman.h>

namespace novaboot::memory {

/// Fixed-size object pool using a free-list.
///
/// Pre-allocates N objects and uses an intrusive free-list for O(1)
/// alloc/dealloc. Thread-local in thread-per-core model (no sync needed).
///
/// Usage:
///   PoolAllocator<QuicConnection> pool(256);
///   auto* conn = pool.allocate();
///   // use conn...
///   pool.deallocate(conn);
template <typename T>
class PoolAllocator {
public:
    explicit PoolAllocator(std::size_t capacity)
        : capacity_(capacity) {
        static_assert(sizeof(T) >= sizeof(FreeNode*),
                      "Object must be at least pointer-sized for free-list");

        // Allocate backing storage via mmap for page-alignment
        const std::size_t slot_size = std::max(sizeof(T), alignof(T));
        const std::size_t total = slot_size * capacity_;
        const std::size_t page_size = 4096;
        storage_size_ = (total + page_size - 1) & ~(page_size - 1);

        storage_ = static_cast<std::uint8_t*>(
            ::mmap(nullptr, storage_size_,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                   -1, 0));

        if (storage_ == MAP_FAILED) {
            throw std::bad_alloc();
        }

        // Build free list
        free_head_ = nullptr;
        for (std::size_t i = capacity_; i > 0; --i) {
            auto* node = reinterpret_cast<FreeNode*>(
                storage_ + (i - 1) * slot_size);
            node->next = free_head_;
            free_head_ = node;
        }
    }

    ~PoolAllocator() {
        if (storage_ && storage_ != MAP_FAILED) {
            ::munmap(storage_, storage_size_);
        }
    }

    // Non-copyable, non-movable
    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
    PoolAllocator(PoolAllocator&&) = delete;
    PoolAllocator& operator=(PoolAllocator&&) = delete;

    /// Allocate a slot (does NOT construct the object).
    /// Returns nullptr if the pool is exhausted.
    [[nodiscard]] T* allocate() noexcept {
        if (!free_head_) {
            return nullptr;
        }

        auto* node = free_head_;
        free_head_ = node->next;
        ++in_use_;
        return reinterpret_cast<T*>(node);
    }

    /// Deallocate a slot (does NOT destruct the object).
    /// Caller must call destructor before deallocating.
    void deallocate(T* ptr) noexcept {
        assert(ptr != nullptr);
        assert(owns(ptr));

        auto* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = free_head_;
        free_head_ = node;
        --in_use_;
    }

    /// Construct an object in-place from the pool.
    template <typename... Args>
    [[nodiscard]] T* construct(Args&&... args) {
        T* ptr = allocate();
        if (!ptr) {
            return nullptr;
        }
        return ::new (static_cast<void*>(ptr)) T(std::forward<Args>(args)...);
    }

    /// Destroy and return to pool.
    void destroy(T* ptr) noexcept {
        if (ptr) {
            ptr->~T();
            deallocate(ptr);
        }
    }

    /// Check if a pointer belongs to this pool
    [[nodiscard]] bool owns(const T* ptr) const noexcept {
        const auto* p = reinterpret_cast<const std::uint8_t*>(ptr);
        return p >= storage_ && p < storage_ + storage_size_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t in_use() const noexcept { return in_use_; }
    [[nodiscard]] std::size_t available() const noexcept {
        return capacity_ - in_use_;
    }

private:
    struct FreeNode {
        FreeNode* next;
    };

    std::uint8_t* storage_      = nullptr;
    std::size_t   storage_size_ = 0;
    std::size_t   capacity_     = 0;
    std::size_t   in_use_       = 0;
    FreeNode*     free_head_    = nullptr;
};

} // namespace novaboot::memory
