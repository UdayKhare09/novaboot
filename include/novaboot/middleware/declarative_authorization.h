#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#ifdef __cpp_impl_reflection
#include <meta>
#endif

#include "novaboot/annotations/stereotypes.h"
#include "novaboot/context/request_context.h"
#include "novaboot/db/transaction.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"

namespace novaboot::middleware {

class AccessDeniedException : public std::runtime_error {
public:
    explicit AccessDeniedException(std::string message)
        : std::runtime_error(std::move(message)) {}
};

enum class DeclarativeAuthorizationDecision {
    Allowed,
    Unauthenticated,
    Forbidden,
};

/// Runtime form of [[= annotations::Authorize(...) ]]. This is public so a
/// custom controller registrar can apply the exact same security contract.
struct DeclarativeAuthorization {
    std::string_view roles;
    std::string_view scopes;
    annotations::AuthorizationMatch role_match = annotations::AuthorizationMatch::All;
    annotations::AuthorizationMatch scope_match = annotations::AuthorizationMatch::All;
};

/// Evaluate requirements without writing an HTTP response. This is used by
/// service proxies as well as the controller route adapter.
[[nodiscard]] DeclarativeAuthorizationDecision
evaluate_declarative(const context::RequestContext& context,
                     const DeclarativeAuthorization& requirements);

/// Authorize an already-authenticated request context. Returns false after
/// writing a standard 401/403 response; callers must not invoke the handler.
[[nodiscard]] bool authorize_declarative(http3::Request& request,
                                         http3::Response& response,
                                         context::RequestContext& context,
                                         const DeclarativeAuthorization& requirements);

#ifdef __cpp_impl_reflection
namespace declarative_detail {

template<typename Ann>
consteval bool has_annotation(std::meta::info target) {
    for (auto annotation : std::meta::annotations_of(std::meta::dealias(target))) {
        if (std::meta::is_same_type(std::meta::remove_cv(std::meta::type_of(annotation)), ^^Ann)) return true;
    }
    return false;
}

template<typename Ann>
consteval Ann get_annotation(std::meta::info target) {
    for (auto annotation : std::meta::annotations_of(std::meta::dealias(target))) {
        if (std::meta::is_same_type(std::meta::remove_cv(std::meta::type_of(annotation)), ^^Ann)) {
            return std::meta::extract<Ann>(annotation);
        }
    }
    return Ann{};
}

template<typename T>
consteval auto members() {
    struct MemberList {
        std::meta::info values[128] = {};
        std::size_t size = 0;
        consteval const std::meta::info* begin() const noexcept { return values; }
        consteval const std::meta::info* end() const noexcept { return values + size; }
    } result;
    constexpr auto access = std::meta::access_context::current();
    for (auto member : std::meta::members_of(std::meta::dealias(^^T), access)) {
        if (result.size < 128) result.values[result.size++] = member;
    }
    return result;
}

template<std::meta::info Member, auto MethodPtr>
consteval bool matches_method() {
    if constexpr (std::meta::is_function(Member) && !std::meta::is_constructor(Member) &&
                  !std::meta::is_destructor(Member) && std::meta::has_identifier(Member)) {
        constexpr std::string_view name = std::meta::identifier_of(Member);
        if constexpr (!name.starts_with("operator") &&
                      std::is_same_v<decltype(&[:Member:]), decltype(MethodPtr)>) {
            return &[:Member:] == MethodPtr;
        }
    }
    return false;
}

template<typename T, auto MethodPtr>
consteval std::meta::info method_meta() {
    static constexpr auto type_members = members<T>();
    template for (constexpr auto member : type_members) {
        if constexpr (matches_method<member, MethodPtr>()) return member;
    }
    return {};
}

struct Requirements {
    bool required = false;
    char roles[128] = {};
    char scopes[128] = {};
    annotations::AuthorizationMatch role_match = annotations::AuthorizationMatch::All;
    annotations::AuthorizationMatch scope_match = annotations::AuthorizationMatch::All;
};

template<typename T, std::meta::info Method>
consteval Requirements requirements_for() {
    Requirements requirements;
    if constexpr (has_annotation<annotations::PermitAll>(Method)) return requirements;
    if constexpr (has_annotation<annotations::Authorize>(Method)) {
        constexpr auto annotation = get_annotation<annotations::Authorize>(Method);
        requirements.required = true;
        for (int index = 0; index < 128; ++index) {
            requirements.roles[index] = annotation.roles[index];
            requirements.scopes[index] = annotation.scopes[index];
        }
        requirements.role_match = annotation.role_match;
        requirements.scope_match = annotation.scope_match;
    } else if constexpr (has_annotation<annotations::Authorize>(^^T)) {
        constexpr auto annotation = get_annotation<annotations::Authorize>(^^T);
        requirements.required = true;
        for (int index = 0; index < 128; ++index) {
            requirements.roles[index] = annotation.roles[index];
            requirements.scopes[index] = annotation.scopes[index];
        }
        requirements.role_match = annotation.role_match;
        requirements.scope_match = annotation.scope_match;
    }
    return requirements;
}

template<typename T>
consteval bool has_authorized_method() {
    if constexpr (has_annotation<annotations::Authorize>(^^T)) return true;
    static constexpr auto type_members = members<T>();
    template for (constexpr auto member : type_members) {
        if constexpr (std::meta::is_function(member) && !std::meta::is_constructor(member) &&
                      !std::meta::is_destructor(member) &&
                      has_annotation<annotations::Authorize>(member)) return true;
    }
    return false;
}

} // namespace declarative_detail

/// Explicit DI proxy for service method security. C++ cannot transparently
/// intercept concrete calls, so inject this proxy and call invoke<&Service::method>(...).
template<typename T>
class AuthorizationProxy {
public:
    explicit AuthorizationProxy(T& target) : target_(target) {}
    AuthorizationProxy(T& target, db::TransactionManager& transactions)
        : target_(target), transactions_(&transactions) {}
    AuthorizationProxy(const AuthorizationProxy&) = delete;
    AuthorizationProxy& operator=(const AuthorizationProxy&) = delete;

    template<auto MethodPtr, typename... Args>
    decltype(auto) invoke(Args&&... args) {
        constexpr auto method = declarative_detail::method_meta<T, MethodPtr>();
        static_assert(method != std::meta::info{}, "AuthorizationProxy::invoke could not find method metadata");
        constexpr auto requirements = declarative_detail::requirements_for<T, method>();
        if constexpr (requirements.required) {
            auto* request_context = context::RequestContext::current();
            if (request_context == nullptr) {
                throw AccessDeniedException("Secured service method requires an active request context");
            }
            const auto decision = evaluate_declarative(*request_context, {
                .roles = requirements.roles, .scopes = requirements.scopes,
                .role_match = requirements.role_match, .scope_match = requirements.scope_match,
            });
            if (decision == DeclarativeAuthorizationDecision::Unauthenticated) {
                throw AccessDeniedException("Secured service method requires authentication");
            }
            if (decision == DeclarativeAuthorizationDecision::Forbidden) {
                throw AccessDeniedException("Secured service method requires additional roles or scopes");
            }
        }
        if (transactions_ != nullptr) {
            return transactions_->template invoke<MethodPtr>(
                target_, std::forward<Args>(args)...);
        }
        return (target_.*MethodPtr)(std::forward<Args>(args)...);
    }

private:
    T& target_;
    db::TransactionManager* transactions_ = nullptr;
};
#endif

} // namespace novaboot::middleware
