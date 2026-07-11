#pragma once

#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "novaboot/core/event_loop.h"
#include "novaboot/middleware/pipeline.h"
#include "novaboot/net/udp_socket.h"
#include "novaboot/quic/connection_manager.h"
#include "novaboot/quic/tls_context.h"
#include "novaboot/router/router.h"

namespace novaboot::http3 {
class Http3Stream;
}

#include "novaboot/net/tcp_listener.h"
#include "novaboot/net/tcp_connection_manager.h"

namespace novaboot::core {

/// Per-core shard configuration
struct ShardConfig {
    int               shard_id = 0;        ///< Shard index (0-based)
    int               cpu_core = -1;       ///< CPU core to pin to (-1 = no pinning)
    net::Address      bind_address;        ///< Address to bind UDP socket
    EventLoopBackend  backend = EventLoopBackend::IoUring; ///< Event loop backend
    std::string       static_resources_dir; ///< Configured static resources directory path
};

/// Per-core shard — the fundamental execution unit.
///
/// Each shard owns:
///   - An EventLoop (io_uring-based)
///   - A UDP socket (SO_REUSEPORT)
///   - A ConnectionManager (CID → QUIC connections)
///   - References to the shared Router and Middleware pipeline
///
/// The shard runs on its own thread, pinned to a specific CPU core.
/// There is NO shared mutable state between shards.
class Shard {
public:
    Shard(const ShardConfig& config,
          const quic::TlsContext& tls_ctx,
          const router::Router& router,
          const middleware::Pipeline& pipeline);
    ~Shard();

    // Non-copyable, non-movable
    Shard(const Shard&) = delete;
    Shard& operator=(const Shard&) = delete;

    /// Start the shard on its own thread
    void start();

    /// Signal the shard to stop
    void stop();

    /// Wait for the shard thread to finish
    void join();

    /// Check if the shard is running
    [[nodiscard]] bool is_running() const noexcept;

    /// Get the shard ID
    [[nodiscard]] int id() const noexcept { return config_.shard_id; }

private:
    /// Main shard loop (runs on the shard thread)
    void run();

    /// Pin the current thread to the configured CPU core
    void pin_to_core();

    /// Handle an HTTP request (called by Http3Session when request is ready)
    void on_request(http3::Http3Stream& stream);

    /// Attempt to serve a static file from static resources directory
    bool serve_static_file(std::string_view path, http3::Response& res, bool head_only);

    /// Send a packet via the UDP socket
    void send_packet(const net::OutgoingPacket& packet);

    /// Periodically cleanup connections
    void schedule_cleanup();

    ShardConfig                   config_;
    const quic::TlsContext&       tls_ctx_;
    const router::Router&         router_;
    const middleware::Pipeline&   pipeline_;

    std::unique_ptr<EventLoop>                  event_loop_;
    std::unique_ptr<net::UdpSocket>         socket_;
    std::unique_ptr<quic::ConnectionManager> conn_mgr_;

    std::unique_ptr<net::TcpListener>           tcp_listener_;
    std::unique_ptr<net::TcpConnectionManager>  tcp_conn_mgr_;

    std::thread thread_;

    /// Periodic cleanup timer
    TimerHandle cleanup_timer_;
};

} // namespace novaboot::core
