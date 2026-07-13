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
#include "novaboot/net/tls_tcp_stream.h"
#include "novaboot/quic/tls_context.h"
#include <nghttp2/nghttp2.h>

namespace novaboot::quic { class QuicClientConnection; }

namespace novaboot::client {

enum class Protocol {
    HTTP3,
    HTTP2,
    HTTP1_1
};

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
        Protocol     protocol = Protocol::HTTP3; ///< HTTP protocol option
    };

    class Builder {
    private:
        Config cfg_;
    public:
        Builder() = default;

        Builder& host(std::string host) { cfg_.host = std::move(host); return *this; }
        Builder& ip(std::string ip) { cfg_.ip = std::move(ip); return *this; }
        Builder& port(uint16_t port) { cfg_.port = port; return *this; }
        Builder& verify_ssl(bool verify) { cfg_.verify_ssl = verify; return *this; }
        Builder& ca_file(std::string ca_file) { cfg_.ca_file = std::move(ca_file); return *this; }
        Builder& connect_timeout_ms(int ms) { cfg_.connect_timeout_ms = ms; return *this; }
        Builder& request_timeout_ms(int ms) { cfg_.request_timeout_ms = ms; return *this; }
        Builder& max_reconnect_attempts(int attempts) { cfg_.max_reconnect_attempts = attempts; return *this; }
        Builder& protocol(Protocol proto) { cfg_.protocol = proto; return *this; }

        std::unique_ptr<RestClient> build(core::EventLoop& event_loop) {
            return RestClient::create(cfg_, event_loop);
        }
    };

    static Builder builder() {
        return Builder();
    }

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
    struct H2PendingStream {
        http3::ClientResponse response;
        std::coroutine_handle<> coroutine;
        bool complete = false;
    };

    RestClient();

    void do_connect();
    void do_reconnect();

    void do_connect_tcp();
    void handle_tcp_readable();
    void handle_tcp_writable();
    void process_decrypted_tcp_data(std::span<const uint8_t> data);
    void write_pending_tcp_data();

    // HTTP/1.1 parsing
    void try_parse_http1_response();

    // HTTP/2 nghttp2 callbacks and integration
    void init_h2_session();
    static int h2_on_header_cb(nghttp2_session* session,
                               const nghttp2_frame* frame,
                               const uint8_t* name, size_t namelen,
                               const uint8_t* value, size_t valuelen,
                               uint8_t flags, void* user_data);
    static int h2_on_data_chunk_recv_cb(nghttp2_session* session, uint8_t flags,
                                        int32_t stream_id, const uint8_t* data,
                                        size_t len, void* user_data);
    static int h2_on_stream_close_cb(nghttp2_session* session, int32_t stream_id,
                                     uint32_t error_code, void* user_data);

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

    // HTTP/3 (QUIC) variables
    std::unique_ptr<net::UdpSocket>              socket_;
    std::unique_ptr<quic::QuicClientConnection>  conn_;
    std::unordered_map<int64_t,
        std::pair<std::optional<http3::ClientResponse>,
                  std::coroutine_handle<>>> pending_;

    // HTTP/1.1 and HTTP/2 (TCP/TLS) variables
    int                                          tcp_fd_ = -1;
    bool                                         tcp_connected_ = false;
    bool                                         tls_handshake_done_ = false;
    uint32_t                                     tcp_event_flags_ = 0;
    std::unique_ptr<net::TlsTcpStream>           tls_stream_;
    std::vector<uint8_t>                         tcp_write_buf_;
    std::string                                  http1_recv_buf_;
    struct nghttp2_session*                      h2_session_ = nullptr;
    std::unordered_map<int32_t, H2PendingStream> h2_pending_;
    std::coroutine_handle<>                      pending_tcp_handle_;
    std::optional<http3::ClientResponse>         tcp_response_;

    // Reconnection state
    int    reconnect_attempts_  = 0;
    bool   reconnect_pending_   = false;
    core::TimerHandle reconnect_timer_;

    std::chrono::milliseconds next_backoff_{100};
    static constexpr std::chrono::milliseconds kMaxBackoff{30000};
};

} // namespace novaboot::client
