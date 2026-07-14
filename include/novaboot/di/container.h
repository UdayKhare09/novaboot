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

#include "novaboot/di/lifecycle.h"
#include "novaboot/di/scope.h"
#include "novaboot/config/app_config.h"
#ifdef __cpp_impl_reflection
#include <meta>
#endif

#include <any>
#include <mutex>
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
namespace novaboot::router { class Router; }

namespace novaboot::di {

/// Annotation for injecting configuration values into class fields.
struct Value {
    char key[64] = {};
    consteval Value() = default;
    consteval explicit Value(const char* k) {
        int i = 0;
        while (k[i] && i < 63) {
            key[i] = k[i];
            i++;
        }
        key[i] = '\0';
    }
};

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
} // namespace detail

class RootContainer;

template<typename T>
class BeanBuilder;

template<typename Interface>
class BindBuilder;


// ─────────────────────────────────────────────────────────────────────────────
// RootContainer — owns all Singleton beans
// ─────────────────────────────────────────────────────────────────────────────

class RootContainer : public ContainerBase {
public:
    using RouteRegistrar = std::function<void(router::Router&)>;

    RootContainer() = default;
    ~RootContainer() override;

    void add_route_registrar(RouteRegistrar registrar) {
        route_registrars_.push_back(std::move(registrar));
    }

    void register_routes_and_advice(router::Router& router);

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

        if constexpr (std::is_base_of_v<Destroyable, T>) {
            reg.pre_destroy_fn = [](void* p) {
                static_cast<Destroyable*>(static_cast<T*>(p))->pre_destroy();
            };
        }

        registrations_[reg.type_id] = std::move(reg);
        return *this;
    }

    template<typename T>
    BeanBuilder<T> singleton(std::function<T*(ContainerBase&)> factory) {
        register_bean<T>(std::move(factory), Scope::Singleton);
        return BeanBuilder<T>(*this, std::type_index(typeid(T)));
    }

    template<typename T>
    BeanBuilder<T> request(std::function<T*(ContainerBase&)> factory) {
        register_bean<T>(std::move(factory), Scope::Request);
        return BeanBuilder<T>(*this, std::type_index(typeid(T)));
    }

    template<typename Interface>
    BindBuilder<Interface> bind() {
        return BindBuilder<Interface>(*this);
    }

#ifdef __cpp_impl_reflection
    template<typename T>
    BeanBuilder<T> autowire(Scope scope = Scope::Singleton);
#endif

    std::unordered_map<std::type_index, BeanRegistration>& get_registrations() {
        return registrations_;
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

    void add_dependency(std::type_index parent_tid, std::type_index dep_tid) {
        auto it = registrations_.find(parent_tid);
        if (it != registrations_.end()) {
            it->second.dep_type_ids.push_back(dep_tid);
        }
    }

private:
    bool built_ = false;
    mutable std::recursive_mutex lazy_mutex_;

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

    std::vector<RouteRegistrar> route_registrars_;
};

template<typename... Args>
struct TypeList {
    template<typename T>
    static T* construct(ContainerBase& c) {
        return new T(c.resolve<std::remove_cvref_t<Args>>()...);
    }

    template<typename T>
    static void add_dependencies(RootContainer& container) {
        auto parent_tid = std::type_index(typeid(T));
        (container.add_dependency(parent_tid, std::type_index(typeid(std::remove_cvref_t<Args>))), ...);
    }
};

#ifdef __cpp_impl_reflection
namespace detail {
    template<typename T>
    consteval auto get_members() {
        constexpr auto ctx = std::meta::access_context::current();
        struct ArrayWrapper {
            std::meta::info data[64] = {};
            std::size_t     size = 0;

            consteval const std::meta::info* begin() const noexcept { return data; }
            consteval const std::meta::info* end() const noexcept { return data + size; }
        };
        ArrayWrapper result;
        for (auto m : std::meta::members_of(^^T, ctx)) {
            if (result.size < 64) {
                result.data[result.size++] = m;
            }
        }
        return result;
    }

    template<typename Ann>
    consteval bool has_annotation(std::meta::info target) {
        for (auto ann : std::meta::annotations_of(std::meta::dealias(target))) {
            if (std::meta::is_same_type(std::meta::remove_cv(std::meta::type_of(ann)), ^^Ann)) {
                return true;
            }
        }
        return false;
    }

    template<typename Ann>
    consteval Ann get_annotation(std::meta::info target) {
        for (auto ann : std::meta::annotations_of(std::meta::dealias(target))) {
            if (std::meta::is_same_type(std::meta::remove_cv(std::meta::type_of(ann)), ^^Ann)) {
                return std::meta::extract<Ann>(ann);
            }
        }
        return Ann{};
    }

    template<typename T>
    consteval std::meta::info get_constructor_typelist() {
        constexpr auto ctx = std::meta::access_context::current();
        std::meta::info target_ctor = std::meta::info{};
        bool found = false;
        std::size_t max_params = 0;

        for (auto m : std::meta::members_of(^^T, ctx)) {
            if (std::meta::is_constructor(m)) {
                auto params = std::meta::parameters_of(m);
                
                // Ignore copy and move constructors
                bool is_copy_or_move = false;
                if (params.size() == 1) {
                    auto p_type = std::meta::type_of(params[0]);
                    if (std::meta::is_same_type(std::meta::remove_cvref(p_type), ^^T)) {
                        is_copy_or_move = true;
                    }
                }
                
                if (!is_copy_or_move) {
                    if (!found || params.size() > max_params) {
                        target_ctor = m;
                        max_params = params.size();
                        found = true;
                    }
                }
            }
        }

        if (!found) {
            return ^^TypeList<>;
        }

        auto params = std::meta::parameters_of(target_ctor);
        std::vector<std::meta::info> param_types;
        for (auto p : params) {
            param_types.push_back(std::meta::type_of(p));
        }

        return std::meta::substitute(std::meta::template_of(^^TypeList<T>), param_types);
    }
} // namespace detail

template<typename T>
BeanBuilder<T> RootContainer::autowire(Scope scope) {
    constexpr auto type_info = detail::get_constructor_typelist<T>();
    using ExtractedTypeList = typename[: type_info :];

    register_bean<T>([](ContainerBase& c) {
        T* obj = ExtractedTypeList::template construct<T>(c);

        // Perform @Value property injection
        static constexpr auto members = detail::get_members<T>();
        template for (constexpr auto m : members) {
            if constexpr (std::meta::is_nonstatic_data_member(m)) {
                if constexpr (detail::has_annotation<Value>(m)) {
                    constexpr auto ann = detail::get_annotation<Value>(m);
                    using FieldType = std::remove_cvref_t<decltype(obj->*&[:m:])>;
                    if (c.has<config::AppConfig>()) {
                        auto& app_cfg = c.resolve<config::AppConfig>();
                        auto val_opt = app_cfg.get<FieldType>(ann.key);
                        if (val_opt) {
                            obj->*&[:m:] = *val_opt;
                        }
                    }
                }
            }
        }

        return obj;
    }, scope);

    ExtractedTypeList::template add_dependencies<T>(*this);

    // Auto-detect @Value dependencies and register AppConfig as a dependency
    static constexpr auto members = detail::get_members<T>();
    constexpr bool has_value = []() {
        bool found = false;
        template for (constexpr auto m : members) {
            if constexpr (std::meta::is_nonstatic_data_member(m)) {
                if constexpr (detail::has_annotation<Value>(m)) {
                    found = true;
                }
            }
        }
        return found;
    }();
    if constexpr (has_value) {
        this->add_dependency(std::type_index(typeid(T)), std::type_index(typeid(config::AppConfig)));
    }

    return BeanBuilder<T>(*this, std::type_index(typeid(T)));
}
#endif

template<typename T>
class BeanBuilder {
private:
    RootContainer& container_;
    std::type_index type_id_;
public:
    BeanBuilder(RootContainer& container, std::type_index type_id)
        : container_(container), type_id_(type_id) {}

    BeanBuilder& on_start(std::function<void(T&)> fn) {
        container_.with_post_construct<T>(std::move(fn));
        return *this;
    }

    BeanBuilder& on_start(void (T::*member_fn)()) {
        container_.with_post_construct<T>([member_fn](T& obj) {
            (obj.*member_fn)();
        });
        return *this;
    }

    BeanBuilder& on_stop(std::function<void(T&)> fn) {
        container_.with_pre_destroy<T>(std::move(fn));
        return *this;
    }

    BeanBuilder& on_stop(void (T::*member_fn)()) {
        container_.with_pre_destroy<T>([member_fn](T& obj) {
            (obj.*member_fn)();
        });
        return *this;
    }

    BeanBuilder& qualifier(const std::string& name) {
        auto& registrations = container_.get_registrations();
        auto it = registrations.find(type_id_);
        if (it != registrations.end()) {
            it->second.qualifier = name;
        }
        return *this;
    }

    BeanBuilder& primary() {
        auto& registrations = container_.get_registrations();
        auto it = registrations.find(type_id_);
        if (it != registrations.end()) {
            it->second.is_primary = true;
        }
        return *this;
    }

    BeanBuilder& lazy() {
        auto& registrations = container_.get_registrations();
        auto it = registrations.find(type_id_);
        if (it != registrations.end()) {
            it->second.is_lazy = true;
        }
        return *this;
    }

    template<typename Dep>
    BeanBuilder& depends_on() {
        container_.add_dependency(type_id_, std::type_index(typeid(Dep)));
        return *this;
    }
};

template<typename Interface>
class BindBuilder {
private:
    RootContainer& container_;
public:
    explicit BindBuilder(RootContainer& container) : container_(container) {}

    template<typename Implementation>
    void to() {
        auto interface_tid = std::type_index(typeid(Interface));
        auto impl_tid = std::type_index(typeid(Implementation));

        Scope scope = Scope::Singleton;
        const char* q = "";
        bool is_prim = false;
        bool is_lazy = false;

        auto& registrations = container_.get_registrations();
        auto it = registrations.find(impl_tid);
        if (it != registrations.end()) {
            scope = it->second.scope;
            q = it->second.qualifier.c_str();
            is_prim = it->second.is_primary;
            is_lazy = it->second.is_lazy;
        }

        container_.register_bean<Interface>([](ContainerBase& c) -> Interface* {
            return &c.resolve<Implementation>();
        }, scope, q, is_prim, is_lazy);

        container_.add_dependency(interface_tid, impl_tid);

        // Override lifecycle handlers to prevent duplicate lifecycle actions or double deletion
        auto base_it = registrations.find(interface_tid);
        if (base_it != registrations.end()) {
            base_it->second.destructor_fn = nullptr;
            base_it->second.post_construct_fn = nullptr;
            base_it->second.pre_destroy_fn = nullptr;
            base_it->second.is_lazy = true;
        }
    }
};

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
