#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include "novaboot/core/event_loop.h"
#include "novaboot/memory/arena_allocator.h"
#include "novaboot/net/address.h"
#include "novaboot/net/packet.h"
#include "novaboot/quic/tls_context.h"
#include "novaboot/quic/tls_session.h"

namespace novaboot::http3 {
class Http3Session; // forward declaration
}

namespace novaboot::quic {

/// Callback for sending outgoing packets (called when the connection
/// needs to write data to the network).
using SendPacketCallback =
    std::function<void(const net::OutgoingPacket& packet)>;

/// QUIC connection wrapping ngtcp2_conn for server-side connections.
///
/// Owns:
///   - ngtcp2_conn* (QUIC state machine)
///   - TlsSession (per-connection TLS)
///   - Http3Session (HTTP/3 framing, created after handshake)
///   - ArenaAllocator (per-connection memory)
///
/// Thread-safety: NOT thread-safe. Owned by exactly one Shard.
class QuicConnection {
public:
    /// Maximum number of outgoing packets to batch
    static constexpr int kMaxOutgoingBatch = 64;

    /// Maximum packet buffer size
    static constexpr std::size_t kMaxPktBufSize = 1500;

    ~QuicConnection();

    // Non-copyable, non-movable
    QuicConnection(const QuicConnection&) = delete;
    QuicConnection& operator=(const QuicConnection&) = delete;

    /// Create a new server-side QUIC connection from an incoming initial packet.
    static std::unique_ptr<QuicConnection> create(
        const TlsContext& tls_ctx,
        const ngtcp2_pkt_hd& hd,
        const net::Address& local_addr,
        const net::Address& remote_addr,
        core::EventLoop& event_loop,
        SendPacketCallback send_cb);

    /// Feed an incoming packet into the QUIC connection
    /// Returns 0 on success, negative on error.
    int on_read(const net::IncomingPacket& packet, ngtcp2_tstamp timestamp);

    /// Write any pending outgoing data (responses, acks, etc.)
    /// Returns 0 on success, negative on error.
    int on_write();

    /// Handle timer expiry (retransmission, idle timeout, etc.)
    int handle_expiry(ngtcp2_tstamp timestamp);

    /// Get the ngtcp2 connection object
    [[nodiscard]] ngtcp2_conn* native_handle() const noexcept { return conn_; }

    /// Get the server Connection ID
    [[nodiscard]] const ngtcp2_cid& scid() const noexcept { return scid_; }

    /// Get the remote address
    [[nodiscard]] const net::Address& remote_addr() const noexcept {
        return remote_addr_;
    }

    /// Get the local address
    [[nodiscard]] const net::Address& local_addr() const noexcept {
        return local_addr_;
    }

    /// Get the HTTP/3 session (may be null before handshake completes)
    [[nodiscard]] http3::Http3Session* http3_session() const noexcept {
        return http3_session_.get();
    }

    /// Get the per-connection arena allocator
    [[nodiscard]] memory::ArenaAllocator& arena() noexcept { return arena_; }

    /// Check if the connection is draining (closing)
    [[nodiscard]] bool is_draining() const noexcept;

    /// Check if the connection is closed
    [[nodiscard]] bool is_closed() const noexcept;

    /// Get the next expiry timestamp for this connection
    [[nodiscard]] ngtcp2_tstamp get_expiry() const noexcept;

    /// Get a ngtcp2 timestamp from steady_clock
    static ngtcp2_tstamp timestamp_now() noexcept;

    /// Set the HTTP/3 session (called when handshake completes)
    void set_http3_session(std::unique_ptr<http3::Http3Session> session);

private:
    QuicConnection() = default;

    // ─── ngtcp2 callbacks (static, route to instance via user_data) ──
    static int on_recv_client_initial(ngtcp2_conn* conn,
                                      const ngtcp2_cid* dcid,
                                      void* user_data);
    static int on_recv_crypto_data(ngtcp2_conn* conn,
                                   ngtcp2_encryption_level level,
                                   uint64_t offset,
                                   const uint8_t* data, size_t datalen,
                                   void* user_data);
    static int on_handshake_completed(ngtcp2_conn* conn, void* user_data);
    static int on_recv_stream_data(ngtcp2_conn* conn, uint32_t flags,
                                   int64_t stream_id,
                                   uint64_t offset,
                                   const uint8_t* data, size_t datalen,
                                   void* user_data);
    static int on_stream_open(ngtcp2_conn* conn, int64_t stream_id,
                              void* user_data);
    static int on_stream_close(ngtcp2_conn* conn, uint32_t flags,
                               int64_t stream_id, uint64_t app_error_code,
                               void* user_data);
    static int on_acked_stream_data_offset(ngtcp2_conn* conn,
                                           int64_t stream_id,
                                           uint64_t offset, uint64_t datalen,
                                           void* user_data);
    static int on_get_new_connection_id(ngtcp2_conn* conn,
                                        ngtcp2_cid* cid,
                                        uint8_t* token,
                                        size_t cidlen,
                                        void* user_data);
    static int on_version_negotiation(ngtcp2_conn* conn,
                                      uint32_t version,
                                      const ngtcp2_cid* client_dcid,
                                      void* user_data);
    static int on_extend_max_stream_data(ngtcp2_conn* conn,
                                         int64_t stream_id,
                                         uint64_t max_data,
                                         void* user_data);

    // Instance-level handlers
    int handle_handshake_completed();
    int handle_recv_stream_data(uint32_t flags, int64_t stream_id,
                                const uint8_t* data, size_t datalen);
    int handle_stream_close(int64_t stream_id, uint64_t app_error_code);
    int handle_acked_stream_data(int64_t stream_id, uint64_t offset,
                                 uint64_t datalen);
    int handle_extend_max_stream_data(int64_t stream_id, uint64_t max_data);

    /// Schedule the retransmission/expiry timer
    void update_timer();

    ngtcp2_conn*             conn_       = nullptr;
    ngtcp2_cid               scid_;
    ngtcp2_crypto_conn_ref   conn_ref_;

    TlsSession               tls_session_;
    std::unique_ptr<http3::Http3Session> http3_session_;
    memory::ArenaAllocator   arena_;

    net::Address             local_addr_;
    net::Address             remote_addr_;

    core::EventLoop*         event_loop_  = nullptr;
    SendPacketCallback       send_cb_;
    core::TimerHandle        expiry_timer_;

    /// Packet buffer for writing outgoing packets
    std::vector<std::uint8_t> pkt_buf_;

    bool closed_ = false;
};

} // namespace novaboot::quic
