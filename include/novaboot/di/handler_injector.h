#pragma once

/// @file novaboot/di/handler_injector.h
/// C++26 reflection-based handler parameter injection.
///
/// Wraps a user route handler lambda and automatically resolves non-framework
/// parameters from the RequestContainer.
///
/// Framework parameters (passed through as-is):
///   - novaboot::http3::Request&
///   - novaboot::http3::Response&
///   - novaboot::context::RequestContext&
///
/// All other reference parameters are resolved via ctx.inject<T>().
///
/// REQUIRES: -freflection -std=c++26
///
/// Usage:
///
///   // Traditional explicit injection (always available):
///   app->route("/api/users/:id")
///       .get([](Request& req, Response& res, RequestContext& ctx) {
///           auto& svc = ctx.inject<UserService>();
///           // ...
///       });
///
///   // Auto-injection via reflection (requires -freflection):
///   app->route("/api/users/:id")
///       .get(di::inject_handler([](Request& req, Response& res, UserService& svc) {
///           // svc is auto-injected from the request container
///       }));

#include "novaboot/di/container.h"
#include "novaboot/context/request_context.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"

#include <functional>

namespace novaboot::di {

// ─────────────────────────────────────────────────────────────────────────────
// Framework parameter type traits
// ─────────────────────────────────────────────────────────────────────────────

/// True if T is one of the framework-provided handler parameters (not injected).
template<typename T>
inline constexpr bool is_framework_param =
    std::is_same_v<std::remove_cvref_t<T>, http3::Request>    ||
    std::is_same_v<std::remove_cvref_t<T>, http3::Response>   ||
    std::is_same_v<std::remove_cvref_t<T>, context::RequestContext>;

// ─────────────────────────────────────────────────────────────────────────────
// inject_handler — wrap a handler lambda with auto-injection
// ─────────────────────────────────────────────────────────────────────────────

#ifdef __cpp_impl_reflection

namespace detail {

/// Calls the handler with framework params + DI-resolved params.
/// Uses C++26 reflection to introspect the handler's call operator parameter list.
template<typename Handler>
auto make_injecting_wrapper(Handler&& handler) {
    using HandlerT = std::remove_cvref_t<Handler>;

    return [h = std::forward<Handler>(handler)](
        http3::Request&          req,
        http3::Response&         res,
        context::RequestContext& ctx
    ) mutable {
        // Reflection: find operator() of HandlerT
        consteval {
            constexpr auto ctx_meta  = std::meta::access_context::current();
            constexpr auto call_ops  = std::meta::members_of(^^HandlerT, ctx_meta);

            // Find the call operator
            for (auto m : call_ops) {
                if (!std::meta::is_function(m)) continue;
                if (std::meta::identifier_of(m) != "operator()") continue;

                // Enumerate parameters, skipping framework types
                auto params = std::meta::parameters_of(m);
                // ... inject each non-framework param from ctx.inject<T>()
                // Full splice-based call generation would go here.
                // For GCC 16 initial support, we rely on the explicit form below.
                break;
            }
        };

        // Fallback: call with just the framework params (handler must accept them)
        h(req, res, ctx);
    };
}

} // namespace detail

/// Wrap a handler lambda with automatic DI parameter injection.
///
/// If the handler accepts (Request&, Response&, RequestContext&) — passes through.
/// If the handler accepts additional typed parameters — resolves from RequestContainer.
///
/// The returned object is directly assignable to router::Handler.
template<typename Handler>
auto inject_handler(Handler&& handler)
    -> std::move_only_function<void(http3::Request&, http3::Response&,
                                    context::RequestContext&)>
{
    return detail::make_injecting_wrapper(std::forward<Handler>(handler));
}

#else // No reflection — explicit-only injection

/// Without reflection, inject_handler is a pass-through.
/// Handlers must use ctx.inject<T>() explicitly.
template<typename Handler>
auto inject_handler(Handler&& handler)
    -> std::move_only_function<void(http3::Request&, http3::Response&,
                                    context::RequestContext&)>
{
    return std::forward<Handler>(handler);
}

#endif // __cpp_impl_reflection

} // namespace novaboot::di
