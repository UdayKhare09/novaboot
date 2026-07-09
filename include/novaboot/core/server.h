#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "novaboot/core/shard.h"
#include "novaboot/middleware/middleware.h"
#include "novaboot/middleware/pipeline.h"
#include "novaboot/quic/tls_context.h"
#include "novaboot/router/router.h"
#include "novaboot/router/web_attributes.h"
#include "novaboot/router/response_entity.h"
#include "novaboot/router/json.h"
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
#endif
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
            constexpr bool is_body = std::is_class_v<CleanArg> && 
                                     !std::is_same_v<CleanArg, http3::Request> && 
                                     !std::is_same_v<CleanArg, http3::Response> && 
                                     !std::is_same_v<CleanArg, context::RequestContext> && 
                                     !std::is_same_v<CleanArg, std::string>;
            if constexpr (is_body) {
                return novaboot::json::deserialize<CleanArg>(req.body());
            } else {
                constexpr auto param_name = std::meta::identifier_of(param_reflect);
                auto val_opt = req.path_params().template get_as<CleanArg>(param_name);
                if (val_opt) {
                    return *val_opt;
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
    };

    /// Create a server builder
    static Builder create();

    ~Server();

    // Non-copyable, non-movable
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Register a route (fluent API)
    router::Router::RouteBuilder route(std::string_view path);

    /// Register a controller's annotated web routes.
    template<typename T>
    Server& register_controller() {
#ifdef __cpp_impl_reflection
        static constexpr auto members = detail::get_members<T>();
        template for (constexpr auto m : members) {
            if constexpr (std::meta::is_function(m) && !std::meta::is_constructor(m) && !std::meta::is_destructor(m)) {
                // GET
                if constexpr (!std::meta::annotations_of_with_type(m, ^^novaboot::web::get).empty()) {
                    constexpr auto ann = std::meta::annotations_of_with_type(m, ^^novaboot::web::get)[0];
                    constexpr auto route_annot = std::meta::extract<novaboot::web::get>(ann);
                    this->deduce_and_bind<T, m>(route_annot.path, router::Method::GET, &[:m:]);
                }
                // POST
                else if constexpr (!std::meta::annotations_of_with_type(m, ^^novaboot::web::post).empty()) {
                    constexpr auto ann = std::meta::annotations_of_with_type(m, ^^novaboot::web::post)[0];
                    constexpr auto route_annot = std::meta::extract<novaboot::web::post>(ann);
                    this->deduce_and_bind<T, m>(route_annot.path, router::Method::POST, &[:m:]);
                }
                // PUT
                else if constexpr (!std::meta::annotations_of_with_type(m, ^^novaboot::web::put).empty()) {
                    constexpr auto ann = std::meta::annotations_of_with_type(m, ^^novaboot::web::put)[0];
                    constexpr auto route_annot = std::meta::extract<novaboot::web::put>(ann);
                    this->deduce_and_bind<T, m>(route_annot.path, router::Method::PUT, &[:m:]);
                }
                // DELETE
                else if constexpr (!std::meta::annotations_of_with_type(m, ^^novaboot::web::del).empty()) {
                    constexpr auto ann = std::meta::annotations_of_with_type(m, ^^novaboot::web::del)[0];
                    constexpr auto route_annot = std::meta::extract<novaboot::web::del>(ann);
                    this->deduce_and_bind<T, m>(route_annot.path, router::Method::DELETE_, &[:m:]);
                }
                // PATCH
                else if constexpr (!std::meta::annotations_of_with_type(m, ^^novaboot::web::patch).empty()) {
                    constexpr auto ann = std::meta::annotations_of_with_type(m, ^^novaboot::web::patch)[0];
                    constexpr auto route_annot = std::meta::extract<novaboot::web::patch>(ann);
                    this->deduce_and_bind<T, m>(route_annot.path, router::Method::PATCH, &[:m:]);
                }
                // HEAD
                else if constexpr (!std::meta::annotations_of_with_type(m, ^^novaboot::web::head).empty()) {
                    constexpr auto ann = std::meta::annotations_of_with_type(m, ^^novaboot::web::head)[0];
                    constexpr auto route_annot = std::meta::extract<novaboot::web::head>(ann);
                    this->deduce_and_bind<T, m>(route_annot.path, router::Method::HEAD, &[:m:]);
                }
                // OPTIONS
                else if constexpr (!std::meta::annotations_of_with_type(m, ^^novaboot::web::options).empty()) {
                    constexpr auto ann = std::meta::annotations_of_with_type(m, ^^novaboot::web::options)[0];
                    constexpr auto route_annot = std::meta::extract<novaboot::web::options>(ann);
                    this->deduce_and_bind<T, m>(route_annot.path, router::Method::OPTIONS, &[:m:]);
                }
                // ANY
                else if constexpr (!std::meta::annotations_of_with_type(m, ^^novaboot::web::any).empty()) {
                    constexpr auto ann = std::meta::annotations_of_with_type(m, ^^novaboot::web::any)[0];
                    constexpr auto route_annot = std::meta::extract<novaboot::web::any>(ann);
                    this->deduce_and_bind<T, m>(route_annot.path, router::Method::ANY, &[:m:]);
                }
            }
        }
#endif
        return *this;
    }

    /// Deduce and bind helper that registers type-safe controller methods to the router
    template<typename Class, std::meta::info method_meta, typename Ret, typename... Args>
    void deduce_and_bind(std::string_view path, router::Method http_method, Ret (Class::*method)(Args...)) {
        this->router_.add_route(http_method, path, [method](http3::Request& req, http3::Response& res, context::RequestContext& ctx) {
            auto& controller = ctx.inject<Class>();
            detail::Invoker<Class, method_meta, Ret, Args...>::invoke(controller, method, req, res, ctx);
        });
    }

    /// Register multiple controllers' annotated web routes.
    template<typename... Ts>
    Server& register_controllers() {
        (register_controller<Ts>(), ...);
        return *this;
    }

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

private:
    Server() = default;

    /// Install signal handlers for graceful shutdown
    void install_signal_handlers();

    router::Router                 router_;
    middleware::Pipeline           pipeline_;
    std::unique_ptr<quic::TlsContext> tls_ctx_;
    std::vector<std::unique_ptr<core::Shard>> shards_;

    net::Address bind_address_;
    int          worker_count_ = 0;
    bool         running_      = false;
    core::EventLoopBackend backend_ = core::EventLoopBackend::IoUring;

    /// Static reference for signal handler
    static Server* instance_;
};

} // namespace novaboot
