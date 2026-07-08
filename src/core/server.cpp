#include "novaboot/core/server.h"

#include <csignal>
#include <format>
#include <stdexcept>
#include <thread>

#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <spdlog/spdlog.h>

namespace novaboot {

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

    // Add middleware
    for (auto& mw : middlewares_) {
        server->pipeline_.add(std::move(mw));
    }

    // Set backend
    server->backend_ = backend_;

    spdlog::info("NovaBoot server configured:");
    spdlog::info("  Bind:    {}", server->bind_address_.to_string());
    spdlog::info("  Workers: {}", server->worker_count_);
    spdlog::info("  Backend: {}",
                 server->backend_ == core::EventLoopBackend::IoUring
                     ? "io_uring" : "epoll");
    spdlog::info("  TLS:     {} / {}", cert_path_, key_path_);

    return server;
}

// ─── Server ──────────────────────────────────────────────────────────────────

Server::~Server() {
    stop();
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

router::Router::RouteBuilder Server::route(std::string_view path) {
    return router_.route(path);
}

void Server::run() {
    if (running_) return;
    running_ = true;

    // Initialize ngtcp2 crypto library
    if (ngtcp2_crypto_ossl_init() != 0) {
        throw std::runtime_error("ngtcp2_crypto_ossl_init failed");
    }

    // Install signal handlers
    install_signal_handlers();

    spdlog::info("Starting {} shard(s) on {}...",
                 worker_count_, bind_address_.to_string());

    // Create and start shards
    shards_.reserve(static_cast<std::size_t>(worker_count_));

    for (int i = 0; i < worker_count_; ++i) {
        core::ShardConfig shard_config;
        shard_config.shard_id     = i;
        shard_config.cpu_core     = i; // Pin shard i to core i
        shard_config.bind_address = bind_address_;
        shard_config.backend      = backend_;

        auto shard = std::make_unique<core::Shard>(
            shard_config, *tls_ctx_, router_, pipeline_);
        shard->start();
        shards_.push_back(std::move(shard));
    }

    spdlog::info("NovaBoot server running. Press Ctrl+C to stop.");

    // Wait for all shards to finish
    for (auto& shard : shards_) {
        shard->join();
    }

    running_ = false;
    ngtcp2_crypto_ossl_free();
    spdlog::info("NovaBoot server stopped.");
}

void Server::stop() {
    if (!running_) return;

    spdlog::info("Shutting down NovaBoot server...");

    for (auto& shard : shards_) {
        shard->stop();
    }

    running_ = false;
}

void Server::install_signal_handlers() {
    instance_ = this;

    struct sigaction sa{};
    sa.sa_handler = [](int /*sig*/) {
        if (Server::instance_) {
            Server::instance_->stop();
        }
    };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Ignore SIGPIPE (common with network code)
    signal(SIGPIPE, SIG_IGN);
}

} // namespace novaboot
