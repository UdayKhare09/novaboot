#pragma once

#include <cstdint>
#include <atomic>
#include <csignal>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <charconv>

#include "novaboot/core/shard.h"
#include "novaboot/middleware/middleware.h"
#include "novaboot/middleware/pipeline.h"
#include "novaboot/quic/tls_context.h"
#include "novaboot/router/router.h"
#include "novaboot/router/response_entity.h"
#include "novaboot/router/json.h"
#include "novaboot/validation/validation.h"
#include "novaboot/db/transaction.h"
#ifdef __cpp_impl_reflection
#  include <meta>
#endif

namespace novaboot::di { class RootContainer; }

namespace novaboot {

namespace detail {

#ifdef __cpp_impl_reflection
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

template<std::meta::info m>
consteval auto get_parameters() {
    struct ArrayWrapper {
        std::meta::info data[16] = {};
        std::size_t     size = 0;

        consteval const std::meta::info* begin() const noexcept { return data; }
        consteval const std::meta::info* end() const noexcept { return data + size; }
        consteval std::meta::info operator[](std::size_t idx) const noexcept { return data[idx]; }
    };
    ArrayWrapper result;
    for (auto p : std::meta::parameters_of(m)) {
        if (result.size < 16) {
            result.data[result.size++] = p;
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
#endif

template<typename T>
inline constexpr bool is_query_parseable_v =
    std::is_same_v<std::remove_cvref_t<T>, std::string> ||
    std::is_same_v<std::remove_cvref_t<T>, std::string_view> ||
    std::is_same_v<std::remove_cvref_t<T>, bool> ||
    std::is_enum_v<std::remove_cvref_t<T>> ||
    std::is_arithmetic_v<std::remove_cvref_t<T>>;

template<typename T>
std::optional<T> parse_value(std::string_view val) {
    using CleanT = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<CleanT, std::string>) {
        return std::string(val);
    } else if constexpr (std::is_same_v<CleanT, std::string_view>) {
        return val;
    } else if constexpr (std::is_same_v<CleanT, bool>) {
        if (val == "true" || val == "1") return true;
        if (val == "false" || val == "0") return false;
        return std::nullopt;
    } else if constexpr (std::is_enum_v<CleanT>) {
        static constexpr auto enums = json::detail::get_enumerator_list<CleanT>();
        template for (constexpr auto e : enums) {
            if (std::meta::identifier_of(e) == val) {
                return [:e:];
            }
        }
        std::int64_t int_val = 0;
        auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), int_val);
        if (ec == std::errc{}) {
            return static_cast<CleanT>(int_val);
        }
        return std::nullopt;
    } else if constexpr (std::is_arithmetic_v<CleanT>) {
        CleanT result{};
        auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), result);
        if (ec != std::errc{}) return std::nullopt;
        return result;
    } else {
        return std::nullopt;
    }
}

template<typename T>
T deserialize_query(const http3::Request& req) {
    T obj{};
#ifdef __cpp_impl_reflection
    static constexpr auto members = get_members<T>();
    template for (constexpr auto m : members) {
        if constexpr (std::meta::is_nonstatic_data_member(m)) {
            constexpr auto name = std::meta::identifier_of(m);
            auto q_val = req.query_param(name);
            if (q_val) {
                using MemberType = std::remove_cvref_t<decltype(obj.*&[:m:])>;
                if constexpr (is_query_parseable_v<MemberType>) {
                    auto parsed = parse_value<MemberType>(*q_val);
                    if (parsed) {
                        obj.*&[:m:] = *parsed;
                    }
                }
            }
        }
    }
#endif
    return obj;
}

#ifdef __cpp_impl_reflection
template<typename Class, std::meta::info m, typename Ret, typename... Args>
struct Invoker {
    static void invoke(Class& obj, Ret (Class::*method)(Args...), http3::Request& req, http3::Response& res, context::RequestContext& ctx) {
        invoke_with_args(obj, method, req, res, ctx, std::make_index_sequence<sizeof...(Args)>{});
    }

    template<std::size_t... Is>
    static void invoke_with_args(Class& obj, Ret (Class::*method)(Args...), http3::Request& req, http3::Response& res, context::RequestContext& ctx, std::index_sequence<Is...>) {
        if constexpr (std::is_void_v<Ret>) {
            (obj.*method)(resolve_arg<Is, Args>(req, res, ctx)...);
        } else {
            auto result = (obj.*method)(resolve_arg<Is, Args>(req, res, ctx)...);
            handle_result(result, res);
        }
    }

    template<std::size_t I, typename Arg>
    static Arg resolve_arg(http3::Request& req, http3::Response& res, context::RequestContext& ctx) {
        using CleanArg = std::remove_cvref_t<Arg>;
        if constexpr (std::is_same_v<CleanArg, http3::Request>) {
            return req;
        } else if constexpr (std::is_same_v<CleanArg, http3::Response>) {
            return res;
        } else if constexpr (std::is_same_v<CleanArg, context::RequestContext>) {
            return ctx;
        } else {
            static constexpr auto params = get_parameters<m>();
            constexpr auto param_reflect = params[I];
            constexpr bool is_struct = std::is_class_v<CleanArg> && 
                                     !std::is_same_v<CleanArg, http3::Request> && 
                                     !std::is_same_v<CleanArg, http3::Response> && 
                                     !std::is_same_v<CleanArg, context::RequestContext> && 
                                     !std::is_same_v<CleanArg, std::string>;
            if constexpr (is_struct) {
                CleanArg body_obj{};
                if (req.method() == "GET" || req.method() == "DELETE") {
                    body_obj = deserialize_query<CleanArg>(req);
                } else {
                    auto parser = std::make_shared<simdjson::dom::parser>();
                    auto doc = parser->parse(req.body());
                    if (doc.error() == simdjson::SUCCESS) {
                        novaboot::json::deserialize_elem(doc.value(), body_obj);
                    }
                    ctx.set<std::shared_ptr<simdjson::dom::parser>>(parser);
                }
                std::vector<std::string> errors;
                if (!novaboot::validation::validate(body_obj, errors)) {
                    throw novaboot::validation::ValidationException(std::move(errors));
                }
                return body_obj;
            } else {
                constexpr auto param_name = std::meta::identifier_of(param_reflect);
                
                // 1. Resolve from path parameter
                if (req.path_params().has(param_name)) {
                    auto val_opt = req.path_params().template get_as<CleanArg>(param_name);
                    if (val_opt) {
                        return *val_opt;
                    }
                }
                
                // 2. Resolve from query parameter
                auto q_val = req.query_param(param_name);
                if (q_val) {
                    auto parsed = parse_value<CleanArg>(*q_val);
                    if (parsed) {
                        return *parsed;
                    }
                }
                
                return CleanArg{};
            }
        }
    }

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
};

#endif

template<typename Ex, typename Handler>
class FluentExceptionHandler : public novaboot::router::ExceptionHandler {
    Handler handler_;

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
    explicit FluentExceptionHandler(Handler&& h) : handler_(std::forward<Handler>(h)) {}

    bool handle(const std::exception& ex, http3::Response& res, context::RequestContext& ctx) override {
        if (auto* typed_ex = dynamic_cast<const Ex*>(&ex)) {
            if constexpr (std::is_invocable_v<Handler, const Ex&, http3::Response&, context::RequestContext&>) {
                handler_(*typed_ex, res, ctx);
            } else if constexpr (std::is_invocable_v<Handler, const Ex&, context::RequestContext&>) {
                auto result = handler_(*typed_ex, ctx);
                handle_result(result, res);
            } else if constexpr (std::is_invocable_v<Handler, const Ex&>) {
                auto result = handler_(*typed_ex);
                handle_result(result, res);
            }
            return true;
        }
        return false;
    }
};

} // namespace detail

/// Server builder for fluent configuration.
///
/// Usage:
///   auto app = novaboot::Server::create()
///       .bind("0.0.0.0", 443)
///       .tls("cert.pem", "key.pem")
///       .workers(4)
///       .build();
///
///   app->route("/api/hello")
///       .get([](auto& req, auto& res, auto& ctx) {
///           res.status(200).body("Hello from NovaBoot!");
///       });
///
///   app->run();
class Server {
public:
    /// Server builder (constructed via Server::create())
    class Builder {
    public:
        Builder() = default;

        /// Set bind address and port
        Builder& bind(std::string_view address, std::uint16_t port);

        /// Set TLS certificate and key paths
        Builder& tls(std::string_view cert_path, std::string_view key_path);

        /// Set the number of worker shards (default: hardware_concurrency)
        Builder& workers(int count);

        /// Add a global middleware
        Builder& middleware(std::shared_ptr<middleware::Middleware> mw);

        /// Set the root DI container for automatic request-scoped injection
        Builder& di_container(di::RootContainer& root);

        /// Set the event loop backend (default: IoUring)
        Builder& backend(core::EventLoopBackend b);

        /// Set the static resources directory (like in Spring Boot)
        Builder& static_resources(std::string_view path);

        /// Build and return the server
        std::unique_ptr<Server> build();

    private:
        std::string   bind_address_ = "0.0.0.0";
        std::uint16_t bind_port_    = 443;
        std::string   cert_path_;
        std::string   key_path_;
        int           worker_count_ = 0; // 0 = auto-detect
        core::EventLoopBackend backend_ = core::EventLoopBackend::IoUring;
        di::RootContainer* di_root_     = nullptr;
        std::vector<std::shared_ptr<middleware::Middleware>> middlewares_;
        std::string   static_resources_dir_;
    };

    /// Create a server builder
    static Builder create();

    ~Server();

    // Non-copyable, non-movable
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Register a route (fluent API)
    router::Router::RouteBuilder route(std::string_view path);

    template<typename Ex, typename Handler>
    Server& on_exception(Handler&& handler) {
        router_.on_exception<Ex>(std::forward<Handler>(handler));
        return *this;
    }

    /// Match and process a thrown exception against fluent handlers.
    bool handle_exception(const std::exception& ex, http3::Response& res, context::RequestContext& ctx);

    /// Run the server (blocks until signaled to stop)
    void run();

    /// Stop the server gracefully
    void stop();

    /// Get the number of active shards
    [[nodiscard]] int worker_count() const noexcept {
        return worker_count_;
    }

    /// Get the router (for testing)
    [[nodiscard]] router::Router& router() noexcept { return router_; }

    /// Get the pipeline (for testing)
    [[nodiscard]] middleware::Pipeline& pipeline() noexcept { return pipeline_; }

private:
    Server() = default;

    /// Install signal handlers for graceful shutdown
    void install_signal_handlers();

    /// Wake and join the signal waiter thread.
    void stop_signal_thread();

    router::Router                 router_;
    middleware::Pipeline           pipeline_;
    std::unique_ptr<quic::TlsContext> tls_ctx_;
    std::vector<std::unique_ptr<core::Shard>> shards_;

    net::Address bind_address_;
    int          worker_count_ = 0;
    std::atomic_bool running_  = false;
    std::atomic_bool stopping_ = false;
    core::EventLoopBackend backend_ = core::EventLoopBackend::IoUring;
    std::string  static_resources_dir_;

    sigset_t     shutdown_signal_set_{};
    std::thread  signal_thread_;

    /// Static reference for compatibility with legacy signal handling.
    static Server* instance_;
};

namespace di {

namespace detail {
template<auto MethodPtr>
struct method_traits;

template<typename Class, typename Ret, typename... Args, Ret (Class::*MethodPtr)(Args...)>
struct method_traits<MethodPtr> {
    using class_type = Class;
    using return_type = Ret;

    template<std::meta::info m>
    using invoker_type = novaboot::detail::Invoker<Class, m, Ret, Args...>;
};

#ifdef __cpp_impl_reflection
template<std::meta::info m, auto MethodPtr>
consteval bool is_matching_method() {
    if constexpr (std::meta::is_function(m) && !std::meta::is_constructor(m) && !std::meta::is_destructor(m)) {
        if constexpr (std::meta::has_identifier(m)) {
            constexpr std::string_view name = std::meta::identifier_of(m);
            if constexpr (name.starts_with("operator")) {
                return false;
            } else {
                if constexpr (std::is_same_v<decltype(&[:m:]), decltype(MethodPtr)>) {
                    return &[:m:] == MethodPtr;
                } else {
                    return false;
                }
            }
        } else {
            return false;
        }
    } else {
        return false;
    }
}

template<typename T, auto MethodPtr>
consteval std::meta::info find_method_meta() {
    static constexpr auto members = novaboot::detail::get_members<T>();
    template for (constexpr auto m : members) {
        if constexpr (is_matching_method<m, MethodPtr>()) {
            return m;
        }
    }
    return {};
}

template<std::meta::info m>
novaboot::db::TransactionOptions transaction_options_from_annotation() {
    constexpr auto transactional = novaboot::detail::get_annotation<novaboot::annotations::Transactional>(m);
    novaboot::db::TransactionOptions options;
    options.propagation = transactional.propagation;
    options.isolation = transactional.isolation;
    options.read_only = transactional.read_only;
    options.timeout_seconds = transactional.timeout_seconds;
    return options;
}
#endif
} // namespace detail

#ifdef __cpp_impl_reflection
template<auto MethodPtr>
auto handler() {
    return [](http3::Request& req, http3::Response& res, context::RequestContext& ctx) {
        using Traits = detail::method_traits<MethodPtr>;
        using Class = typename Traits::class_type;
        constexpr auto m_meta = detail::find_method_meta<Class, MethodPtr>();
        using InvokerType = typename Traits::template invoker_type<m_meta>;

        auto& controller = ctx.inject<Class>();
        if constexpr (novaboot::detail::has_annotation<novaboot::annotations::Transactional>(m_meta)) {
            auto& transactions = ctx.inject<novaboot::db::TransactionManager>();
            auto options = detail::transaction_options_from_annotation<m_meta>();
            transactions.execute(options, [&](std::shared_ptr<novaboot::db::Connection>) {
                InvokerType::invoke(controller, MethodPtr, req, res, ctx);
            });
        } else {
            InvokerType::invoke(controller, MethodPtr, req, res, ctx);
        }
    };
}
#endif

} // namespace di

} // namespace novaboot
