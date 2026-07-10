#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include "novaboot/core/event_loop.h"
#include "novaboot/net/address.h"
#include "novaboot/net/packet.h"
#include "novaboot/quic/tls_context.h"
#include "novaboot/quic/tls_session.h"

namespace novaboot::http3 {
class Http3ClientSession; // forward declaration
} // namespace novaboot::http3

namespace novaboot::quic {

/// Callback for sending outgoing UDP packets from the client socket.
using ClientSendPacketCallback =
    std::function<void(const net::OutgoingPacket& packet)>;

/// Client-side QUIC connection using ngtcp2_conn_client_new.
///
/// Differences from the server QuicConnection:
///   - Initiates the handshake (does not wait for an incoming Initial packet)
///   - No original_dcid requirement
///   - Holds a disconnect callback for reconnect signalling
///
/// Thread-safety: NOT thread-safe. Must be accessed only from the EventLoop thread.
class QuicClientConnection {
public:
    static constexpr std::size_t kMaxPktBufSize = 1500;

    ~QuicClientConnection();

    QuicClientConnection(const QuicClientConnection&) = delete;
    QuicClientConnection& operator=(const QuicClientConnection&) = delete;

    /// Create a new client-side QUIC connection.
    ///
    /// @param host        Remote hostname (used for SNI)
    /// @param remote_addr Resolved remote address
    /// @param local_addr  Local UDP address (e.g. 0.0.0.0:0)
    /// @param tls_ctx     Client TLS context
    /// @param event_loop  The shared shard event loop
    /// @param send_cb     Callback to push outgoing UDP packets
    static std::unique_ptr<QuicClientConnection> create(
        const std::string&            host,
        const net::Address&           remote_addr,
        const net::Address&           local_addr,
        const TlsContext&             tls_ctx,
        core::EventLoop&              event_loop,
        ClientSendPacketCallback      send_cb);

    /// Feed an incoming UDP packet into the connection
    int on_read(const net::IncomingPacket& packet, ngtcp2_tstamp timestamp);

    /// Write any pending data (handshake, ACKs, responses)
    int on_write();

    /// Handle timer expiry (retransmission / idle timeout)
    int handle_expiry(ngtcp2_tstamp timestamp);

    /// Get the ngtcp2 connection object
    [[nodiscard]] ngtcp2_conn* native_handle() const noexcept { return conn_; }

    /// True once the QUIC + TLS handshake has completed
    [[nodiscard]] bool is_handshake_complete() const noexcept { return handshake_done_; }

    [[nodiscard]] bool is_draining() const noexcept;
    [[nodiscard]] bool is_closed()   const noexcept;

    void close(uint64_t app_error_code = 0);

    [[nodiscard]] ngtcp2_tstamp get_expiry() const noexcept;
    static ngtcp2_tstamp        timestamp_now() noexcept;

    void set_http3_session(std::unique_ptr<http3::Http3ClientSession> session);
    [[nodiscard]] http3::Http3ClientSession* http3_session() const noexcept {
        return http3_session_.get();
    }

    /// Callback fired once QUIC handshake completes — create HTTP/3 session here.
    void set_handshake_callback(std::function<void(QuicClientConnection&)> cb) {
        handshake_cb_ = std::move(cb);
    }

    /// Callback fired when the connection is closed unexpectedly.
    void set_disconnect_callback(std::function<void()> cb) {
        disconnect_cb_ = std::move(cb);
    }

    [[nodiscard]] const net::Address& remote_addr() const noexcept { return remote_addr_; }
    [[nodiscard]] const net::Address& local_addr()  const noexcept { return local_addr_; }

private:
    QuicClientConnection() = default;

    // ─── ngtcp2 callbacks ────────────────────────────────────────────
    static int on_recv_crypto_data(ngtcp2_conn*, ngtcp2_encryption_level,
                                   uint64_t, const uint8_t*, size_t, void*);
    static int on_handshake_completed(ngtcp2_conn*, void*);
    static int on_recv_stream_data(ngtcp2_conn*, uint32_t, int64_t,
                                   uint64_t, const uint8_t*, size_t,
                                   void*, void*);
    static int on_stream_open(ngtcp2_conn*, int64_t, void*);
    static int on_stream_close(ngtcp2_conn*, uint32_t, int64_t, uint64_t,
                               void*, void*);
    static int on_acked_stream_data_offset(ngtcp2_conn*, int64_t, uint64_t,
                                           uint64_t, void*, void*);
    static int on_get_new_connection_id(ngtcp2_conn*, ngtcp2_cid*, uint8_t*,
                                        size_t, void*);
    static int on_remove_connection_id(ngtcp2_conn*, const ngtcp2_cid*, void*);
    static int on_extend_max_stream_data(ngtcp2_conn*, int64_t, uint64_t,
                                         void*, void*);
    static void on_rand(uint8_t*, size_t, const ngtcp2_rand_ctx*);

    // Instance-level handlers
    int handle_handshake_completed();
    int handle_recv_stream_data(uint32_t flags, int64_t stream_id,
                                uint64_t offset, const uint8_t* data,
                                size_t datalen);
    int handle_stream_close(int64_t stream_id, uint64_t app_error_code);
    int handle_acked_stream_data(int64_t stream_id, uint64_t offset,
                                 uint64_t datalen);
    int handle_extend_max_stream_data(int64_t stream_id, uint64_t max_data);

    void update_timer();
    void signal_disconnect();

    ngtcp2_conn*           conn_ = nullptr;
    ngtcp2_cid             scid_;
    ngtcp2_crypto_conn_ref conn_ref_;

    TlsSession             tls_session_;
    std::unique_ptr<http3::Http3ClientSession> http3_session_;

    net::Address           local_addr_;
    net::Address           remote_addr_;

    core::EventLoop*       event_loop_ = nullptr;
    ClientSendPacketCallback send_cb_;
    core::TimerHandle      expiry_timer_;

    std::vector<std::uint8_t> pkt_buf_;

    std::function<void(QuicClientConnection&)> handshake_cb_;
    std::function<void()>                      disconnect_cb_;

    bool handshake_done_ = false;
    bool closed_         = false;
    bool in_error_state_ = false;
};

} // namespace novaboot::quic
