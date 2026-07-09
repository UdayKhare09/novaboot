#pragma once

#include <cstdint>
#include <any>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <stdexcept>

// Forward declaration — avoids circular include before user includes container.h
namespace novaboot::di { class RequestContainer; }

namespace novaboot::context {

/// Per-request context for dependency injection and middleware data passing.
///
/// Middleware can store typed values and downstream handlers can retrieve them.
/// It also holds a pointer to the request-scoped DI container so that route
/// handlers can do `ctx.inject<UserService>()` without taking the container
/// as an explicit parameter.
class RequestContext {
public:
    RequestContext() = default;

    // ── DI container binding ───────────────────────────────────────────────────

    /// Bind a DI RequestContainer to this context.
    /// Called by the framework before invoking the route handler.
    void bind_container(di::RequestContainer& container) noexcept {
        di_container_ = &container;
    }

    /// Returns the bound DI container (nullptr if not yet bound).
    [[nodiscard]] di::RequestContainer* di_container() noexcept {
        return di_container_;
    }

    /// Resolve a bean of type T from the DI request container.
    /// The full definition is in request_context_di.h — include that header
    /// (after including novaboot/di/container.h) to use inject<T>().
    template<typename T>
    [[nodiscard]] T& inject();

    /// Resolve a named-qualifier bean from the DI request container.
    template<typename T>
    [[nodiscard]] T& inject_named(const char* qualifier_name);

    // ── Typed key-value store ─────────────────────────────────────────────────

    template <typename T>
    void set(T value) {
        values_[std::type_index(typeid(T))] = std::move(value);
    }

    template <typename T>
    [[nodiscard]] T* get() {
        auto it = values_.find(std::type_index(typeid(T)));
        if (it == values_.end()) return nullptr;
        return std::any_cast<T>(&it->second);
    }

    template <typename T>
    [[nodiscard]] const T* get() const {
        auto it = values_.find(std::type_index(typeid(T)));
        if (it == values_.end()) return nullptr;
        return std::any_cast<T>(&it->second);
    }

    template <typename T>
    [[nodiscard]] bool has() const {
        return values_.contains(std::type_index(typeid(T)));
    }

    template <typename T>
    void remove() {
        values_.erase(std::type_index(typeid(T)));
    }

    void set_string(std::string_view key, std::string value) {
        string_values_[std::string(key)] = std::move(value);
    }

    [[nodiscard]] std::string_view get_string(std::string_view key) const {
        auto it = string_values_.find(std::string(key));
        if (it != string_values_.end()) return it->second;
        return {};
    }

    void clear() {
        values_.clear();
        string_values_.clear();
    }

private:
    std::unordered_map<std::type_index, std::any> values_;
    std::unordered_map<std::string, std::string>  string_values_;

    di::RequestContainer* di_container_ = nullptr;
};

} // namespace novaboot::context

// ─── Inline DI injection (requires novaboot/di/container.h) ──────────────────
// These template definitions are placed AFTER the class so that users who only
// need the key-value API can include this header without pulling in container.h.
// Anyone calling inject<T>() must also include novaboot/di/container.h first,
// or include novaboot/di/di.h which pulls everything.
//
// If you get "undefined reference to inject<T>()", include di/container.h before
// this header, or include novaboot/di/di.h instead.

#ifdef NOVABOOT_DI_CONTAINER_H_INCLUDED  // set by container.h
// already defined below
#endif

// We use a simple trick: the definitions are in a separate header that is
// automatically included when container.h is already visible.
// The actual trick: define them here inline unconditionally using a forward-decl trick.
// Since RequestContainer has resolve<T>() as a template, we need the full definition.
// Solution: include the definitions inline below — this is fine because
// container.h guards itself with #pragma once.

// Pull in the full container definition so inject<T>() can call resolve<T>().
// This is intentional: anyone using inject<T>() needs the DI runtime anyway.
#include "novaboot/di/container.h"

namespace novaboot::context {

template<typename T>
T& RequestContext::inject() {
    if (!di_container_)
        throw std::runtime_error(
            "novaboot::di: RequestContext::inject<T>() — no DI container bound. "
            "Call bind_container() before calling inject().");
    return di_container_->resolve<T>();
}

template<typename T>
T& RequestContext::inject_named(const char* qualifier_name) {
    if (!di_container_)
        throw std::runtime_error(
            "novaboot::di: RequestContext::inject_named<T>() — no DI container bound.");
    return di_container_->resolve_named<T>(qualifier_name);
}

} // namespace novaboot::context
