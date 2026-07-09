#pragma once

/// @file novaboot/di/container.h
/// The NovaBoot DI container hierarchy.
///
/// Four containers form a parent→child tree:
///
///   RootContainer (singletons, built once at startup, immutable after build())
///     └── ShardContainer (per-CPU-core, owns Shard-scoped beans)
///           ├── RequestContainer (per HTTP/3 stream, arena-allocated)
///           └── ConnectionContainer (per QUIC connection, arena-allocated)
///
/// Thread Safety:
///   - RootContainer is read-only after build() → safe to access from any thread.
///   - ShardContainer is thread-local (one per shard thread) → no locks.
///   - RequestContainer/ConnectionContainer are single-threaded (per-shard).

#include "novaboot/di/attributes.h"
#include "novaboot/di/lifecycle.h"
#include "novaboot/di/scope.h"

#include <any>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace novaboot::memory { class ArenaAllocator; }  // forward decl

namespace novaboot::di {

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────

class ContainerBase;
class RootContainer;
class ShardContainer;
class RequestContainer;
class ConnectionContainer;

// ─────────────────────────────────────────────────────────────────────────────
// DIError — thrown at build() time only (never in hot path)
// ─────────────────────────────────────────────────────────────────────────────

class DIError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ─────────────────────────────────────────────────────────────────────────────
// BeanRegistration — internal descriptor for a single bean
// ─────────────────────────────────────────────────────────────────────────────

struct BeanRegistration {
    std::type_index            type_id{typeid(void)};  // default to void (overwritten on registration)
    std::string                name;           ///< Type name for diagnostics
    std::string                qualifier;      ///< Named qualifier (empty = unqualified)
    Scope                      scope  = Scope::Singleton;
    bool                       is_primary = false;
    bool                       is_lazy    = false;
    bool                       is_async   = false;
    std::uint32_t              async_timeout_ms = 30'000u;

    /// Factory that creates the bean instance (void*-owned).
    /// For Singleton: called once at build(). For Prototype: called per resolve_new().
    std::function<void*(ContainerBase&)> factory;

    /// Async factory: returns a future<void*>. Used when is_async == true.
    std::function<std::future<void*>(ContainerBase&)> async_factory;

    /// Lifecycle callbacks (null if none)
    std::function<void(void*)> post_construct_fn;
    std::function<void(void*)> pre_destroy_fn;

    /// Destructor for the bean (called when the owning container is destroyed)
    std::function<void(void*)> destructor_fn;

    /// Dependency type_ids (for cycle detection at build time)
    std::vector<std::type_index> dep_type_ids;
};

// ─────────────────────────────────────────────────────────────────────────────
// ContainerBase — shared resolve logic
// ─────────────────────────────────────────────────────────────────────────────

class ContainerBase {
public:
    virtual ~ContainerBase() = default;

    /// Resolve a Singleton or scoped-singleton bean by type.
    /// Returns a reference to the cached instance.
    template<typename T>
    T& resolve() {
        auto tid = std::type_index(typeid(T));
        auto it = instances_.find(tid);
        if (it != instances_.end())
            return *static_cast<T*>(it->second);

        // Check for a lazy singleton in our own registrations
        auto reg_it = registrations_.find(tid);
        if (reg_it != registrations_.end()
            && reg_it->second.is_lazy
            && reg_it->second.scope != Scope::Prototype) {
            void* raw = resolve_lazy_impl(tid);
            if (raw) return *static_cast<T*>(raw);
        }

        // Delegate to parent (Shard → Root)
        if (parent_) return parent_->resolve<T>();
        throw DIError(std::string("novaboot::di: Bean not found: ") + typeid(T).name()
                    + ". Did you forget to register it?");
    }

    /// Resolve by type and named qualifier.
    template<typename T>
    T& resolve_named(const char* qualifier_name) {
        auto key = make_qualified_key(typeid(T), qualifier_name);
        auto it = qualified_instances_.find(key);
        if (it != qualified_instances_.end()) {
            return *static_cast<T*>(it->second);
        }
        if (parent_) return parent_->resolve_named<T>(qualifier_name);
        throw DIError(std::string("novaboot::di: Bean not found: ") + typeid(T).name()
                    + " with qualifier '" + qualifier_name + "'");
    }

    /// Create a new Prototype instance (caller owns it).
    template<typename T>
    std::unique_ptr<T> resolve_new() {
        auto* reg = find_registration(std::type_index(typeid(T)));
        if (!reg || reg->scope != Scope::Prototype) {
            throw DIError(std::string("novaboot::di: resolve_new<T> requires Prototype scope for: ")
                        + typeid(T).name());
        }
        void* raw = reg->factory(*this);
        if (reg->post_construct_fn) reg->post_construct_fn(raw);
        auto ptr = std::unique_ptr<T>(static_cast<T*>(raw));
        return ptr;
    }

    /// Check if a bean of type T is registered.
    template<typename T>
    bool has() const {
        if (instances_.contains(std::type_index(typeid(T)))) return true;
        if (registrations_.contains(std::type_index(typeid(T)))) return true;
        if (parent_) return parent_->has<T>();
        return false;
    }

    ContainerBase* parent() noexcept { return parent_; }

protected:
    ContainerBase() = default;
    explicit ContainerBase(ContainerBase* parent) : parent_(parent) {}

    ContainerBase* parent_ = nullptr;

    // Cached instances (non-qualified)
    std::unordered_map<std::type_index, void*> instances_;

    // Cached instances (qualified: "TypeID:qualifier_name")
    std::unordered_map<std::string, void*> qualified_instances_;

    // Bean registrations (used for Prototype, Lazy, and internal lookup)
    std::unordered_map<std::type_index, BeanRegistration> registrations_;

    // Lazy state
    std::unordered_map<std::type_index, bool> lazy_initialized_;

    LifecycleManager lifecycle_;

    BeanRegistration* find_registration(std::type_index tid) {
        auto it = registrations_.find(tid);
        if (it != registrations_.end()) return &it->second;
        if (parent_) return parent_->find_registration(tid);
        return nullptr;
    }

    static std::string make_qualified_key(const std::type_info& ti,
                                          const char* qualifier) {
        return std::string(ti.name()) + ":" + qualifier;
    }

    /// Initialize a Singleton (or scoped) bean: run factory, cache, register lifecycle.
    void* init_singleton(BeanRegistration& reg);

    /// Virtual hook for lazy singleton resolution.
    /// Default: returns nullptr (subclass overrides for containers that own lazy beans).
    virtual void* resolve_lazy_impl(std::type_index /*tid*/) { return nullptr; }
};

// ─────────────────────────────────────────────────────────────────────────────
// LifecycleInvoker — static helper to call annotated functions at runtime
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {
template<typename T, typename Ann>
consteval auto get_lifecycle_methods() {
    constexpr auto ctx = std::meta::access_context::current();
    struct ArrayWrapper {
        std::meta::info data[32] = {};
        std::size_t     size = 0;

        consteval const std::meta::info* begin() const noexcept { return data; }
        consteval const std::meta::info* end() const noexcept { return data + size; }
    };
    ArrayWrapper result;

    for (auto m : std::meta::members_of(^^T, ctx)) {
        if (std::meta::is_function(m) && !std::meta::is_constructor(m) && !std::meta::is_destructor(m)) {
            if (!std::meta::annotations_of_with_type(m, ^^Ann).empty()) {
                if (result.size < 32) {
                    result.data[result.size++] = m;
                }
            }
        }
    }
    return result;
}

#ifdef __cpp_impl_reflection
template<typename T>
consteval auto get_ctor_params() {
    constexpr auto ctor = novaboot::di::detail::find_inject_ctor(^^T);
    
    struct ParamArray {
        std::meta::info data[32] = {};
        std::size_t     size = 0;
    };
    ParamArray result;

    for (auto p : std::meta::parameters_of(ctor)) {
        if (result.size < 32) {
            result.data[result.size++] = p;
        }
    }
    return result;
}

template<typename T, auto Array, typename Indices>
struct FactoryRegistrarImpl;
#endif
} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// RootContainer — owns all Singleton beans
// ─────────────────────────────────────────────────────────────────────────────

class RootContainer : public ContainerBase {
public:
    RootContainer() = default;
    ~RootContainer() override;

    // Non-copyable, non-movable after build()
    RootContainer(const RootContainer&) = delete;
    RootContainer& operator=(const RootContainer&) = delete;

    // ── Registration API ────────────────────────────────────────────────────

    /// Register a bean explicitly with a factory lambda.
    /// The factory receives a ContainerBase& to resolve dependencies.
    ///
    ///   root.register_bean<MyService>(
    ///       [](auto& c) { return new MyService{c.template resolve<Dep>()}; }
    ///   );
    template<typename T>
    RootContainer& register_bean(
            std::function<T*(ContainerBase&)>  factory,
            Scope                               scope     = Scope::Singleton,
            const char*                         qualifier = "",
            bool                                is_primary = false,
            bool                                is_lazy    = false) {
        BeanRegistration reg;
        reg.type_id    = std::type_index(typeid(T));
        reg.name       = typeid(T).name();
        reg.qualifier  = qualifier ? qualifier : "";
        reg.scope      = scope;
        reg.is_primary = is_primary;
        reg.is_lazy    = is_lazy;
        reg.factory    = [f = std::move(factory)](ContainerBase& c) -> void* {
            return f(c);
        };
        reg.destructor_fn = [](void* p) { delete static_cast<T*>(p); };

        // Auto-wire Initializable / Destroyable interface callbacks
        if constexpr (std::is_base_of_v<Initializable, T>) {
            reg.post_construct_fn = [](void* p) {
                static_cast<Initializable*>(static_cast<T*>(p))->post_construct();
            };
        }
#ifdef __cpp_impl_reflection
        else if constexpr (std::is_class_v<T>) {
            static constexpr auto info = detail::get_lifecycle_methods<T, novaboot::di::post_construct>();
            if constexpr (info.size > 0) {
                reg.post_construct_fn = [](void* p) {
                    auto* obj = static_cast<T*>(p);
                    template for (constexpr auto m : info) {
                        (obj->* &[:m:])();
                    }
                };
            }
        }
#endif

        if constexpr (std::is_base_of_v<Destroyable, T>) {
            reg.pre_destroy_fn = [](void* p) {
                static_cast<Destroyable*>(static_cast<T*>(p))->pre_destroy();
            };
        }
#ifdef __cpp_impl_reflection
        else if constexpr (std::is_class_v<T>) {
            static constexpr auto info = detail::get_lifecycle_methods<T, novaboot::di::pre_destroy>();
            if constexpr (info.size > 0) {
                reg.pre_destroy_fn = [](void* p) {
                    auto* obj = static_cast<T*>(p);
                    template for (constexpr auto m : info) {
                        (obj->* &[:m:])();
                    }
                };
            }
        }
#endif

        registrations_[reg.type_id] = std::move(reg);
        return *this;
    }

    /// Register a component class using compile-time reflection.
    /// The constructor dependencies are auto-wired automatically.
    template<typename T>
    RootContainer& register_component() {
#ifdef __cpp_impl_reflection
        constexpr auto cls = ^^T;
        constexpr auto scope = novaboot::di::detail::get_scope(cls);
        constexpr bool is_lazy = novaboot::di::detail::is_lazy_bean(cls);
        constexpr bool is_prim = novaboot::di::detail::is_primary_bean(cls);
        constexpr const char* q = novaboot::di::detail::get_named_qualifier(cls);

        constexpr auto params = detail::get_ctor_params<T>();
        detail::FactoryRegistrarImpl<T, params, std::make_index_sequence<params.size>>::register_in(
            *this, scope, q, is_prim, is_lazy
        );
#else
        // Fallback without reflection
        static_assert(sizeof(T) == 0, "novaboot::di: register_component requires C++26 static reflection.");
#endif
        return *this;
    }

    /// Register multiple component classes at once using compile-time reflection.
    template<typename... Ts>
    RootContainer& register_components() {
        (register_component<Ts>(), ...);
        return *this;
    }

    /// Register an async bean (factory returns std::future<T*>).
    template<typename T>
    RootContainer& register_async_bean(
            std::function<std::future<T*>(ContainerBase&)> async_factory,
            std::uint32_t timeout_ms = 30'000u,
            const char*   qualifier  = "") {
        BeanRegistration reg;
        reg.type_id          = std::type_index(typeid(T));
        reg.name             = typeid(T).name();
        reg.qualifier        = qualifier ? qualifier : "";
        reg.scope            = Scope::Singleton;
        reg.is_async         = true;
        reg.async_timeout_ms = timeout_ms;
        reg.async_factory    = [f = std::move(async_factory)](ContainerBase& c) {
            return std::async(std::launch::async, [&f, &c] {
                return static_cast<void*>(f(c).get());
            });
        };
        reg.destructor_fn = [](void* p) { delete static_cast<T*>(p); };
        registrations_[reg.type_id] = std::move(reg);
        return *this;
    }

    /// Add a post_construct callback for bean type T (chains with existing callbacks).
    template<typename T>
    RootContainer& with_post_construct(std::function<void(T&)> fn) {
        auto it = registrations_.find(std::type_index(typeid(T)));
        if (it == registrations_.end())
            throw DIError(std::string("novaboot::di: No registration for ") + typeid(T).name());
        auto prev = std::move(it->second.post_construct_fn);
        it->second.post_construct_fn = [prev = std::move(prev), f = std::move(fn)](void* p) {
            if (prev) prev(p);
            f(*static_cast<T*>(p));
        };
        return *this;
    }

    /// Add a pre_destroy callback for bean type T (chains with existing callbacks).
    template<typename T>
    RootContainer& with_pre_destroy(std::function<void(T&)> fn) {
        auto it = registrations_.find(std::type_index(typeid(T)));
        if (it == registrations_.end())
            throw DIError(std::string("novaboot::di: No registration for ") + typeid(T).name());
        auto prev = std::move(it->second.pre_destroy_fn);
        it->second.pre_destroy_fn = [prev = std::move(prev), f = std::move(fn)](void* p) {
            f(*static_cast<T*>(p));
            if (prev) prev(p);  // interface-based runs after callback (matches Spring order)
        };
        return *this;
    }

    // ── Build ───────────────────────────────────────────────────────────────

    /// Finalize the container:
    ///   1. Detect circular dependencies (throws DIError if found)
    ///   2. Instantiate all non-lazy Singleton beans in dependency order
    ///   3. Await all async beans (with timeout)
    ///   4. Invoke all post_construct callbacks
    ///
    /// After build() returns, the container is immutable for singletons.
    void build();

    /// Signal that the application is shutting down.
    /// Invokes all pre_destroy callbacks in reverse order, then destroys beans.
    void shutdown();

    // ── Child container factories ────────────────────────────────────────────

    /// Create a per-shard container (borrows read-only refs from this container).
    std::unique_ptr<ShardContainer> make_shard_container();

    [[nodiscard]] bool is_built() const noexcept { return built_; }

private:
    bool built_ = false;

    /// Topological sort of registrations (dependency order).
    std::vector<std::type_index> topo_order_;

    /// Detect cycles using DFS. Throws DIError if a cycle is found.
    void detect_cycles();

    /// Build topological order (Kahn's algorithm).
    void build_topo_order();

    /// Instantiate a single bean (handles async, lazy).
    void* instantiate(BeanRegistration& reg);

    /// Resolve a lazy singleton — creates+caches it on first access.
    /// Returns nullptr if tid is not a registered lazy singleton.
    void* resolve_lazy_singleton(std::type_index tid);

    /// Override: handle lazy singletons owned by this RootContainer.
    void* resolve_lazy_impl(std::type_index tid) override {
        return resolve_lazy_singleton(tid);
    }

    /// Storage for owned bean instances (for destruction)
    std::vector<std::pair<void*, std::function<void(void*)>>> owned_instances_;
};

namespace detail {
#ifdef __cpp_impl_reflection
template<typename T, auto Array, std::size_t... Is>
struct FactoryRegistrarImpl<T, Array, std::index_sequence<Is...>> {
    static void register_in(RootContainer& root, Scope scope, const char* q, bool is_prim, bool is_lazy) {
        root.register_bean<T>(
            [](ContainerBase& c) -> T* {
                return new T{ c.resolve<
                    typename[: 
                        std::meta::remove_const(
                            std::meta::is_reference_type(std::meta::type_of(Array.data[Is])) 
                            ? std::meta::remove_reference(std::meta::type_of(Array.data[Is]))
                            : std::meta::type_of(Array.data[Is])
                        )
                    :]>()
                ... };
            },
            scope, q, is_prim, is_lazy
        );
    }
};
#endif
} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// ShardContainer — per-CPU-core, borrows singletons, owns Shard-scoped beans
// ─────────────────────────────────────────────────────────────────────────────

class ShardContainer : public ContainerBase {
public:
    explicit ShardContainer(RootContainer& root)
        : ContainerBase(&root) {}

    ~ShardContainer() override;

    // Non-copyable
    ShardContainer(const ShardContainer&) = delete;
    ShardContainer& operator=(const ShardContainer&) = delete;

    /// Register a Shard-scoped bean (one instance per shard, not shared).
    template<typename T>
    ShardContainer& register_bean(std::function<T*(ContainerBase&)> factory) {
        BeanRegistration reg;
        reg.type_id = std::type_index(typeid(T));
        reg.name    = typeid(T).name();
        reg.scope   = Scope::Singleton;  // within this shard
        reg.factory = [f = std::move(factory)](ContainerBase& c) -> void* { return f(c); };
        reg.destructor_fn = [](void* p) { delete static_cast<T*>(p); };
        registrations_[reg.type_id] = std::move(reg);
        return *this;
    }

    /// Initialize all Shard-scoped beans (called after shard thread starts).
    void initialize();

    /// Create a request-scoped child container.
    std::unique_ptr<RequestContainer> make_request_container();

    /// Create a connection-scoped child container.
    std::unique_ptr<ConnectionContainer> make_connection_container();

private:
    std::vector<std::pair<void*, std::function<void(void*)>>> owned_instances_;
};

// ─────────────────────────────────────────────────────────────────────────────
// RequestContainer — per HTTP/3 stream
// ─────────────────────────────────────────────────────────────────────────────

class RequestContainer : public ContainerBase {
public:
    explicit RequestContainer(ShardContainer& parent)
        : ContainerBase(&parent) {}

    ~RequestContainer() override;

    // Non-copyable, non-movable
    RequestContainer(const RequestContainer&) = delete;
    RequestContainer& operator=(const RequestContainer&) = delete;

    /// Register a Request-scoped bean.
    template<typename T>
    RequestContainer& register_bean(std::function<T*(ContainerBase&)> factory) {
        BeanRegistration reg;
        reg.type_id       = std::type_index(typeid(T));
        reg.name          = typeid(T).name();
        reg.scope         = Scope::Request;
        reg.factory       = [f = std::move(factory)](ContainerBase& c) -> void* { return f(c); };
        reg.destructor_fn = [](void* p) { delete static_cast<T*>(p); };
        registrations_[reg.type_id] = std::move(reg);
        return *this;
    }

    /// Lazily resolve a Request-scoped bean (created on first access).
    template<typename T>
    T& resolve() {
        auto tid = std::type_index(typeid(T));
        auto it = instances_.find(tid);
        if (it != instances_.end()) return *static_cast<T*>(it->second);

        // Try to create from registration
        auto* reg = find_registration(tid);
        if (reg && reg->scope == Scope::Request) {
            void* raw = reg->factory(*this);
            if (reg->post_construct_fn) reg->post_construct_fn(raw);
            lifecycle_.register_bean(static_cast<T*>(raw), {}, reg->pre_destroy_fn);
            instances_[tid] = raw;
            owned_instances_.emplace_back(raw, reg->destructor_fn);
            return *static_cast<T*>(raw);
        }

        // Delegate to parent (Shard or Root for Singletons)
        return ContainerBase::resolve<T>();
    }

private:
    std::vector<std::pair<void*, std::function<void(void*)>>> owned_instances_;
};

// ─────────────────────────────────────────────────────────────────────────────
// ConnectionContainer — per QUIC connection
// ─────────────────────────────────────────────────────────────────────────────

class ConnectionContainer : public ContainerBase {
public:
    explicit ConnectionContainer(ShardContainer& parent)
        : ContainerBase(&parent) {}

    ~ConnectionContainer() override;

    // Non-copyable
    ConnectionContainer(const ConnectionContainer&) = delete;
    ConnectionContainer& operator=(const ConnectionContainer&) = delete;

    /// Register a Connection-scoped bean.
    template<typename T>
    ConnectionContainer& register_bean(std::function<T*(ContainerBase&)> factory) {
        BeanRegistration reg;
        reg.type_id       = std::type_index(typeid(T));
        reg.name          = typeid(T).name();
        reg.scope         = Scope::Connection;
        reg.factory       = [f = std::move(factory)](ContainerBase& c) -> void* { return f(c); };
        reg.destructor_fn = [](void* p) { delete static_cast<T*>(p); };
        registrations_[reg.type_id] = std::move(reg);
        return *this;
    }

    /// Lazily resolve a Connection-scoped bean.
    template<typename T>
    T& resolve() {
        auto tid = std::type_index(typeid(T));
        auto it = instances_.find(tid);
        if (it != instances_.end()) return *static_cast<T*>(it->second);

        auto* reg = find_registration(tid);
        if (reg && reg->scope == Scope::Connection) {
            void* raw = reg->factory(*this);
            if (reg->post_construct_fn) reg->post_construct_fn(raw);
            lifecycle_.register_bean(static_cast<T*>(raw), {}, reg->pre_destroy_fn);
            instances_[tid] = raw;
            owned_instances_.emplace_back(raw, reg->destructor_fn);
            return *static_cast<T*>(raw);
        }

        return ContainerBase::resolve<T>();
    }

private:
    std::vector<std::pair<void*, std::function<void(void*)>>> owned_instances_;
};

} // namespace novaboot::di
