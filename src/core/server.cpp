#include "novaboot/core/server.h"
#include <algorithm>

#include <csignal>
#include <format>
#include <pthread.h>
#include <stdexcept>
#include <thread>

#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <spdlog/spdlog.h>

#include "novaboot/di/di.h"
#include "novaboot/actuator/actuator.h"
#include "novaboot/middleware/middleware.h"

namespace novaboot {

class DIMiddleware : public middleware::Middleware {
public:
    explicit DIMiddleware(di::RootContainer& root) : root_(root) {}

    void handle(http3::Request&, http3::Response&,
                context::RequestContext& ctx, Next next) override {
        static thread_local std::unique_ptr<di::ShardContainer> shard_di = nullptr;
        if (!shard_di) {
            shard_di = root_.make_shard_container();
            shard_di->initialize();
        }

        auto request_di = shard_di->make_request_container();
        ctx.bind_container(*request_di);
        next();
    }

private:
    di::RootContainer& root_;
};

Server* Server::instance_ = nullptr;

// ─── Builder ─────────────────────────────────────────────────────────────────

Server::Builder Server::create() {
    return Builder{};
}

Server::Builder& Server::Builder::bind(std::string_view address,
                                       std::uint16_t port) {
    bind_address_ = std::string(address);
    bind_port_    = port;
    return *this;
}

Server::Builder& Server::Builder::tls(std::string_view cert_path,
                                      std::string_view key_path) {
    cert_path_ = std::string(cert_path);
    key_path_  = std::string(key_path);
    return *this;
}

Server::Builder& Server::Builder::workers(int count) {
    worker_count_ = count;
    return *this;
}

Server::Builder& Server::Builder::middleware(
    std::shared_ptr<middleware::Middleware> mw) {
    middlewares_.push_back(std::move(mw));
    return *this;
}

Server::Builder& Server::Builder::backend(core::EventLoopBackend b) {
    backend_ = b;
    return *this;
}

Server::Builder& Server::Builder::static_resources(std::string_view path) {
    static_resources_dir_ = std::string(path);
    return *this;
}

Server::Builder& Server::Builder::expose_error_details(bool enabled) {
    expose_error_details_ = enabled;
    return *this;
}

Server::Builder& Server::Builder::shutdown_timeout(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) throw std::invalid_argument("Shutdown timeout must not be negative");
    shutdown_timeout_ = timeout;
    return *this;
}

Server::Builder& Server::Builder::shutdown_participant(core::ShutdownParticipant participant) {
    if (participant.name.empty()) throw std::invalid_argument("Shutdown participant name must not be empty");
    shutdown_participants_.push_back(std::move(participant));
    return *this;
}

Server::Builder& Server::Builder::actuator(std::shared_ptr<actuator::Actuator> actuator) {
    if (!actuator) {
        throw std::invalid_argument("Actuator must not be null");
    }
    actuators_.push_back(std::move(actuator));
    return *this;
}

Server::Builder& Server::Builder::di_container(di::RootContainer& root) {
    di_root_ = &root;
    return *this;
}

std::unique_ptr<Server> Server::Builder::build() {
    // Validate configuration
    if (cert_path_.empty()) {
        throw std::invalid_argument(
            "TLS certificate path is required. "
            "Call .tls(cert_path, key_path) on the builder.");
    }
    if (key_path_.empty()) {
        throw std::invalid_argument(
            "TLS private key path is required. "
            "Call .tls(cert_path, key_path) on the builder.");
    }

    auto server = std::unique_ptr<Server>(new Server());
    server->pipeline_.add(server->http_drain_);

    // Resolve bind address
    server->bind_address_ = net::Address::from_string(
        bind_address_, bind_port_);

    // Worker count: default to hardware concurrency
    server->worker_count_ = worker_count_ > 0
        ? worker_count_
        : static_cast<int>(std::thread::hardware_concurrency());
    if (server->worker_count_ == 0) {
        server->worker_count_ = 1;
    }

    // Create TLS context
    quic::TlsContext::Config tls_config;
    tls_config.cert_file = cert_path_;
    tls_config.key_file  = key_path_;
    server->tls_ctx_ = std::make_unique<quic::TlsContext>(
        quic::TlsContext::create(tls_config));

    // Add middleware (inject DIMiddleware first if DI container is registered)
    if (di_root_) {
        server->pipeline_.add(std::make_shared<DIMiddleware>(*di_root_));
        di_root_->register_routes_and_advice(server->router());

        // Gather all middlewares from the DI container
        struct OrderedMiddleware {
            std::shared_ptr<middleware::Middleware> middleware;
            int order;
        };
        std::vector<OrderedMiddleware> di_middlewares;

        for (auto& [tid, reg] : di_root_->get_registrations()) {
            if (reg.get_middleware_fn) {
                void* instance = di_root_->resolve(tid);
                if (instance) {
                    auto mw = reg.get_middleware_fn(instance);
                    if (mw) {
                        di_middlewares.push_back({std::move(mw), reg.middleware_order});
                    }
                }
            }
        }

        // Sort middlewares by order ascending
        std::stable_sort(di_middlewares.begin(), di_middlewares.end(),
                         [](const OrderedMiddleware& a, const OrderedMiddleware& b) {
                             return a.order < b.order;
                         });

        // Add them to pipeline
        for (auto& omw : di_middlewares) {
            server->pipeline_.add(std::move(omw.middleware));
        }
    }
    for (auto& mw : middlewares_) {
        server->pipeline_.add(std::move(mw));
    }

    server->actuators_ = std::move(actuators_);
    server->shutdown_timeout_ = shutdown_timeout_;
    for (auto& participant : shutdown_participants_) {
        server->shutdown_coordinator_.add(std::move(participant));
    }
    server->shutdown_coordinator_.add(core::ShutdownParticipant{
        .name = "http-requests",
        .stop_accepting = [gate = server->http_drain_] { gate->stop_accepting(); },
        .drained = [gate = server->http_drain_] { return gate->drained(); },
        .force_close = {},
    });
    server->shutdown_coordinator_.add(core::ShutdownParticipant{
        .name = "websocket-sessions",
        .stop_accepting = [sessions = server->websocket_sessions_] { sessions->close_all(1001, "Server shutting down"); },
        .drained = [sessions = server->websocket_sessions_] { return sessions->size() == 0; },
        .force_close = [sessions = server->websocket_sessions_] { sessions->close_all(1001, "Server shutdown deadline exceeded"); },
    });
    for (const auto& actuator : server->actuators_) {
        actuator->register_routes(server->router_);
        server->pipeline_.add(actuator->observation_middleware());
    }

    // Set backend
    server->backend_ = backend_;
    server->static_resources_dir_ = static_resources_dir_;
    server->expose_error_details_ = expose_error_details_;

    spdlog::debug("NovaBoot configured: bind={}, workers={}, backend=io_uring, tls={} / {}",
                  server->bind_address_.to_string(),
                  server->worker_count_,
                  cert_path_,
                  key_path_);
    if (!static_resources_dir_.empty()) {
        spdlog::debug("Static resources: {}", static_resources_dir_);
    }
    if (di_root_) {
        spdlog::debug("DI: {} registrations, {} singleton(s), {} route registrar(s)",
                      di_root_->registration_count(),
                      di_root_->singleton_count(),
                      di_root_->route_registrar_count());
    }
    spdlog::debug("Routes: {} route(s), {} exception handler(s)",
                  server->router_.size(),
                  server->router_.exception_handler_count());
    for (const auto& route : server->router_.routes()) {
        spdlog::debug("  {} {}", router::method_to_string(route.method), route.pattern);
    }
    for (const auto& path : server->router_.websocket_routes()) {
        spdlog::debug("  WS {}", path);
    }

    return server;
}

// ─── Server ──────────────────────────────────────────────────────────────────

Server::~Server() {
    stop();
    stop_signal_thread();
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

router::Router::RouteBuilder Server::route(std::string_view path) {
    return router_.route(path);
}

void Server::run() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    stopping_ = false;
    ready_ = false;

    // Initialize ngtcp2 crypto library
    if (ngtcp2_crypto_ossl_init() != 0) {
        running_ = false;
        throw std::runtime_error("ngtcp2_crypto_ossl_init failed");
    }

    // Install signal handlers
    install_signal_handlers();

    spdlog::debug("Starting NovaBoot: {} shard(s) on {}",
                  worker_count_, bind_address_.to_string());

    // Create and start shards
    shards_.reserve(static_cast<std::size_t>(worker_count_));

    for (int i = 0; i < worker_count_; ++i) {
        core::ShardConfig shard_config;
        shard_config.shard_id     = i;
        shard_config.cpu_core     = i; // Pin shard i to core i
        shard_config.bind_address = bind_address_;
        shard_config.backend      = backend_;
        shard_config.static_resources_dir = static_resources_dir_;
        shard_config.expose_error_details = expose_error_details_;

        auto shard = std::make_unique<core::Shard>(
            shard_config, *tls_ctx_, router_, pipeline_);
        shard->start();
        shards_.push_back(std::move(shard));
    }

    // Do not report readiness until every shard has bound both UDP and TCP
    // listeners. This is also the lifecycle contract used by LiveServer tests.
    while (!stopping_) {
        bool all_ready = !shards_.empty();
        for (const auto& shard : shards_) all_ready = all_ready && shard->is_ready();
        if (all_ready) {
            ready_ = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    for (const auto& actuator : actuators_) {
        actuator->set_availability(actuator::Availability::Ready);
    }

    spdlog::info("NovaBoot running on {} ({} worker{}). Press Ctrl+C to stop.",
                 bind_address_.to_string(), worker_count_, worker_count_ == 1 ? "" : "s");

    // Wait for all shards to finish
    for (auto& shard : shards_) {
        shard->join();
    }

    running_ = false;
    stopping_ = false;
    ready_ = false;
    for (const auto& actuator : actuators_) {
        actuator->set_availability(actuator::Availability::Stopped);
    }
    stop_signal_thread();
    ngtcp2_crypto_ossl_free();
    spdlog::info("NovaBoot server stopped.");
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) {
        logger->flush();
    });
}

void Server::stop() {
    if (!running_) return;

    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true)) return;
    ready_ = false;

    spdlog::info("Shutting down NovaBoot server...");

    for (const auto& actuator : actuators_) {
        actuator->set_availability(actuator::Availability::RefusingTraffic);
    }

    const auto drain = shutdown_coordinator_.drain_for(shutdown_timeout_);
    if (!drain.drained) {
        spdlog::warn("Forced shutdown of {} participant(s) after {}ms",
                     drain.forced_participants.size(), shutdown_timeout_.count());
    }

    for (auto& shard : shards_) {
        shard->stop();
    }

    running_ = false;

    if (signal_thread_.joinable() &&
        signal_thread_.get_id() != std::this_thread::get_id()) {
        pthread_kill(signal_thread_.native_handle(), SIGTERM);
    }
}

void Server::install_signal_handlers() {
    instance_ = this;

    sigemptyset(&shutdown_signal_set_);
    sigaddset(&shutdown_signal_set_, SIGINT);
    sigaddset(&shutdown_signal_set_, SIGTERM);

    const int mask_result =
        pthread_sigmask(SIG_BLOCK, &shutdown_signal_set_, nullptr);
    if (mask_result != 0) {
        spdlog::warn("Failed to block shutdown signals: {}", mask_result);
    }

    // Ignore SIGPIPE (common with network code)
    signal(SIGPIPE, SIG_IGN);

    if (signal_thread_.joinable()) return;

    signal_thread_ = std::thread([this]() {
        for (;;) {
            int signal_number = 0;
            const int wait_result =
                sigwait(&shutdown_signal_set_, &signal_number);
            if (wait_result != 0) {
                continue;
            }

            if (!running_) {
                break;
            }

            spdlog::info("Received shutdown signal {}", signal_number);
            stop();
            break;
        }
    });
}

void Server::stop_signal_thread() {
    if (!signal_thread_.joinable()) return;
    if (signal_thread_.get_id() == std::this_thread::get_id()) return;

    pthread_kill(signal_thread_.native_handle(), SIGTERM);
    signal_thread_.join();
}

bool Server::handle_exception(const std::exception& ex, http3::Response& res, context::RequestContext& ctx) {
    return router_.handle_exception(ex, res, ctx);
}

} // namespace novaboot
