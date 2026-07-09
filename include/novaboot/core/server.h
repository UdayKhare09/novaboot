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

namespace novaboot {

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
