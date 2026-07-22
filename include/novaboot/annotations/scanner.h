#pragma once

#include "novaboot/di/di.h"
#include "novaboot/router/router.h"
#include "novaboot/annotations/annotations.h"
#include "novaboot/db/transaction.h"
#include "novaboot/middleware/declarative_authorization.h"
#include "novaboot/messaging/stomp.h"

#include <meta>
#include <concepts>
#include <string>
#include <string_view>
#include <iostream>

namespace novaboot::annotations {

template<typename... T>
void register_routes(router::Router& router);

template<typename... T>
void register_advice(router::Router& router);

template<typename T>
void register_routes_for(router::Router& router, di::RootContainer&) {
    register_routes<T>(router);
}

template<typename T>
void register_advice_for(router::Router& router, di::RootContainer&) {
    register_advice<T>(router);
}

template<typename T>
void register_websocket_for(router::Router& router, di::RootContainer& container);

template<typename T>
void register_message_mappings(messaging::stomp::MessageDispatcher& dispatcher,
                               messaging::stomp::MessagingTemplate& messaging,
                               T& controller);

// ─────────────────────────────────────────────────────────────────────────────
// Consteval Reflection Helper Structures
// ─────────────────────────────────────────────────────────────────────────────

struct MemberList {
    std::meta::info data[128] = {};
    std::size_t size = 0;
    
    consteval const std::meta::info* begin() const noexcept { return data; }
    consteval const std::meta::info* end() const noexcept { return data + size; }
};

template<typename T>
consteval MemberList get_members() {
    constexpr auto ctx = std::meta::access_context::current();
    MemberList result;
    for (auto m : std::meta::members_of(std::meta::dealias(^^T), ctx)) {
        if (result.size < 128) {
            result.data[result.size++] = m;
        }
    }
    return result;
}

template<std::meta::info m>
consteval auto get_member_ptr() {
    return &[:m:];
}

// ─────────────────────────────────────────────────────────────────────────────
// Annotation Extraction Helpers
// ─────────────────────────────────────────────────────────────────────────────

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

struct RouteMappingInfo {
    bool has_mapping = false;
    router::Method method = router::Method::GET;
    char path[64] = {};
};

consteval RouteMappingInfo get_route_mapping(std::meta::info m) {
    RouteMappingInfo info;
    for (auto ann : std::meta::annotations_of(m)) {
        auto type = std::meta::remove_cv(std::meta::type_of(ann));
        
        auto copy_path = [&](const auto& val) {
            info.has_mapping = true;
            int i = 0;
            while (val.path[i] && i < 63) {
                info.path[i] = val.path[i];
                i++;
            }
            info.path[i] = '\0';
        };

        if (std::meta::is_same_type(type, ^^GetMapping)) {
            info.method = router::Method::GET;
            copy_path(std::meta::extract<GetMapping>(ann));
            return info;
        } else if (std::meta::is_same_type(type, ^^PostMapping)) {
            info.method = router::Method::POST;
            copy_path(std::meta::extract<PostMapping>(ann));
            return info;
        } else if (std::meta::is_same_type(type, ^^PutMapping)) {
            info.method = router::Method::PUT;
            copy_path(std::meta::extract<PutMapping>(ann));
            return info;
        } else if (std::meta::is_same_type(type, ^^DeleteMapping)) {
            info.method = router::Method::DELETE_;
            copy_path(std::meta::extract<DeleteMapping>(ann));
            return info;
        } else if (std::meta::is_same_type(type, ^^PatchMapping)) {
            info.method = router::Method::PATCH;
            copy_path(std::meta::extract<PatchMapping>(ann));
            return info;
        } else if (std::meta::is_same_type(type, ^^HeadMapping)) {
            info.method = router::Method::HEAD;
            copy_path(std::meta::extract<HeadMapping>(ann));
            return info;
        } else if (std::meta::is_same_type(type, ^^OptionsMapping)) {
            info.method = router::Method::OPTIONS;
            copy_path(std::meta::extract<OptionsMapping>(ann));
            return info;
        }
    }
    return info;
}

// ─────────────────────────────────────────────────────────────────────────────
// Path Joining Helper
// ─────────────────────────────────────────────────────────────────────────────

inline std::string join_paths(std::string_view base, std::string_view rel) {
    if (rel.empty()) return std::string(base);
    if (base.empty()) return std::string(rel);
    std::string res(base);
    if (res.back() == '/' && rel.front() == '/') {
        res.pop_back();
    } else if (res.back() != '/' && rel.front() != '/') {
        res.push_back('/');
    }
    res.append(rel);
    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// DI Lifecycle Wiring Helper
// ─────────────────────────────────────────────────────────────────────────────

template<typename T, typename Builder>
void wire_lifecycle(Builder& builder) {
    static constexpr auto members = get_members<T>();
    template for (constexpr auto m : members) {
        constexpr bool has_id = std::meta::has_identifier(m);
        if constexpr (has_id) {
            constexpr bool is_func = std::meta::is_function(m);
            constexpr bool is_ctor = std::meta::is_constructor(m);
            constexpr bool is_dtor = std::meta::is_destructor(m);
            if constexpr (is_func && !is_ctor && !is_dtor) {
                if constexpr (has_annotation<PostConstruct>(m)) {
                    builder.on_start(get_member_ptr<m>());
                }
                if constexpr (has_annotation<PreDestroy>(m)) {
                    builder.on_stop(get_member_ptr<m>());
                }
            }
        }
    }
}

template<typename T>
consteval bool has_transactional_method() {
    static constexpr auto members = get_members<T>();
    template for (constexpr auto m : members) {
        constexpr bool has_id = std::meta::has_identifier(m);
        if constexpr (has_id) {
            constexpr bool is_func = std::meta::is_function(m);
            constexpr bool is_ctor = std::meta::is_constructor(m);
            constexpr bool is_dtor = std::meta::is_destructor(m);
            if constexpr (is_func && !is_ctor && !is_dtor) {
                if constexpr (has_annotation<Transactional>(m)) {
                    return true;
                }
            }
        }
    }
    return false;
}

template<typename T>
void register_transactional_proxy_if_needed(di::RootContainer& container,
                                            di::Scope scope) {
    if constexpr (has_transactional_method<T>()) {
        using Proxy = novaboot::db::TransactionalProxy<T>;
        container.template register_bean<Proxy>(
            [](di::ContainerBase& c) -> Proxy* {
                return new Proxy(c.template resolve<T>(),
                                 c.template resolve<novaboot::db::TransactionManager>());
            },
            scope,
            "",     // qualifier
            false,  // is_primary
            true    // is_lazy: do not require TransactionManager unless proxy is used
        );
        container.add_dependency(std::type_index(typeid(Proxy)), std::type_index(typeid(T)));
        container.add_dependency(std::type_index(typeid(Proxy)),
                                 std::type_index(typeid(novaboot::db::TransactionManager)));
    }
}

template<typename T>
void register_authorization_proxy_if_needed(di::RootContainer& container,
                                            di::Scope scope) {
    if constexpr (middleware::declarative_detail::has_authorized_method<T>()) {
        using Proxy = middleware::AuthorizationProxy<T>;
        if constexpr (has_transactional_method<T>()) {
            container.template register_bean<Proxy>(
                [](di::ContainerBase& c) -> Proxy* {
                    return new Proxy(c.template resolve<T>(),
                                     c.template resolve<novaboot::db::TransactionManager>());
                },
                scope, "", false, true);
            container.add_dependency(std::type_index(typeid(Proxy)),
                                     std::type_index(typeid(novaboot::db::TransactionManager)));
        } else {
            container.template register_bean<Proxy>(
                [](di::ContainerBase& c) -> Proxy* {
                    return new Proxy(c.template resolve<T>());
                },
                scope, "", false, true);
        }
        container.add_dependency(std::type_index(typeid(Proxy)), std::type_index(typeid(T)));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Scanner APIs
// ─────────────────────────────────────────────────────────────────────────────

template<typename PMF>
struct bean_method_traits;

#define REGISTER_BEAN_TRAITS(PMF_TYPE, B_TYPE, RET_TYPE, INVOKE_EXPR) \
template<typename Class, typename T, typename... Args> \
struct bean_method_traits<PMF_TYPE> { \
    using class_type = Class; \
    using bean_type = B_TYPE; \
    using arg_types = di::TypeList<Args...>; \
    static RET_TYPE invoke(Class& config, auto pmf, di::ContainerBase& c) { \
        return INVOKE_EXPR; \
    } \
};

// Raw Pointer T*
REGISTER_BEAN_TRAITS(T* (Class::*)(Args...), T, T*, (config.*pmf)(c.resolve<std::remove_cvref_t<Args>>()...))
REGISTER_BEAN_TRAITS(T* (Class::*)(Args...) const, T, T*, (config.*pmf)(c.resolve<std::remove_cvref_t<Args>>()...))
REGISTER_BEAN_TRAITS(T* (Class::* const)(Args...), T, T*, (config.*pmf)(c.resolve<std::remove_cvref_t<Args>>()...))
REGISTER_BEAN_TRAITS(T* (Class::* const)(Args...) const, T, T*, (config.*pmf)(c.resolve<std::remove_cvref_t<Args>>()...))

// unique_ptr<T>
REGISTER_BEAN_TRAITS(std::unique_ptr<T> (Class::*)(Args...), T, T*, (config.*pmf)(c.resolve<std::remove_cvref_t<Args>>()...).release())
REGISTER_BEAN_TRAITS(std::unique_ptr<T> (Class::*)(Args...) const, T, T*, (config.*pmf)(c.resolve<std::remove_cvref_t<Args>>()...).release())
REGISTER_BEAN_TRAITS(std::unique_ptr<T> (Class::* const)(Args...), T, T*, (config.*pmf)(c.resolve<std::remove_cvref_t<Args>>()...).release())
REGISTER_BEAN_TRAITS(std::unique_ptr<T> (Class::* const)(Args...) const, T, T*, (config.*pmf)(c.resolve<std::remove_cvref_t<Args>>()...).release())

// shared_ptr<T>
REGISTER_BEAN_TRAITS(std::shared_ptr<T> (Class::*)(Args...), std::shared_ptr<T>, std::shared_ptr<T>*, new std::shared_ptr<T>((config.*pmf)(c.resolve<std::remove_cvref_t<Args>>()...)))
REGISTER_BEAN_TRAITS(std::shared_ptr<T> (Class::*)(Args...) const, std::shared_ptr<T>, std::shared_ptr<T>*, new std::shared_ptr<T>((config.*pmf)(c.resolve<std::remove_cvref_t<Args>>()...)))
REGISTER_BEAN_TRAITS(std::shared_ptr<T> (Class::* const)(Args...), std::shared_ptr<T>, std::shared_ptr<T>*, new std::shared_ptr<T>((config.*pmf)(c.resolve<std::remove_cvref_t<Args>>()...)))
REGISTER_BEAN_TRAITS(std::shared_ptr<T> (Class::* const)(Args...) const, std::shared_ptr<T>, std::shared_ptr<T>*, new std::shared_ptr<T>((config.*pmf)(c.resolve<std::remove_cvref_t<Args>>()...)))

#undef REGISTER_BEAN_TRAITS

/// Scans each type in the pack and registers it to the DI container if stereotyped.
template<typename... T>
void register_beans(di::RootContainer& container) {
    ([&container]() {
        using Type = T;
        if constexpr (has_annotation<Service>(^^Type)) {
            constexpr auto scope = get_annotation<Service>(^^Type).scope;
            auto builder = container.template autowire<Type>(scope);
            wire_lifecycle<Type>(builder);
            register_transactional_proxy_if_needed<Type>(container, scope);
            register_authorization_proxy_if_needed<Type>(container, scope);
        } else if constexpr (has_annotation<Repository>(^^Type)) {
            constexpr auto scope = get_annotation<Repository>(^^Type).scope;
            auto builder = container.template autowire<Type>(scope);
            wire_lifecycle<Type>(builder);
        } else if constexpr (has_annotation<Component>(^^Type)) {
            constexpr auto scope = get_annotation<Component>(^^Type).scope;
            auto builder = container.template autowire<Type>(scope);
            wire_lifecycle<Type>(builder);
            register_transactional_proxy_if_needed<Type>(container, scope);
        } else if constexpr (has_annotation<RestController>(^^Type)) {
            auto builder = container.template autowire<Type>(di::Scope::Singleton);
            wire_lifecycle<Type>(builder);
            container.add_route_registrar(&novaboot::annotations::register_routes_for<Type>);
        } else if constexpr (has_annotation<WebSocket>(^^Type)) {
            auto builder = container.template autowire<Type>(di::Scope::Singleton);
            wire_lifecycle<Type>(builder);
            container.add_route_registrar(&novaboot::annotations::register_websocket_for<Type>);
        } else if constexpr (has_annotation<ControllerAdvice>(^^Type)) {
            auto builder = container.template autowire<Type>(di::Scope::Singleton);
            wire_lifecycle<Type>(builder);
            container.add_route_registrar(&novaboot::annotations::register_advice_for<Type>);
        } else if constexpr (has_annotation<Configuration>(^^Type)) {
            auto builder = container.template autowire<Type>(di::Scope::Singleton);
            wire_lifecycle<Type>(builder);

            static constexpr auto members = get_members<Type>();
            template for (constexpr auto m : members) {
                constexpr bool has_id = std::meta::has_identifier(m);
                if constexpr (has_id) {
                    constexpr bool is_func = std::meta::is_function(m);
                    constexpr bool is_ctor = std::meta::is_constructor(m);
                    constexpr bool is_dtor = std::meta::is_destructor(m);
                    if constexpr (is_func && !is_ctor && !is_dtor) {
                        if constexpr (has_annotation<Bean>(m)) {
                            constexpr auto pmf = get_member_ptr<m>();
                            constexpr auto scope = get_annotation<Bean>(m).scope;
                            
                            constexpr int order_val = []() {
                                if constexpr (has_annotation<Order>(m)) {
                                    return get_annotation<Order>(m).value;
                                }
                                return 0;
                            }();

                            using Traits = bean_method_traits<decltype(pmf)>;
                            using BType = typename Traits::bean_type;
                            using ArgsList = typename Traits::arg_types;

                            container.register_bean<BType>(
                                [pmf](di::ContainerBase& c) -> BType* {
                                    auto& config = c.resolve<Type>();
                                    return Traits::invoke(config, pmf, c);
                                },
                                scope,
                                "",     // qualifier
                                false,  // is_primary
                                false,  // is_lazy
                                order_val
                            );

                            container.add_dependency(std::type_index(typeid(BType)), std::type_index(typeid(Type)));

                            auto b_builder = di::BeanBuilder<BType>(container, std::type_index(typeid(BType)));
                            wire_lifecycle<BType>(b_builder);

                            ArgsList::template add_dependencies<BType>(container);
                        }
                    }
                }
            }
        }
    }(), ...);
}

template<typename T>
struct advice_handler_traits;

template<typename Class, typename Ret, typename Ex, typename... Args>
struct advice_handler_traits<Ret (Class::*)(Ex, Args...)> {
    using class_type = Class;
    using exception_type = std::remove_cvref_t<Ex>;
};

template<typename Class, typename Ret, typename Ex, typename... Args>
struct advice_handler_traits<Ret (Class::*)(Ex, Args...) const> {
    using class_type = Class;
    using exception_type = std::remove_cvref_t<Ex>;
};

template<typename Class, typename Ret, typename Ex, typename... Args>
struct advice_handler_traits<Ret (Class::* const)(Ex, Args...)> {
    using class_type = Class;
    using exception_type = std::remove_cvref_t<Ex>;
};

template<typename Class, typename Ret, typename Ex, typename... Args>
struct advice_handler_traits<Ret (Class::* const)(Ex, Args...) const> {
    using class_type = Class;
    using exception_type = std::remove_cvref_t<Ex>;
};

template<typename Class, typename Ex, auto MethodPtr>
class AdviceExceptionHandler : public router::ExceptionHandler {
    template<typename R>
    static void handle_result(const R& result, http3::Response& res) {
        if constexpr (requires { result.status_code(); result.headers(); }) {
            res.status(result.status_code());
            for (const auto& h : result.headers()) {
                res.header(h.first, h.second);
            }
            if constexpr (requires { result.body(); }) {
                res.json(novaboot::json::serialize(result.body()));
            }
        } else {
            res.status(200);
            res.json(novaboot::json::serialize(result));
        }
    }

public:
    bool handle(const std::exception& ex, http3::Response& res, context::RequestContext& ctx) override {
        if (auto* typed_ex = dynamic_cast<const Ex*>(&ex)) {
            auto& advice = ctx.inject<Class>();
            if constexpr (std::is_invocable_v<decltype(MethodPtr), Class&, const Ex&, http3::Response&, context::RequestContext&>) {
                if constexpr (std::is_void_v<std::invoke_result_t<decltype(MethodPtr), Class&, const Ex&, http3::Response&, context::RequestContext&>>) {
                    (advice.*MethodPtr)(*typed_ex, res, ctx);
                } else {
                    auto result = (advice.*MethodPtr)(*typed_ex, res, ctx);
                    handle_result(result, res);
                }
            } else if constexpr (std::is_invocable_v<decltype(MethodPtr), Class&, const Ex&, context::RequestContext&>) {
                if constexpr (std::is_void_v<std::invoke_result_t<decltype(MethodPtr), Class&, const Ex&, context::RequestContext&>>) {
                    (advice.*MethodPtr)(*typed_ex, ctx);
                } else {
                    auto result = (advice.*MethodPtr)(*typed_ex, ctx);
                    handle_result(result, res);
                }
            } else if constexpr (std::is_invocable_v<decltype(MethodPtr), Class&, const Ex&>) {
                if constexpr (std::is_void_v<std::invoke_result_t<decltype(MethodPtr), Class&, const Ex&>>) {
                    (advice.*MethodPtr)(*typed_ex);
                } else {
                    auto result = (advice.*MethodPtr)(*typed_ex);
                    handle_result(result, res);
                }
            }
            return true;
        }
        return false;
    }
};

/// Scans each type in the pack for RestController annotations and maps annotated routes.
template<typename... T>
void register_routes(router::Router& router) {
    ([&router]() {
        using Type = T;
        if constexpr (has_annotation<RestController>(^^Type)) {
            constexpr auto rest_ctrl = get_annotation<RestController>(^^Type);
            std::string_view base_path(rest_ctrl.base_path);
            static constexpr auto members = get_members<Type>();
            template for (constexpr auto m : members) {
                constexpr bool has_id = std::meta::has_identifier(m);
                if constexpr (has_id) {
                    constexpr bool is_func = std::meta::is_function(m);
                    constexpr bool is_ctor = std::meta::is_constructor(m);
                    constexpr bool is_dtor = std::meta::is_destructor(m);
                    if constexpr (is_func && !is_ctor && !is_dtor) {
                        constexpr auto mapping = get_route_mapping(m);
                        if constexpr (mapping.has_mapping) {
                            std::string full_path = join_paths(base_path, mapping.path);
                            constexpr auto pmf = get_member_ptr<m>();
                            router.add_route(mapping.method, full_path, di::handler<pmf>());
                        }
                    }
                }
            }
        }
    }(), ...);
}

/// Register a class-level [[= WebSocket("/path") ]] endpoint. Endpoint
/// instances are singleton beans and callbacks are invoked on the owning
/// network shard. Supported optional methods are:
///   void on_open(websocket::Session&)
///   void on_message(websocket::Session&, const websocket::Message&)
///   void on_close(websocket::Session&, const websocket::CloseInfo&)
///   websocket::HandshakeDecision authorize(const http3::Request&)
template<typename T>
void register_websocket_for(router::Router& router, di::RootContainer& container) {
    static_assert(has_annotation<WebSocket>(^^T),
                  "register_websocket_for requires [[= WebSocket(\"/path\") ]]");
    constexpr auto endpoint_annotation = get_annotation<WebSocket>(^^T);
    static_assert(endpoint_annotation.path[0] == '/',
                  "WebSocket endpoint paths must start with '/'");

    auto* endpoint = &container.template resolve<T>();
    router.add_websocket(endpoint_annotation.path, websocket::Handler{
        .on_open = [endpoint](websocket::Session& session) {
            if constexpr (requires(T& instance, websocket::Session& socket) {
                              instance.on_open(socket);
                          }) {
                endpoint->on_open(session);
            }
        },
        .on_message = [endpoint](websocket::Session& session,
                                 const websocket::Message& message) {
            if constexpr (requires(T& instance, websocket::Session& socket,
                                   const websocket::Message& incoming) {
                              instance.on_message(socket, incoming);
                          }) {
                endpoint->on_message(session, message);
            }
        },
        .on_close = [endpoint](websocket::Session& session,
                               const websocket::CloseInfo& close) {
            if constexpr (requires(T& instance, websocket::Session& socket,
                                   const websocket::CloseInfo& info) {
                              instance.on_close(socket, info);
                          }) {
                endpoint->on_close(session, close);
            }
        },
        .authorize = [endpoint](const http3::Request& request) {
            if constexpr (requires(T& instance, const http3::Request& handshake) {
                              { instance.authorize(handshake) } ->
                                  std::convertible_to<websocket::HandshakeDecision>;
                          }) {
                return static_cast<websocket::HandshakeDecision>(endpoint->authorize(request));
            }
            return websocket::HandshakeDecision::allow();
        },
    });
}

/// Register [[= MessageMapping("/path") ]] methods on an application
/// controller. Endpoint strips its configurable application prefix (normally
/// /app) from a client SEND destination before this lookup. Supported method
/// forms are:
///   void method(std::string_view payload)
///   std::string method(std::string_view payload) [[= SendTo("/topic/path") ]]
/// The latter publishes the returned payload through MessagingTemplate.
template<typename T>
void register_message_mappings(messaging::stomp::MessageDispatcher& dispatcher,
                               messaging::stomp::MessagingTemplate& messaging,
                               T& controller) {
    static constexpr auto members = get_members<T>();
    template for (constexpr auto m : members) {
        constexpr bool has_id = std::meta::has_identifier(m);
        if constexpr (has_id && std::meta::is_function(m) &&
                      !std::meta::is_constructor(m) && !std::meta::is_destructor(m)) {
            if constexpr (has_annotation<MessageMapping>(m)) {
                constexpr auto mapping = get_annotation<MessageMapping>(m);
                constexpr auto send_to = get_annotation<SendTo>(m);
                constexpr auto pmf = get_member_ptr<m>();
                static_assert(mapping.path[0] == '/',
                              "MessageMapping destinations must start with '/'");
                dispatcher.add_mapping(mapping.path,
                    [&controller, &messaging, destination = std::string(send_to.path)](
                        std::string_view payload) -> bool {
                        if constexpr (requires(T& instance, std::string_view value) {
                                          (instance.*pmf)(value);
                                      }) {
                            using Result = decltype((controller.*pmf)(payload));
                            if constexpr (std::is_void_v<Result>) {
                                (controller.*pmf)(payload);
                                return true;
                            } else if constexpr (std::is_same_v<std::remove_cvref_t<Result>, std::string>) {
                                static_assert(send_to.path[0] != '\0',
                                              "MessageMapping methods returning std::string require [[= SendTo(...) ]]");
                                return messaging.convert_and_send(destination, (controller.*pmf)(payload)) > 0U;
                            } else {
                                static_assert(std::is_void_v<Result>,
                                              "MessageMapping must return void or std::string");
                            }
                        } else {
                            static_assert(requires(T& instance, std::string_view value) {
                                              (instance.*pmf)(value);
                                          },
                                          "MessageMapping methods must accept std::string_view");
                        }
                    });
            }
        }
    }
}

/// Scans each type in the pack for ControllerAdvice annotations and registers exception handlers.
template<typename... T>
void register_advice(router::Router& router) {
    ([&router]() {
        using Type = T;
        if constexpr (has_annotation<ControllerAdvice>(^^Type)) {
            static constexpr auto members = get_members<Type>();
            template for (constexpr auto m : members) {
                constexpr bool has_id = std::meta::has_identifier(m);
                if constexpr (has_id) {
                    constexpr bool is_func = std::meta::is_function(m);
                    constexpr bool is_ctor = std::meta::is_constructor(m);
                    constexpr bool is_dtor = std::meta::is_destructor(m);
                    if constexpr (is_func && !is_ctor && !is_dtor) {
                        if constexpr (has_annotation<ExceptionHandler>(m)) {
                            constexpr auto pmf = get_member_ptr<m>();
                            using Traits = advice_handler_traits<decltype(pmf)>;
                            using Class = typename Traits::class_type;
                            using Ex = typename Traits::exception_type;
                            router.add_exception_handler(
                                std::make_unique<AdviceExceptionHandler<Class, Ex, pmf>>()
                            );
                        }
                    }
                }
            }
        }
    }(), ...);
}

} // namespace novaboot::annotations
