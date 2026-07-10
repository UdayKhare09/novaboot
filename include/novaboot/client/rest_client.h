#pragma once

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "novaboot/async/task.h"
#include "novaboot/core/event_loop.h"
#include "novaboot/http3/client_response.h"
#include "novaboot/http3/header_map.h"
#include "novaboot/net/address.h"
#include "novaboot/net/packet.h"
#include "novaboot/net/udp_socket.h"
#include "novaboot/quic/tls_context.h"

namespace novaboot::quic { class QuicClientConnection; }
namespace novaboot::client {

/// Exception thrown for network-level failures (connection drop, timeout).
class ClientError : public std::runtime_error {
public:
    explicit ClientError(const std::string& msg) : std::runtime_error(msg) {}
};

/// High-level HTTP/3 REST client.
///
/// Owns a single persistent QUIC connection. Requests are multiplexed as
/// independent streams. If the connection drops, the client automatically
/// attempts reconnection with exponential backoff before the next request.
///
/// The client registers its UDP socket on the caller-provided EventLoop,
/// so it is driven by the same shard thread as the server — no extra thread.
///
/// Thread-safety: NOT thread-safe. Must be used from the EventLoop thread only.
class RestClient {
public:
    struct Config {
        std::string  host;                       ///< Hostname (for SNI + DNS)
        std::string  ip;                         ///< Pre-resolved IP (if empty, resolves host)
        uint16_t     port         = 443;
        bool         verify_ssl   = true;        ///< Verify server certificate
        std::string  ca_file;                    ///< Custom CA PEM (empty = system bundle)
        int          connect_timeout_ms = 5000;  ///< Handshake timeout
        int          request_timeout_ms = 30000; ///< Per-request timeout
        int          max_reconnect_attempts = 5; ///< Max reconnect tries
    };

    ~RestClient();

    RestClient(const RestClient&) = delete;
    RestClient& operator=(const RestClient&) = delete;

    /// Create and connect the REST client.
    /// @param cfg       Connection configuration
    /// @param event_loop The shard event loop to register the UDP socket on
    static std::unique_ptr<RestClient> create(const Config& cfg,
                                              core::EventLoop& event_loop);

    // ─── Synchronous API (blocks current thread until response/timeout) ──
    http3::ClientResponse get(std::string_view path,
                              const http3::HeaderMap& headers = {});
    http3::ClientResponse post(std::string_view path,
                               std::string_view body,
                               const http3::HeaderMap& headers = {});
    http3::ClientResponse put(std::string_view path,
                              std::string_view body,
                              const http3::HeaderMap& headers = {});
    http3::ClientResponse del(std::string_view path,
                              const http3::HeaderMap& headers = {});
    http3::ClientResponse patch(std::string_view path,
                                std::string_view body,
                                const http3::HeaderMap& headers = {});

    // ─── Typed convenience (defined in rest_client_typed.h, include json.h first)
    // template<typename T> T get_as(...)  — see rest_client_factory.h

    // ─── Async / coroutine API ───────────────────────────────────────────
    /// Returns a Task<ClientResponse> that can be co_await-ed.
    async::Task<http3::ClientResponse> async_get(
        std::string_view path, const http3::HeaderMap& headers = {});
    async::Task<http3::ClientResponse> async_post(
        std::string_view path, std::string_view body,
        const http3::HeaderMap& headers = {});
    async::Task<http3::ClientResponse> async_put(
        std::string_view path, std::string_view body,
        const http3::HeaderMap& headers = {});
    async::Task<http3::ClientResponse> async_del(
        std::string_view path, const http3::HeaderMap& headers = {});
    async::Task<http3::ClientResponse> async_patch(
        std::string_view path, std::string_view body,
        const http3::HeaderMap& headers = {});

    /// True if the QUIC connection is established and ready
    [[nodiscard]] bool is_connected() const noexcept;

    /// Block until QUIC+TLS handshake is complete or timeout fires.
    void wait_for_connection();

private:
    RestClient();

    void do_connect();
    void do_reconnect();

    /// Submit a request and return a Task that resolves when the response arrives.
    async::Task<http3::ClientResponse> async_request(
        std::string_view method,
        std::string_view path,
        std::string_view body,
        const http3::HeaderMap& headers);

    /// Blocking wrapper: runs the event loop until the Task resolves.
    http3::ClientResponse sync_execute(async::Task<http3::ClientResponse> task);

    void on_packet_received(net::IncomingPacket&& pkt);
    void send_packet(const net::OutgoingPacket& pkt);
    void on_disconnect();

    Config            cfg_;
    core::EventLoop*  event_loop_ = nullptr;
    std::optional<quic::TlsContext>  tls_ctx_;

    std::unique_ptr<net::UdpSocket>              socket_;
    std::unique_ptr<quic::QuicClientConnection>  conn_;

    /// Pending response callbacks keyed by stream_id
    std::unordered_map<int64_t,
        std::pair<std::optional<http3::ClientResponse>,
                  std::coroutine_handle<>>> pending_;

    // Reconnection state
    int    reconnect_attempts_  = 0;
    bool   reconnect_pending_   = false;
    core::TimerHandle reconnect_timer_;

    std::chrono::milliseconds next_backoff_{100};
    static constexpr std::chrono::milliseconds kMaxBackoff{30000};
};

} // namespace novaboot::client
