#include "novaboot/core/shard.h"
#include "novaboot/http3/http3_session.h"

#include <format>

#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>

#include <spdlog/spdlog.h>

namespace novaboot::core {

Shard::Shard(const ShardConfig& config,
             const quic::TlsContext& tls_ctx,
             const router::Router& router,
             const middleware::Pipeline& pipeline)
    : config_(config),
      tls_ctx_(tls_ctx),
      router_(router),
      pipeline_(pipeline) {

    // Pre-allocate receive buffer (enough for batched GRO receives)
    recv_buf_.resize(net::kMaxPacketSize);
}

Shard::~Shard() {
    stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Shard::start() {
    thread_ = std::thread([this]() { run(); });
}

void Shard::stop() {
    if (event_loop_) {
        event_loop_->stop();
    }
}

void Shard::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool Shard::is_running() const noexcept {
    return event_loop_ && event_loop_->is_running();
}

void Shard::run() {
    spdlog::info("Shard {} starting on core {}",
                 config_.shard_id, config_.cpu_core);

    // Pin thread to CPU core
    if (config_.cpu_core >= 0) {
        pin_to_core();
    }

    // Create the event loop
    event_loop_ = std::make_unique<EpollEventLoop>();

    // Create and bind the UDP socket
    net::UdpSocketConfig sock_config;
    sock_config.bind_address = config_.bind_address;
    sock_config.reuse_port   = true;
    sock_config.enable_gro   = true;
    sock_config.enable_gso   = true;

    auto sock_result = net::UdpSocket::create(sock_config);
    if (!sock_result) {
        spdlog::error("Shard {}: Failed to create UDP socket",
                      config_.shard_id);
        return;
    }
    socket_ = std::make_unique<net::UdpSocket>(std::move(*sock_result));

    // Create the connection manager
    conn_mgr_ = std::make_unique<quic::ConnectionManager>(
        tls_ctx_, *event_loop_);

    // Wire up callbacks
    conn_mgr_->set_send_callback(
        [this](const net::OutgoingPacket& pkt) { send_packet(pkt); });

    conn_mgr_->set_handshake_callback(
        [this](quic::QuicConnection& conn) {
            // Create HTTP/3 session when a new connection arrives
            auto h3_session = http3::Http3Session::create(
                conn.native_handle(),
                [this](http3::Http3Stream& stream) {
                    on_request(stream);
                });
            conn.set_http3_session(std::move(h3_session));
        });

    // Register the UDP socket with the event loop
    event_loop_->add_fd(socket_->fd(), EPOLLIN,
        [this](uint32_t events) { on_socket_readable(events); });

    // Periodic cleanup of closed connections (every 5 seconds)
    cleanup_timer_ = event_loop_->add_timer(
        std::chrono::seconds(5),
        [this]() {
            conn_mgr_->cleanup();
            // Re-schedule (one-shot timer, so we need to re-add)
            cleanup_timer_ = event_loop_->add_timer(
                std::chrono::seconds(5),
                [this]() { conn_mgr_->cleanup(); });
        });

    spdlog::info("Shard {} ready (fd={}, connections=0)",
                 config_.shard_id, socket_->fd());

    // Run the event loop (blocks until stop() is called)
    event_loop_->run();

    spdlog::info("Shard {} stopped", config_.shard_id);
}

void Shard::pin_to_core() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(config_.cpu_core, &cpuset);

    int rv = pthread_setaffinity_np(
        pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rv != 0) {
        spdlog::warn("Shard {}: Failed to pin to core {} (error {})",
                     config_.shard_id, config_.cpu_core, rv);
    } else {
        spdlog::info("Shard {}: Pinned to core {}",
                     config_.shard_id, config_.cpu_core);
    }
}

void Shard::on_socket_readable(uint32_t /*events*/) {
    // Edge-triggered: drain all available packets
    for (;;) {
        auto result = socket_->recv_one(recv_buf_);

        if (!result) {
            if (result.error() == net::UdpSocket::Error::WouldBlock) {
                break; // No more data available
            }
            spdlog::warn("Shard {}: recv error", config_.shard_id);
            break;
        }

        auto& packet = *result;

        // Route the packet to the connection manager
        conn_mgr_->on_packet(packet);
    }
}

void Shard::on_request(http3::Http3Stream& stream) {
    auto& req = stream.request();
    auto& res = stream.response();

    // Route the request
    auto result = router_.match(req.method(), req.path());

    if (!result.handler) {
        // 404 Not Found
        res.status(404)
           .header("content-type", "text/plain")
           .body("Not Found");
        return;
    }

    // Set path parameters on the request
    req.path_params() = std::move(result.params);

    // Execute through middleware pipeline
    context::RequestContext ctx;
    pipeline_.execute(req, res, ctx, *result.handler);
}

void Shard::send_packet(const net::OutgoingPacket& packet) {
    auto result = socket_->send_one(packet);
    if (!result) {
        if (result.error() != net::UdpSocket::Error::WouldBlock) {
            spdlog::warn("Shard {}: send error", config_.shard_id);
        }
        // TODO: Queue for retry on EAGAIN
    }
}

} // namespace novaboot::core
