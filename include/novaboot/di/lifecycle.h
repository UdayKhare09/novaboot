#pragma once

/// @file novaboot/di/lifecycle.h
/// Lifecycle interfaces for NovaBoot DI beans.
///
/// Beans can implement these interfaces OR use [[=post_construct{}]]/[[=pre_destroy{}]]
/// attribute annotations. The container checks both.

#include <functional>
#include <vector>
#include <typeindex>

namespace novaboot::di {

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle interfaces (interface-based, no reflection needed)
// ─────────────────────────────────────────────────────────────────────────────

/// Implement this to receive a callback after construction and dep injection.
/// Equivalent to Spring InitializingBean / @PostConstruct.
class Initializable {
public:
    virtual ~Initializable() = default;
    virtual void post_construct() = 0;
};

/// Implement this to receive a callback just before destruction.
/// Equivalent to Spring DisposableBean / @PreDestroy.
class Destroyable {
public:
    virtual ~Destroyable() = default;
    virtual void pre_destroy() = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle record (internal — one per managed bean instance)
// ─────────────────────────────────────────────────────────────────────────────

/// A type-erased lifecycle record attached to each bean instance in the container.
struct LifecycleEntry {
    void* instance = nullptr;

    /// Invokes post_construct (null if bean has no lifecycle callbacks).
    std::function<void(void*)> on_post_construct;

    /// Invokes pre_destroy (null if bean has no lifecycle callbacks).
    std::function<void(void*)> on_pre_destroy;
};

// ─────────────────────────────────────────────────────────────────────────────
// LifecycleManager
// ─────────────────────────────────────────────────────────────────────────────

/// Tracks and drives lifecycle callbacks for all beans in a container.
///
/// Beans are registered in construction order.
/// pre_destroy callbacks are fired in REVERSE construction order (Spring behaviour).
class LifecycleManager {
public:
    LifecycleManager() = default;

    // Non-copyable (each container owns its own lifecycle manager)
    LifecycleManager(const LifecycleManager&) = delete;
    LifecycleManager& operator=(const LifecycleManager&) = delete;

    // Movable (used during container construction)
    LifecycleManager(LifecycleManager&&) = default;
    LifecycleManager& operator=(LifecycleManager&&) = default;

    /// Register a bean instance with its lifecycle callbacks.
    /// Both callbacks may be null.
    void register_bean(void*                           instance,
                       std::function<void(void*)>      on_post_construct,
                       std::function<void(void*)>      on_pre_destroy);

    /// Convenience: register a bean instance typed as T.
    /// Checks for Initializable / Destroyable interfaces.
    template<typename T>
    void register_bean(T* instance,
                       std::function<void(void*)> on_post_construct = {},
                       std::function<void(void*)> on_pre_destroy    = {}) {
        // Merge interface-based and annotation-based callbacks
        std::function<void(void*)> pc = std::move(on_post_construct);
        std::function<void(void*)> pd = std::move(on_pre_destroy);

        if constexpr (std::is_base_of_v<Initializable, T>) {
            auto prev_pc = std::move(pc);
            pc = [prev = std::move(prev_pc)](void* p) {
                if (prev) prev(p);
                static_cast<Initializable*>(static_cast<T*>(p))->post_construct();
            };
        }
        if constexpr (std::is_base_of_v<Destroyable, T>) {
            auto prev_pd = std::move(pd);
            pd = [prev = std::move(prev_pd)](void* p) {
                static_cast<Destroyable*>(static_cast<T*>(p))->pre_destroy();
                if (prev) prev(p);
            };
        }

        register_bean(static_cast<void*>(instance), std::move(pc), std::move(pd));
    }

    /// Invoke all post_construct callbacks in registration order.
    void invoke_post_constructs();

    /// Invoke all pre_destroy callbacks in REVERSE registration order.
    void invoke_pre_destroys();

    std::size_t size() const noexcept { return entries_.size(); }

private:
    std::vector<LifecycleEntry> entries_;
};

} // namespace novaboot::di
