#include "novaboot/quic/quic_connection.h"
#include "novaboot/http3/http3_session.h"

#include <chrono>
#include <cstring>
#include <format>
#include <random>
#include <stdexcept>

#include <openssl/rand.h>

#include <spdlog/spdlog.h>

namespace novaboot::quic {

namespace {

/// Generate random bytes using OpenSSL
void generate_random(uint8_t* dest, size_t len) {
    RAND_bytes(dest, static_cast<int>(len));
}

/// Get ngtcp2 timestamp (nanoseconds since some epoch)
ngtcp2_tstamp get_timestamp() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<ngtcp2_tstamp>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch())
            .count());
}

} // anonymous namespace

QuicConnection::~QuicConnection() {
    if (event_loop_ && expiry_timer_) {
        event_loop_->cancel_timer(expiry_timer_);
    }
    if (conn_) {
        ngtcp2_conn_del(conn_);
    }
}

std::unique_ptr<QuicConnection> QuicConnection::create(
    const TlsContext& tls_ctx,
    const ngtcp2_pkt_hd& hd,
    const net::Address& local_addr,
    const net::Address& remote_addr,
    core::EventLoop& event_loop,
    SendPacketCallback send_cb) {

    auto qconn = std::unique_ptr<QuicConnection>(new QuicConnection());
    qconn->local_addr_  = local_addr;
    qconn->remote_addr_ = remote_addr;
    qconn->event_loop_  = &event_loop;
    qconn->send_cb_     = std::move(send_cb);
    qconn->pkt_buf_.resize(kMaxPktBufSize);

    // Generate server Connection ID
    qconn->scid_.datalen = NGTCP2_MIN_CIDLEN;
    generate_random(qconn->scid_.data, qconn->scid_.datalen);

    // Setup conn_ref for ngtcp2 ↔ OpenSSL crypto callback routing
    qconn->conn_ref_.get_conn = [](ngtcp2_crypto_conn_ref* ref) {
        auto* self = static_cast<QuicConnection*>(ref->user_data);
        return self->conn_;
    };
    qconn->conn_ref_.user_data = qconn.get();

    // Create TLS session
    qconn->tls_session_ = TlsSession::create_server(tls_ctx, &qconn->conn_ref_);

    // Setup ngtcp2 callbacks
    ngtcp2_callbacks callbacks{};
    ngtcp2_crypto_ngtcp2_callbacks(&callbacks);

    callbacks.recv_client_initial     = on_recv_client_initial;
    callbacks.recv_crypto_data        = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.handshake_completed     = on_handshake_completed;
    callbacks.recv_stream_data        = on_recv_stream_data;
    callbacks.stream_open             = on_stream_open;
    callbacks.stream_close            = on_stream_close;
    callbacks.acked_stream_data_offset = on_acked_stream_data_offset;
    callbacks.get_new_connection_id   = on_get_new_connection_id;
    callbacks.version_negotiation     = on_version_negotiation;
    callbacks.extend_max_stream_data  = on_extend_max_stream_data;
    callbacks.delete_crypto_aead_ctx  = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;

    // Settings
    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = get_timestamp();
    settings.log_printf = nullptr; // TODO: integrate with spdlog

    // Transport parameters
    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_streams_bidi  = 100;
    params.initial_max_streams_uni   = 3; // Control + QPACK enc/dec
    params.initial_max_data          = 1024 * 1024;     // 1MB
    params.initial_max_stream_data_bidi_local  = 256 * 1024; // 256KB
    params.initial_max_stream_data_bidi_remote = 256 * 1024;
    params.initial_max_stream_data_uni         = 256 * 1024;
    params.max_idle_timeout = 30 * NGTCP2_SECONDS;

    // Generate stateless reset token
    generate_random(params.stateless_reset_token,
                    sizeof(params.stateless_reset_token));
    params.stateless_reset_token_present = 1;

    // Setup path
    ngtcp2_path path;
    path.local = ngtcp2_addr{
        const_cast<sockaddr*>(local_addr.sockaddr_ptr()),
        local_addr.sockaddr_len()
    };
    path.remote = ngtcp2_addr{
        const_cast<sockaddr*>(remote_addr.sockaddr_ptr()),
        remote_addr.sockaddr_len()
    };

    // Create the ngtcp2 server connection
    int rv = ngtcp2_conn_server_new(
        &qconn->conn_,
        &hd.dcid,           // Client's DCID becomes our initial SCID
        &qconn->scid_,      // Our chosen SCID
        &path,
        hd.version,
        &callbacks,
        &settings,
        &params,
        nullptr,             // Use default memory allocator
        qconn.get());        // user_data

    if (rv != 0) {
        throw std::runtime_error(
            std::format("ngtcp2_conn_server_new failed: {}",
                        ngtcp2_strerror(rv)));
    }

    // Set TLS native handle (the ngtcp2_crypto_ossl_ctx)
    ngtcp2_conn_set_tls_native_handle(qconn->conn_,
                                      qconn->tls_session_.crypto_ctx());

    spdlog::debug("QUIC connection created: {} ← {}",
                  local_addr.to_string(), remote_addr.to_string());

    return qconn;
}

int QuicConnection::on_read(const net::IncomingPacket& packet,
                            ngtcp2_tstamp timestamp) {
    ngtcp2_path path;
    path.local = ngtcp2_addr{
        const_cast<sockaddr*>(local_addr_.sockaddr_ptr()),
        local_addr_.sockaddr_len()
    };
    path.remote = ngtcp2_addr{
        const_cast<sockaddr*>(remote_addr_.sockaddr_ptr()),
        remote_addr_.sockaddr_len()
    };

    ngtcp2_pkt_info pi{};
    pi.ecn = packet.ecn;

    int rv = ngtcp2_conn_read_pkt(conn_, &path, &pi,
                                  packet.data, packet.size,
                                  timestamp);

    if (rv != 0) {
        spdlog::debug("ngtcp2_conn_read_pkt error: {}", ngtcp2_strerror(rv));

        if (!ngtcp2_err_is_fatal(rv)) {
            // Non-fatal: write a connection close frame
            on_write();
        }
        return rv;
    }

    update_timer();
    return 0;
}

int QuicConnection::on_write() {
    if (is_closed() || is_draining()) {
        return 0;
    }

    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi;

    // Write stream data via HTTP/3 if available
    if (http3_session_) {
        for (;;) {
            int64_t stream_id = -1;
            int fin = 0;
            ngtcp2_vec datav[16];
            size_t datavcnt = 0;

            // Ask nghttp3 what data to send
            auto ngdatavcnt = http3_session_->get_stream_data(
                &stream_id, &fin, datav, 16);

            if (ngdatavcnt < 0) {
                // No more data to send
                break;
            }
            datavcnt = static_cast<size_t>(ngdatavcnt);

            uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
            if (fin) {
                flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
            }

            ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(
                conn_, &ps.path, &pi,
                pkt_buf_.data(), pkt_buf_.size(),
                nullptr, // pdatacnt (written data count)
                flags,
                stream_id,
                datav, datavcnt,
                timestamp_now());

            if (nwrite < 0) {
                if (nwrite == NGTCP2_ERR_WRITE_MORE) {
                    // Tell nghttp3 how much data was consumed
                    // ngtcp2 wants more data in the same packet
                    http3_session_->add_write_offset(stream_id, datavcnt, datav);
                    continue;
                }
                if (nwrite == NGTCP2_ERR_STREAM_NOT_FOUND ||
                    nwrite == NGTCP2_ERR_STREAM_SHUT_WR) {
                    http3_session_->add_write_offset(stream_id, datavcnt, datav);
                    continue;
                }
                spdlog::debug("ngtcp2_conn_writev_stream error: {}",
                              ngtcp2_strerror(static_cast<int>(nwrite)));
                return static_cast<int>(nwrite);
            }

            if (nwrite == 0) {
                // Congestion limited
                break;
            }

            // Tell nghttp3 about written data
            if (stream_id >= 0 && datavcnt > 0) {
                http3_session_->add_write_offset(stream_id, datavcnt, datav);
            }

            // Send the packet
            net::OutgoingPacket out;
            out.data   = pkt_buf_.data();
            out.size   = static_cast<std::size_t>(nwrite);
            out.remote = remote_addr_;
            out.local  = local_addr_;
            out.ecn    = pi.ecn;
            send_cb_(out);
        }
    }

    // Write any remaining non-stream data (ACKs, handshake, etc.)
    for (;;) {
        ngtcp2_ssize nwrite = ngtcp2_conn_write_pkt(
            conn_, &ps.path, &pi,
            pkt_buf_.data(), pkt_buf_.size(),
            timestamp_now());

        if (nwrite < 0) {
            spdlog::debug("ngtcp2_conn_write_pkt error: {}",
                          ngtcp2_strerror(static_cast<int>(nwrite)));
            return static_cast<int>(nwrite);
        }

        if (nwrite == 0) {
            break;
        }

        net::OutgoingPacket out;
        out.data   = pkt_buf_.data();
        out.size   = static_cast<std::size_t>(nwrite);
        out.remote = remote_addr_;
        out.local  = local_addr_;
        out.ecn    = pi.ecn;
        send_cb_(out);
    }

    update_timer();
    return 0;
}

int QuicConnection::handle_expiry(ngtcp2_tstamp timestamp) {
    int rv = ngtcp2_conn_handle_expiry(conn_, timestamp);
    if (rv != 0) {
        spdlog::debug("ngtcp2_conn_handle_expiry error: {}",
                      ngtcp2_strerror(rv));
        return rv;
    }
    return on_write();
}

bool QuicConnection::is_draining() const noexcept {
    return conn_ && ngtcp2_conn_in_draining_period(conn_);
}

bool QuicConnection::is_closed() const noexcept {
    return closed_ || !conn_;
}

ngtcp2_tstamp QuicConnection::get_expiry() const noexcept {
    if (!conn_) return UINT64_MAX;
    return ngtcp2_conn_get_expiry(conn_);
}

ngtcp2_tstamp QuicConnection::timestamp_now() noexcept {
    return get_timestamp();
}

void QuicConnection::set_http3_session(
    std::unique_ptr<http3::Http3Session> session) {
    http3_session_ = std::move(session);
}

void QuicConnection::update_timer() {
    if (!event_loop_ || !conn_) return;

    // Cancel existing timer
    if (expiry_timer_) {
        event_loop_->cancel_timer(expiry_timer_);
    }

    auto expiry = ngtcp2_conn_get_expiry(conn_);
    if (expiry == UINT64_MAX) return;

    auto now_ts = timestamp_now();
    if (expiry <= now_ts) {
        // Already expired — handle immediately on next loop iteration
        expiry_timer_ = event_loop_->add_timer(
            std::chrono::milliseconds(0),
            [this]() { handle_expiry(timestamp_now()); });
        return;
    }

    auto delay_ns = expiry - now_ts;
    auto delay = std::chrono::nanoseconds(delay_ns);
    expiry_timer_ = event_loop_->add_timer(delay, [this]() {
        handle_expiry(timestamp_now());
    });
}

// ─── Static callback implementations ────────────────────────────────────────

int QuicConnection::on_recv_client_initial(
    ngtcp2_conn* conn, const ngtcp2_cid* dcid, void* user_data) {
    // The crypto helper handles Initial key installation
    return ngtcp2_crypto_recv_client_initial_cb(conn, dcid, user_data);
}

int QuicConnection::on_handshake_completed(ngtcp2_conn* /*conn*/,
                                           void* user_data) {
    auto* self = static_cast<QuicConnection*>(user_data);
    return self->handle_handshake_completed();
}

int QuicConnection::handle_handshake_completed() {
    spdlog::debug("QUIC handshake completed: {}", remote_addr_.to_string());

    // Create HTTP/3 session now that the handshake is done
    // This will be called by the Shard which owns the router
    // The actual Http3Session creation is deferred to the Shard
    // because it needs access to the Router.
    return 0;
}

int QuicConnection::on_recv_stream_data(
    ngtcp2_conn* /*conn*/, uint32_t flags,
    int64_t stream_id, uint64_t /*offset*/,
    const uint8_t* data, size_t datalen,
    void* user_data) {
    auto* self = static_cast<QuicConnection*>(user_data);
    return self->handle_recv_stream_data(flags, stream_id, data, datalen);
}

int QuicConnection::handle_recv_stream_data(
    uint32_t flags, int64_t stream_id,
    const uint8_t* data, size_t datalen) {

    if (!http3_session_) {
        return 0; // HTTP/3 session not yet established
    }

    return http3_session_->on_stream_data(stream_id, data, datalen,
                                          flags & NGTCP2_STREAM_DATA_FLAG_FIN);
}

int QuicConnection::on_stream_open(ngtcp2_conn* /*conn*/,
                                   int64_t /*stream_id*/,
                                   void* /*user_data*/) {
    // Stream management is handled by HTTP/3 layer
    return 0;
}

int QuicConnection::on_stream_close(
    ngtcp2_conn* /*conn*/, uint32_t /*flags*/,
    int64_t stream_id, uint64_t app_error_code,
    void* user_data) {
    auto* self = static_cast<QuicConnection*>(user_data);
    return self->handle_stream_close(stream_id, app_error_code);
}

int QuicConnection::handle_stream_close(int64_t stream_id,
                                        uint64_t app_error_code) {
    if (!http3_session_) return 0;
    return http3_session_->on_stream_close(stream_id, app_error_code);
}

int QuicConnection::on_acked_stream_data_offset(
    ngtcp2_conn* /*conn*/,
    int64_t stream_id, uint64_t offset, uint64_t datalen,
    void* user_data) {
    auto* self = static_cast<QuicConnection*>(user_data);
    return self->handle_acked_stream_data(stream_id, offset, datalen);
}

int QuicConnection::handle_acked_stream_data(int64_t stream_id,
                                             uint64_t offset,
                                             uint64_t datalen) {
    if (!http3_session_) return 0;
    return http3_session_->on_acked_stream_data(stream_id, offset, datalen);
}

int QuicConnection::on_get_new_connection_id(
    ngtcp2_conn* /*conn*/,
    ngtcp2_cid* cid, uint8_t* token,
    size_t cidlen, void* /*user_data*/) {

    cid->datalen = cidlen;
    generate_random(cid->data, cidlen);
    generate_random(token, NGTCP2_STATELESS_RESET_TOKENLEN);
    return 0;
}

int QuicConnection::on_version_negotiation(
    ngtcp2_conn* /*conn*/, uint32_t version,
    const ngtcp2_cid* /*client_dcid*/,
    void* /*user_data*/) {
    spdlog::debug("Version negotiation: selected version 0x{:08x}", version);
    return 0;
}

int QuicConnection::on_extend_max_stream_data(
    ngtcp2_conn* /*conn*/, int64_t stream_id,
    uint64_t max_data, void* user_data) {
    auto* self = static_cast<QuicConnection*>(user_data);
    return self->handle_extend_max_stream_data(stream_id, max_data);
}

int QuicConnection::handle_extend_max_stream_data(int64_t stream_id,
                                                   uint64_t /*max_data*/) {
    if (!http3_session_) return 0;
    return http3_session_->on_extend_max_stream_data(stream_id);
}

} // namespace novaboot::quic
