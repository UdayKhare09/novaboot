#pragma once

/// @file novaboot/di/inject.h
/// Injection wrapper types for NovaBoot DI.
///
/// These types are used in constructor parameters to express how a dependency
/// should be resolved:
///
///   class MyService {
///   public:
///       explicit MyService(
///           UserRepository& repo,           // regular: resolved by type
///           Lazy<HeavyService>& heavy,      // lazy: resolved on first .get()
///           Optional<MetricsService>& metrics, // optional: null if not registered
///           All<Cache>& caches              // all implementations of Cache
///       );
///   };

#include "novaboot/di/container.h"
#include <memory>
#include <span>
#include <vector>

namespace novaboot::di {

// ─────────────────────────────────────────────────────────────────────────────
// Lazy<T> — resolved on first .get() call
// ─────────────────────────────────────────────────────────────────────────────

/// Wraps a bean that should be initialized on first access rather than at
/// construction time.
///
/// Usage in constructor:
///   explicit MyService(Lazy<HeavyAnalytics>& analytics) : analytics_(analytics) {}
///   Lazy<HeavyAnalytics>& analytics_;
///
/// Access:
///   analytics_.get().run_report();
///   analytics_->run_report();   // via operator->
template<typename T>
class Lazy {
public:
    explicit Lazy(ContainerBase& container) : container_(&container) {}

    // Non-copyable to prevent accidental copies
    Lazy(const Lazy&) = delete;
    Lazy& operator=(const Lazy&) = delete;

    // Movable
    Lazy(Lazy&&) = default;
    Lazy& operator=(Lazy&&) = default;

    /// Resolve and cache the instance on first call. Thread-safe within a single shard.
    T& get() {
        if (!instance_) {
            instance_ = &container_->resolve<T>();
        }
        return *instance_;
    }

    T* operator->() { return &get(); }
    T& operator*()  { return  get(); }

    /// Returns true if the bean has already been resolved.
    bool is_resolved() const noexcept { return instance_ != nullptr; }

private:
    ContainerBase* container_;
    T* instance_ = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// Optional<T> — null if not registered
// ─────────────────────────────────────────────────────────────────────────────

/// Wraps an optional dependency that may or may not be registered.
///
/// Usage:
///   explicit MyService(Optional<MetricsService>& metrics) : metrics_(metrics) {}
///   if (metrics_) metrics_->record("event");
template<typename T>
class Optional {
public:
    explicit Optional(ContainerBase& container) {
        if (container.has<T>()) {
            instance_ = &container.resolve<T>();
        }
    }

    explicit Optional(T* ptr) : instance_(ptr) {}
    Optional() = default;

    [[nodiscard]] T* get() const noexcept { return instance_; }
    [[nodiscard]] bool has_value() const noexcept { return instance_ != nullptr; }

    T* operator->() const { return instance_; }
    T& operator*()  const { return *instance_; }
    explicit operator bool() const noexcept { return has_value(); }

private:
    T* instance_ = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// All<T> — all registered implementations of an interface
// ─────────────────────────────────────────────────────────────────────────────

/// Collects all beans that are registered as implementations of T.
/// Requires that all implementations were registered with their own type_id
/// AND as T (via an interface registration or explicit multi-registration).
///
/// Typically used in conjunction with codegen-produced registration.
template<typename T>
class All {
public:
    explicit All(std::vector<T*> instances) : instances_(std::move(instances)) {}
    All() = default;

    std::span<T* const> all() const noexcept { return instances_; }

    auto begin() const noexcept { return instances_.begin(); }
    auto end()   const noexcept { return instances_.end(); }

    [[nodiscard]] bool empty() const noexcept { return instances_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return instances_.size(); }

    T* operator[](std::size_t i) const { return instances_[i]; }

private:
    std::vector<T*> instances_;
};

} // namespace novaboot::di
